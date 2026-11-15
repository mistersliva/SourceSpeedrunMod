//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include "basefilesystem.h"
#include "packfile.h"

#ifdef _WIN32
#include "tier0/tslist.h"
#elif defined(POSIX)
#include <fcntl.h>

#ifdef LINUX
#include <sys/file.h>
#endif
#endif

#include "tier0/dbg.h"
#include "tier0/threadtools.h"
#include "tier0/vcrmode.h"
#include "tier0/vprof.h"
#include "tier1/convar.h"
#include "tier1/fmtstr.h"
#include "tier1/utlrbtree.h"
#include "vstdlib/osversion.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
// SourceSpeedrunMod: I/O accounting for load-time experiments (fs_iostats).
//-----------------------------------------------------------------------------
#include <atomic>
namespace
{
	struct IoStat_t
	{
		std::atomic<int64> us{0};
		std::atomic<int64> count{0};
		std::atomic<int64> fails{0};
		std::atomic<int64> bytes{0};
	};
	IoStat_t s_IoOpen, s_IoRead, s_IoStat, s_IoFind, s_IoSkip;

	struct IoTimer_t
	{
		IoStat_t &stat;
		const double start;
		explicit IoTimer_t( IoStat_t &s ) : stat( s ), start( Plat_FloatTime() ) {}
		~IoTimer_t() { stat.us += static_cast<int64>( ( Plat_FloatTime() - start ) * 1e6 ); ++stat.count; }
	};
}

static ConVar fs_iostats_spew( "fs_iostats_spew", "0", 0, "Print every low-level file open (SourceSpeedrunMod diagnostics)." );

CON_COMMAND( fs_iostats, "Print and reset file I/O time spent since the last call (SourceSpeedrunMod)." )
{
	auto line = []( const char *name, IoStat_t &st ) {
		Msg( "  %-6s %7lld calls  %7lld failed  %8.1f ms  %8.1f MB\n", name,
			(long long)st.count.load(), (long long)st.fails.load(), st.us.load() / 1000.0, st.bytes.load() / ( 1024.0 * 1024.0 ) );
		st.us = 0; st.count = 0; st.fails = 0; st.bytes = 0;
	};
	Msg( "fs_iostats:\n" );
	line( "open", s_IoOpen );
	line( "read", s_IoRead );
	line( "stat", s_IoStat );
	line( "find", s_IoFind );
	line( "skip", s_IoSkip );
}

//-----------------------------------------------------------------------------
// SourceSpeedrunMod: loose-file directory cache.
//
// Every asset lookup tries the loose search paths (mod folder, custom, the HL2
// install) before finding the file inside a VPK. Each failed CreateFile costs
// 0.1-0.5 ms on Windows (antivirus filters see every open), which on a modern
// NVMe drive is far more than reading the data: ~1400 failed opens per new map,
// 6000+ at startup. Asset folders are listed once instead, and a lookup for a
// file that is not in the listing fails without touching the disk.
//
// Only asset folders are cached (the engine never writes there); writes,
// renames, removals and new folders made through the filesystem drop the
// affected entries anyway.
//-----------------------------------------------------------------------------
#ifdef _WIN32
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

static ConVar fs_loose_dir_cache( "fs_loose_dir_cache", "1", FCVAR_ARCHIVE,
	"Remember asset folder listings so lookups of files that only exist in VPKs skip the disk." );

namespace
{
	struct LooseDir_t
	{
		bool exists = false;
		std::unordered_set< std::string > names;	// lowercase file and folder names
	};

	std::shared_mutex s_LooseDirMutex;
	std::unordered_map< std::string, LooseDir_t > s_LooseDirs;	// key: lowercase folder path

	void LowerSlashes( const char *pIn, std::string &out )
	{
		out.clear();
		for ( const char *p = pIn; *p; ++p )
		{
			char c = *p;
			if ( c == '/' )
				c = '\\';
			else if ( c >= 'A' && c <= 'Z' )
				c = static_cast<char>( c - 'A' + 'a' );
			out.push_back( c );
		}
	}

	// Folders whose contents only change when content is installed.
	bool IsAssetPath( const std::string &path )
	{
		static const char *const kSkip[] = { "\\save\\", "\\cache\\", "\\cfg\\", "\\screenshots\\", "\\downloadlists\\",
			"\\demos\\", "\\splits\\", "\\graphs\\", "\\logs\\" };
		for ( const char *skip : kSkip )
		{
			if ( path.find( skip ) != std::string::npos )
				return false;
		}
		static const char *const kAssets[] = { "\\maps\\", "\\materials\\", "\\models\\", "\\sound\\", "\\particles\\", "\\scenes\\",
			"\\resource\\", "\\scripts\\", "\\media\\", "\\expressions\\" };
		for ( const char *asset : kAssets )
		{
			if ( path.find( asset ) != std::string::npos )
				return true;
		}
		return false;
	}

	void ListDir( const std::string &dir, LooseDir_t &out )
	{
		WIN32_FIND_DATAA data;
		HANDLE h = ::FindFirstFileExA( ( dir + "\\*" ).c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH );
		if ( h == INVALID_HANDLE_VALUE )
		{
			out.exists = false;
			return;
		}
		out.exists = true;
		std::string name;
		do
		{
			LowerSlashes( data.cFileName, name );
			if ( name != "." && name != ".." )
				out.names.insert( name );
		} while ( ::FindNextFileA( h, &data ) );
		::FindClose( h );
	}
}

// False only when the file is known not to exist.
static bool LooseFile_MayExist( const char *pFullPath )
{
	if ( !fs_loose_dir_cache.GetBool() )
		return true;

	std::string path;
	LowerSlashes( pFullPath, path );
	if ( !IsAssetPath( path ) )
		return true;

	const size_t slash = path.find_last_of( '\\' );
	if ( slash == std::string::npos || slash == 0 )
		return true;
	const std::string dir = path.substr( 0, slash );
	const std::string name = path.substr( slash + 1 );

	{
		std::shared_lock lock( s_LooseDirMutex );
		auto it = s_LooseDirs.find( dir );
		if ( it != s_LooseDirs.end() )
			return it->second.exists && it->second.names.count( name ) != 0;
	}

	LooseDir_t listing;
	ListDir( dir, listing );
	const bool bExists = listing.exists && listing.names.count( name ) != 0;
	std::unique_lock lock( s_LooseDirMutex );
	s_LooseDirs.emplace( dir, std::move( listing ) );
	return bExists;
}

static void MemFile_Invalidate( const std::string &lowerPath );

// Something changed at this path: forget its folder and every parent listing.
void LooseFile_Invalidate( const char *pFullPath )
{
	if ( !pFullPath || !pFullPath[0] )
		return;
	std::string path;
	LowerSlashes( pFullPath, path );
	MemFile_Invalidate( path );
	std::unique_lock lock( s_LooseDirMutex );
	if ( s_LooseDirs.empty() )
		return;
	s_LooseDirs.erase( path );	// in case it is a folder
	for ( size_t slash = path.find_last_of( '\\' ); slash != std::string::npos && slash > 0; slash = path.find_last_of( '\\', slash - 1 ) )
		s_LooseDirs.erase( path.substr( 0, slash ) );
}

CON_COMMAND( fs_loose_dir_cache_flush, "Forget the cached asset folder listings." )
{
	std::unique_lock lock( s_LooseDirMutex );
	Msg( "Dropped %zu cached folder listings.\n", s_LooseDirs.size() );
	s_LooseDirs.clear();
}
#else
static bool LooseFile_MayExist( const char * ) { return true; }
void LooseFile_Invalidate( const char * ) {}
#endif

static_assert( SEEK_CUR == FILESYSTEM_SEEK_CURRENT );
static_assert( SEEK_SET == FILESYSTEM_SEEK_HEAD );
static_assert( SEEK_END == FILESYSTEM_SEEK_TAIL );

//-----------------------------------------------------------------------------

class CFileSystem_Stdio : public CBaseFileSystem
{
public:
	CFileSystem_Stdio();
	~CFileSystem_Stdio();

	// Used to get at older versions
	void *QueryInterface( const char *pInterfaceName ) override;

	// Higher level filesystem methods requiring specific behavior
	void GetLocalCopy( const char *pFileName ) override;
	int	HintResourceNeed( const char *hintlist, int forgetEverything ) override;
	bool IsFileImmediatelyAvailable(const char *pFileName) override;
	WaitForResourcesHandle_t WaitForResources( const char *resourcelist ) override;
	bool GetWaitForResourcesProgress( WaitForResourcesHandle_t handle, float *progress /* out */ , bool *complete /* out */ ) override;
	void CancelWaitForResources( WaitForResourcesHandle_t handle ) override;
	bool IsSteam() const override { return false; }
	FilesystemMountRetval_t MountSteamContent( int nExtraAppId = -1 ) override { return FILESYSTEM_MOUNT_OK; }

	bool GetOptimalIOConstraints( FileHandle_t hFile, unsigned *pOffsetAlign, unsigned *pSizeAlign, unsigned *pBufferAlign ) override;
	void *AllocOptimalReadBuffer( FileHandle_t hFile, unsigned nSize, unsigned nOffset ) override;
	void FreeOptimalReadBuffer( void *p ) override;

protected:
	// implementation of CBaseFileSystem virtual functions
	FILE *FS_fopen( const char *filename, const char *options, unsigned flags, int64 *size ) override;
	void FS_setbufsize( FILE *fp, unsigned nBytes ) override;
	void FS_fclose( FILE *fp ) override;
	// dimhotepus: FS_fseek now returns offset.
	int FS_fseek( FILE *fp, int64 pos, int seekType ) override;
	long FS_ftell( FILE *fp ) override;
	int FS_feof( FILE *fp ) override;
	size_t FS_fread( OUT_BYTECAP(destSize) void *dest, size_t destSize, size_t size, FILE *fp ) override;
	size_t FS_fwrite( IN_BYTECAP(size) const void *src, size_t size, FILE *fp ) override;
	bool FS_setmode( FILE *fp, FileMode_t mode ) override;
	size_t FS_vfprintf( FILE *fp, const char *fmt, va_list list ) override;
	int FS_ferror( FILE *fp ) override;
	int FS_fflush( FILE *fp ) override;
	char *FS_fgets( OUT_Z_CAP(destSize) char *dest, int destSize, FILE *fp ) override;
	int FS_stat( const char *path, struct _stat *buf, bool *pbLoadedFromSteamCache=NULL  ) override;
	int FS_chmod( const char *path, int pmode ) override;
	HANDLE FS_FindFirstFile(const char *findname, WIN32_FIND_DATA *dat) override;
	bool FS_FindNextFile(HANDLE handle, WIN32_FIND_DATA *dat) override;
	bool FS_FindClose(HANDLE handle) override;
	int FS_GetSectorSize( FILE * ) override;

private:
	bool CanAsync() const
	{
		return m_bCanAsync;
	}

	bool m_bMounted;
	bool m_bCanAsync;
};


//-----------------------------------------------------------------------------
// Per-file worker classes
//-----------------------------------------------------------------------------
abstract_class CStdFilesystemFile
{
public:
	virtual ~CStdFilesystemFile() {}
	virtual void FS_setbufsize( unsigned nBytes ) = 0;
	virtual void FS_fclose() = 0;
	// dimhotepus: FS_fseek now returns offset.
	virtual int FS_fseek( int64 pos, int seekType ) = 0;
	virtual long FS_ftell() = 0;
	virtual int FS_feof() = 0;
	virtual size_t FS_fread( OUT_BYTECAP(destSize) void *dest, size_t destSize, size_t size ) = 0;
	virtual size_t FS_fwrite( IN_BYTECAP(size) const void *src, size_t size ) = 0;
	virtual bool FS_setmode( FileMode_t mode ) = 0;
	virtual size_t FS_vfprintf( const char *fmt, va_list list ) = 0;
	virtual int FS_ferror() = 0;
	virtual int FS_fflush() = 0;
	virtual char *FS_fgets( OUT_Z_CAP(destSize) char *dest, int destSize ) = 0;
	virtual int FS_GetSectorSize() { return 1; }
};

//---------------------------------------------------------

class CStdioFile : public CStdFilesystemFile
{
public:
	static CStdioFile *FS_fopen( const char *filename, const char *options, int64 *size );

	void FS_setbufsize( unsigned nBytes ) override;
	void FS_fclose() override;
	// dimhotepus: FS_fseek now returns offset.
	int FS_fseek( int64 pos, int seekType ) override;
	long FS_ftell() override;
	int FS_feof() override;
	size_t FS_fread( OUT_BYTECAP(destSize) void *dest, size_t destSize, size_t size) override;
	size_t FS_fwrite( IN_BYTECAP(size) const void *src, size_t size ) override;
	bool FS_setmode( FileMode_t mode ) override;
	size_t FS_vfprintf( const char *fmt, va_list list ) override;
	int FS_ferror() override;
	int FS_fflush() override;
	char *FS_fgets( OUT_Z_CAP(destSize) char *dest, int destSize ) override;

#ifdef POSIX
	static CUtlMap< ino_t, CThreadMutex * > m_LockedFDMap;
	static CThreadMutex	m_MutexLockedFD;
#endif
private:
	CStdioFile( FILE *pFile, bool bWriteable )
		: m_pFile( pFile )
#ifdef POSIX
		, m_bWriteable( bWriteable )
#endif
	{
	}

	FILE *m_pFile;

#ifdef POSIX
	bool m_bWriteable;
#endif
};

#ifdef POSIX
CUtlMap< ino_t, CThreadMutex * > CStdioFile::m_LockedFDMap;
CThreadMutex	CStdioFile::m_MutexLockedFD;
#endif

//-----------------------------------------------------------------------------

#ifdef _WIN32
class CWin32ReadOnlyFile : public CStdFilesystemFile
{
public:
	static bool CanOpen( const char *filename, const char *options );
	static CWin32ReadOnlyFile *FS_fopen( const char *filename, const char *options, int64 *size );

	void FS_setbufsize( unsigned nBytes ) override {}
	void FS_fclose() override;
	// dimhotepus: FS_fseek now returns offset.
	int FS_fseek( int64 pos, int seekType ) override;
	long FS_ftell() override;
	int FS_feof() override;
	size_t FS_fread( OUT_BYTECAP(destSize) void *dest, size_t destSize, size_t size) override;
	size_t FS_fwrite( IN_BYTECAP(size) const void *src, size_t size ) override { return 0; }
	bool FS_setmode( FileMode_t mode ) override { Error( "Can't set mode, open a second file in right mode\n" ); return false; }
	size_t FS_vfprintf( const char *fmt, va_list list ) override { return 0; }
	int FS_ferror() override { return 0;	}
	int FS_fflush() override { return 0; }
	char *FS_fgets( OUT_Z_CAP(destSize) char *dest, int destSize ) override;
	int FS_GetSectorSize() override { return m_SectorSize; }

private:
	CWin32ReadOnlyFile( HANDLE hFileUnbuffered, HANDLE hFileBuffered, int sectorSize, int64 fileSize, bool bOverlapped )
	 :	m_ReadPos( 0 ),
		m_Size( fileSize ),
		m_hFileUnbuffered( hFileUnbuffered ),
		m_hFileBuffered( hFileBuffered ),
		m_SectorSize( sectorSize ),
		m_bOverlapped( bOverlapped )
	{
	}

	int64				m_ReadPos;
	int64				m_Size;
	HANDLE				m_hFileUnbuffered;
	HANDLE				m_hFileBuffered;
	CThreadFastMutex	m_Mutex;
	int					m_SectorSize;
	bool				m_bOverlapped;
};

#endif

//-----------------------------------------------------------------------------
// singleton
//-----------------------------------------------------------------------------
CFileSystem_Stdio g_FileSystem_Stdio;
#if defined(_WIN32) && defined(DEDICATED)
CBaseFileSystem *BaseFileSystem_Stdio()
{
	return &g_FileSystem_Stdio;
}
#endif
 
#ifdef DEDICATED // "hack" to allow us to not export a stdio version of the FILESYSTEM_INTERFACE_VERSION anywhere

IFileSystem *g_pFileSystem = &g_FileSystem_Stdio;
IBaseFileSystem *g_pBaseFileSystem = &g_FileSystem_Stdio;

#else

EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CFileSystem_Stdio, IFileSystem, FILESYSTEM_INTERFACE_VERSION, g_FileSystem_Stdio );
EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CFileSystem_Stdio, IBaseFileSystem, BASEFILESYSTEM_INTERFACE_VERSION, g_FileSystem_Stdio );

#endif

//-----------------------------------------------------------------------------

#ifndef _RETAIL
bool UseOptimalBufferAllocation()
{
	static bool bUseOptimalBufferAllocation = ( !IsLinux() && Q_stristr( Plat_GetCommandLine(), "-unbuffered_io" ) != NULL );
	return bUseOptimalBufferAllocation;
}
ConVar filesystem_unbuffered_io( "filesystem_unbuffered_io", "1", 0, "" );
#define UseUnbufferedIO() ( UseOptimalBufferAllocation() && filesystem_unbuffered_io.GetBool() )
#else
#define UseUnbufferedIO() true
#endif

ConVar filesystem_native( "filesystem_native", "1", 0, "Use native FS or STDIO" );
// dimhotepus: 16 -> 64 on PCs.
ConVar filesystem_max_stdio_read( "filesystem_max_stdio_read", "64", 0, "Maximum chunk size to read in MiB" );
ConVar filesystem_report_buffered_io( "filesystem_report_buffered_io", "0" );

//-----------------------------------------------------------------------------
// constructor
//-----------------------------------------------------------------------------
CFileSystem_Stdio::CFileSystem_Stdio()
{
	m_bMounted = false;
	m_bCanAsync = true;
#ifdef POSIX
	SetDefLessFunc( CStdioFile::m_LockedFDMap );
#endif
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CFileSystem_Stdio::~CFileSystem_Stdio()
{
#ifdef POSIX
	FOR_EACH_MAP_FAST( CStdioFile::m_LockedFDMap, i )
	{
		Assert( CStdioFile::m_LockedFDMap[ i ] );
		delete CStdioFile::m_LockedFDMap[ i ];
	}
	CStdioFile::m_LockedFDMap.RemoveAll();
#endif

	Assert(!m_bMounted);
}


//-----------------------------------------------------------------------------
// QueryInterface: 
//-----------------------------------------------------------------------------
void *CFileSystem_Stdio::QueryInterface( const char *pInterfaceName )
{
	// We also implement the IMatSystemSurface interface
	if (!Q_strncmp(	pInterfaceName, FILESYSTEM_INTERFACE_VERSION, ssize(FILESYSTEM_INTERFACE_VERSION) ))
		return (IFileSystem*)this;

	return CBaseFileSystem::QueryInterface( pInterfaceName );
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
bool CFileSystem_Stdio::GetOptimalIOConstraints( FileHandle_t hFile, unsigned *pOffsetAlign, unsigned *pSizeAlign, unsigned *pBufferAlign )
{
	unsigned sectorSize;
	
	if ( hFile && UseOptimalBufferAllocation() )
	{
		auto *fh = static_cast<CFileHandle *>(hFile);
		sectorSize = fh->GetSectorSize();

		if ( !sectorSize || ( fh->m_pPackFileHandle && ( fh->m_pPackFileHandle->AbsoluteBaseOffset() % sectorSize ) ) )
		{
			sectorSize = 1;
		}
	}
	else
	{
		sectorSize = 1;
	}

	if ( pOffsetAlign )
	{
		*pOffsetAlign = sectorSize;
	}

	if ( pSizeAlign )
	{
		*pSizeAlign = sectorSize;
	}

	if ( pBufferAlign )
	{
		*pBufferAlign = sectorSize;
	}

	return ( sectorSize > 1 );
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void *CFileSystem_Stdio::AllocOptimalReadBuffer( FileHandle_t hFile, unsigned nSize, unsigned nOffset )
{
	if ( !UseOptimalBufferAllocation() )
	{
		return CBaseFileSystem::AllocOptimalReadBuffer( hFile, nSize, nOffset );
	}

	unsigned sectorSize;
	if ( hFile != FILESYSTEM_INVALID_HANDLE )
	{
		auto *fh = static_cast<CFileHandle *>(hFile);
		sectorSize = fh->GetSectorSize();

		if ( !nSize ) //-V1051
		{
			nSize = fh->Size();
		}

		if ( fh->m_pPackFileHandle )
		{
			nOffset += fh->m_pPackFileHandle->AbsoluteBaseOffset();
		}
	}
	else
	{
		// an invalid handle gets a fake "optimal" but valid buffer
		// this path is for a caller that isn't doing i/o, 
		// but needs an "optimal" buffer that can end up passed to FreeOptimalReadBuffer()
		sectorSize = 4;
	}

	bool bOffsetIsAligned = ( nOffset % sectorSize == 0 );
	unsigned nAllocSize = ( bOffsetIsAligned ) ? AlignValue( nSize, sectorSize ) : nSize;

	unsigned nAllocAlignment = ( bOffsetIsAligned ) ? sectorSize : 4;
	return _aligned_malloc( nAllocSize, nAllocAlignment );
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void CFileSystem_Stdio::FreeOptimalReadBuffer( void *p )
{
	if ( !UseOptimalBufferAllocation() )
	{
		CBaseFileSystem::FreeOptimalReadBuffer( p );
		return;
	}

	_aligned_free( p );
}

//-----------------------------------------------------------------------------
// SourceSpeedrunMod: in-memory cache of loose files.
//
// Loading a level reopens the same loose files every time (the map's .bsp
// several times, scripts, resource files). Each open is a CreateFile through
// the antivirus filter and whatever file system driver the game folder sits
// on, which costs far more than the read itself on an SSD. A file opened for
// reading is kept in memory and later opens are served from there.
//-----------------------------------------------------------------------------
#ifdef _WIN32
#include <list>
#include <memory>
#include <vector>

static ConVar fs_file_mem_cache( "fs_file_mem_cache", "1", FCVAR_ARCHIVE,
	"Keep loose game files in memory after the first open (maps, scripts) so reloads skip the disk." );
static ConVar fs_file_mem_cache_mb( "fs_file_mem_cache_mb", "768", FCVAR_ARCHIVE,
	"Memory limit of fs_file_mem_cache, in MB.", true, 64, true, 8192 );

namespace
{
	using FileBytes = std::shared_ptr< const std::vector< char > >;

	class CMemFsFile final : public CStdFilesystemFile
	{
	public:
		explicit CMemFsFile( FileBytes data ) : m_Data( std::move( data ) ) {}

		void FS_setbufsize( unsigned ) override {}
		void FS_fclose() override { m_Data.reset(); }
		int FS_fseek( int64 pos, int seekType ) override
		{
			int64 base = 0;
			if ( seekType == SEEK_CUR )
				base = m_nPos;
			else if ( seekType == SEEK_END )
				base = Size();
			const int64 target = base + pos;
			if ( target < 0 )
				return -1;
			m_nPos = target;
			m_bEof = false;
			return static_cast<int>( m_nPos );
		}
		long FS_ftell() override { return static_cast<long>( m_nPos ); }
		int FS_feof() override { return m_bEof ? 1 : 0; }
		size_t FS_fread( OUT_BYTECAP(destSize) void *dest, size_t destSize, size_t size ) override
		{
			const int64 avail = Size() - m_nPos;
			size_t n = size;
			if ( avail <= 0 )
				n = 0;
			else if ( static_cast<int64>( n ) > avail )
				n = static_cast<size_t>( avail );
			if ( n > destSize )
				n = destSize;
			if ( n )
				memcpy( dest, m_Data->data() + m_nPos, n );
			m_nPos += static_cast<int64>( n );
			if ( n < size )
				m_bEof = true;
			return n;
		}
		size_t FS_fwrite( IN_BYTECAP(size) const void *, size_t ) override { return 0; }
		bool FS_setmode( FileMode_t ) override { return true; }
		size_t FS_vfprintf( const char *, va_list ) override { return 0; }
		int FS_ferror() override { return 0; }
		int FS_fflush() override { return 0; }
		char *FS_fgets( OUT_Z_CAP(destSize) char *dest, int destSize ) override
		{
			if ( destSize <= 0 )
				return nullptr;
			const int64 size = Size();
			if ( m_nPos >= size )
			{
				m_bEof = true;
				return nullptr;
			}
			int i = 0;
			while ( i < destSize - 1 && m_nPos < size )
			{
				const char c = ( *m_Data )[ static_cast<size_t>( m_nPos++ ) ];
				dest[i++] = c;
				if ( c == '\n' )
					break;
			}
			dest[i] = '\0';
			return dest;
		}

	private:
		int64 Size() const { return m_Data ? static_cast<int64>( m_Data->size() ) : 0; }

		FileBytes m_Data;
		int64 m_nPos = 0;
		bool m_bEof = false;
	};

	struct MemEntry_t
	{
		FileBytes data;
		std::list< std::string >::iterator lru;
	};

	std::mutex s_MemMutex;
	std::unordered_map< std::string, MemEntry_t > s_MemFiles;
	std::list< std::string > s_MemLru;	// front = most recently used
	size_t s_nMemBytes = 0;

	bool IsMemCacheable( const std::string &path )
	{
		static const char *const kSkip[] = { "\\save\\", "\\cache\\", "\\cfg\\", "\\screenshots\\", "\\downloadlists\\",
			"\\demos\\", "\\splits\\", "\\graphs\\", "\\logs\\", "\\bin\\", "\\htmlui\\" };
		for ( const char *skip : kSkip )
		{
			if ( path.find( skip ) != std::string::npos )
				return false;
		}
		const size_t dot = path.find_last_of( '.' );
		if ( dot == std::string::npos )
			return false;
		const std::string ext = path.substr( dot );
		return ext != ".vpk" && ext != ".dll" && ext != ".exe" && ext != ".log" && ext != ".sav" && ext != ".cfg";
	}
}

static void MemFile_Invalidate( const std::string &lowerPath )
{
	std::lock_guard lock( s_MemMutex );
	auto it = s_MemFiles.find( lowerPath );
	if ( it == s_MemFiles.end() )
		return;
	s_nMemBytes -= it->second.data->size();
	s_MemLru.erase( it->second.lru );
	s_MemFiles.erase( it );
}

enum class MemOpen_t { Opened, Missing, Fallback };

// Serves a read-only open from memory, reading the whole file the first time.
static MemOpen_t MemFile_Open( const char *pFullPath, CStdFilesystemFile **ppFile, int64 *pSize )
{
	if ( !fs_file_mem_cache.GetBool() )
		return MemOpen_t::Fallback;

	std::string key;
	LowerSlashes( pFullPath, key );
	if ( !IsMemCacheable( key ) )
		return MemOpen_t::Fallback;

	{
		std::lock_guard lock( s_MemMutex );
		auto it = s_MemFiles.find( key );
		if ( it != s_MemFiles.end() )
		{
			s_MemLru.splice( s_MemLru.begin(), s_MemLru, it->second.lru );
			*ppFile = new CMemFsFile( it->second.data );
			if ( pSize )
				*pSize = static_cast<int64>( it->second.data->size() );
			return MemOpen_t::Opened;
		}
	}

	HANDLE h = ::CreateFileA( pFullPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr );
	if ( h == INVALID_HANDLE_VALUE )
	{
		const DWORD err = ::GetLastError();
		return ( err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ) ? MemOpen_t::Missing : MemOpen_t::Fallback;
	}

	const size_t limit = static_cast<size_t>( fs_file_mem_cache_mb.GetInt() ) * 1024 * 1024;
	LARGE_INTEGER fileSize;
	if ( !::GetFileSizeEx( h, &fileSize ) || fileSize.QuadPart > 128ll * 1024 * 1024 || static_cast<size_t>( fileSize.QuadPart ) > limit / 4 )
	{
		::CloseHandle( h );
		return MemOpen_t::Fallback;
	}

	auto bytes = std::make_shared< std::vector< char > >( static_cast<size_t>( fileSize.QuadPart ) );
	size_t done = 0;
	while ( done < bytes->size() )
	{
		DWORD chunk = 0;
		const DWORD want = static_cast<DWORD>( std::min< size_t >( bytes->size() - done, 64u * 1024 * 1024 ) );
		if ( !::ReadFile( h, bytes->data() + done, want, &chunk, nullptr ) || chunk == 0 )
			break;
		done += chunk;
	}
	::CloseHandle( h );
	if ( done != bytes->size() )
		return MemOpen_t::Fallback;

	FileBytes data = std::move( bytes );
	{
		std::lock_guard lock( s_MemMutex );
		if ( s_MemFiles.find( key ) == s_MemFiles.end() )
		{
			s_MemLru.push_front( key );
			s_MemFiles.emplace( key, MemEntry_t{ data, s_MemLru.begin() } );
			s_nMemBytes += data->size();
			while ( s_nMemBytes > limit && s_MemLru.size() > 1 )
			{
				auto victim = s_MemFiles.find( s_MemLru.back() );
				s_nMemBytes -= victim->second.data->size();
				s_MemFiles.erase( victim );
				s_MemLru.pop_back();
			}
		}
	}

	*ppFile = new CMemFsFile( data );
	if ( pSize )
		*pSize = static_cast<int64>( data->size() );
	return MemOpen_t::Opened;
}

CON_COMMAND( fs_file_mem_cache_info, "Show how many files fs_file_mem_cache holds." )
{
	std::lock_guard lock( s_MemMutex );
	Msg( "fs_file_mem_cache: %zu files, %.1f MB\n", s_MemFiles.size(), s_nMemBytes / ( 1024.0 * 1024.0 ) );
}
#endif

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
FILE *CFileSystem_Stdio::FS_fopen( const char *filenameT, const char *options, unsigned flags, int64 *size )
{
	IoTimer_t ioTimer( s_IoOpen );
	char filename[ MAX_PATH ];
	CBaseFileSystem::FixUpPath ( filenameT, filename );

	if ( strpbrk( options, "wa+" ) )
		LooseFile_Invalidate( filename );
	else if ( !LooseFile_MayExist( filename ) )
	{
		++s_IoOpen.fails;
		++s_IoSkip.count;
		return nullptr;
	}

	alignas(FILE *) CStdFilesystemFile *pFile = nullptr;

#ifdef _WIN32
	if ( !strpbrk( options, "wa+" ) )
	{
		const MemOpen_t mem = MemFile_Open( filename, &pFile, size );
		if ( mem == MemOpen_t::Opened )
			return reinterpret_cast<FILE *>(pFile);
		if ( mem == MemOpen_t::Missing )
		{
			++s_IoOpen.fails;
			return nullptr;
		}
	}

	if ( CWin32ReadOnlyFile::CanOpen( filename, options ) )
	{
		pFile = CWin32ReadOnlyFile::FS_fopen( filename, options, size );
		if ( pFile )
		{
			if ( fs_iostats_spew.GetBool() )
				Msg( "FS open ok   %s\n", filename );
			return reinterpret_cast<FILE *>(pFile);
		}

		// SourceSpeedrunMod: a missing file is missing for fopen too; trying
		// again doubled the cost of every failed lookup.
		const DWORD err = ::GetLastError();
		if ( err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND )
		{
			++s_IoOpen.fails;
			if ( fs_iostats_spew.GetBool() )
				Msg( "FS open miss %s\n", filename );
			return nullptr;
		}
	}
#endif

	pFile = CStdioFile::FS_fopen( filename, options, size );
	if ( !pFile )
		++s_IoOpen.fails;
	if ( fs_iostats_spew.GetBool() )
		Msg( "FS open %s %s (%s)\n", pFile ? "ok  " : "miss", filename, options );
	return reinterpret_cast<FILE *>(pFile);
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
void CFileSystem_Stdio::FS_setbufsize( FILE *fp, unsigned nBytes )
{
	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);
	pFile->FS_setbufsize( nBytes );
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
void CFileSystem_Stdio::FS_fclose( FILE *fp )
{
	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);

	pFile->FS_fclose();
	delete pFile;
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
// dimhotepus: FS_fseek now returns offset.
int CFileSystem_Stdio::FS_fseek( FILE *fp, int64 pos, int seekType )
{
	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);

	return pFile->FS_fseek( pos, seekType );
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
long CFileSystem_Stdio::FS_ftell( FILE *fp )
{
	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);
	return pFile->FS_ftell();
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
int CFileSystem_Stdio::FS_feof( FILE *fp )
{
	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);
	return pFile->FS_feof();
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
size_t CFileSystem_Stdio::FS_fread( OUT_BYTECAP(destSize) void *dest, size_t destSize, size_t size, FILE *fp )
{
	tmZone( TELEMETRY_LEVEL0, TMZF_NONE, "%s", __FUNCTION__ );
	if( ThreadInMainThread() )
	{
		tmPlotI32( TELEMETRY_LEVEL0, TMPT_MEMORY, 0, size, "FileBytesRead" );
	}

	IoTimer_t ioTimer( s_IoRead );
	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);
	size_t nBytesRead = pFile->FS_fread( dest, destSize, size);
	s_IoRead.bytes += static_cast<int64>( nBytesRead );

	Trace_FRead( nBytesRead, fp );

	return nBytesRead;
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
size_t CFileSystem_Stdio::FS_fwrite( IN_BYTECAP(size) const void *src, size_t size, FILE *fp )
{
	tmZone( TELEMETRY_LEVEL0, TMZF_NONE, "%s %t", __FUNCTION__, tmSendCallStack( TELEMETRY_LEVEL0, 0 ) );
	if( ThreadInMainThread() )
	{
		tmPlotI32( TELEMETRY_LEVEL0, TMPT_MEMORY, 0, size, "FileBytesWrite" );
	}

	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);
	size_t nBytesWritten = pFile->FS_fwrite(src, size);

	return nBytesWritten;
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
bool CFileSystem_Stdio::FS_setmode( FILE *fp, FileMode_t mode )
{
	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);
	return pFile->FS_setmode( mode );
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
size_t CFileSystem_Stdio::FS_vfprintf( FILE *fp, const char *fmt, va_list list )
{
	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);
	return pFile->FS_vfprintf(fmt, list);
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
int CFileSystem_Stdio::FS_ferror( FILE *fp )
{
	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);
	return pFile->FS_ferror();
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
int CFileSystem_Stdio::FS_fflush( FILE *fp )
{
	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);
	return pFile->FS_fflush();
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
char *CFileSystem_Stdio::FS_fgets( OUT_Z_CAP(destSize) char *dest, int destSize, FILE *fp )
{
	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);
	return pFile->FS_fgets(dest, destSize);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *path - 
//			pmode - 
// Output : int
//-----------------------------------------------------------------------------
int CFileSystem_Stdio::FS_chmod( const char *pathT, int pmode )
{
	if ( !pathT )
		return -1;

	char path[ MAX_PATH ];
	CBaseFileSystem::FixUpPath ( pathT, path );

	int rt = _chmod( path, pmode );
#if defined(LINUX)
	if ( rt == -1 )
	{
		char caseFixedName[ MAX_PATH ];
		if ( findFileInDirCaseInsensitive_safe( path, caseFixedName ) )
		{
			rt = _chmod( caseFixedName, pmode );
		}
	}	
#endif
	return rt;
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
int CFileSystem_Stdio::FS_stat( const char *pathT, struct _stat *buf, bool *pbLoadedFromSteamCache )
{
	if ( pbLoadedFromSteamCache )
		*pbLoadedFromSteamCache = false;
		
	if ( !pathT )
	{
		return -1;
	}

	char path[ MAX_PATH ];

	CBaseFileSystem::FixUpPath ( pathT, path );

	IoTimer_t ioTimer( s_IoStat );
	if ( !LooseFile_MayExist( path ) )
	{
		++s_IoStat.fails;
		++s_IoSkip.count;
		errno = ENOENT;
		return -1;
	}
	int rt = _stat( path, buf );
	if ( rt != 0 )
		++s_IoStat.fails;

#if defined(LINUX)
	if ( rt == -1 )
	{
		char caseFixedName[ MAX_PATH ];
		if ( findFileInDirCaseInsensitive_safe( path, caseFixedName ) )
		{
			rt = _stat( caseFixedName, buf );
		}
	}	
#endif
	return rt;
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
HANDLE CFileSystem_Stdio::FS_FindFirstFile(const char *findnameT, WIN32_FIND_DATA *dat)
{
	char findname[ MAX_PATH ];

	CBaseFileSystem::FixUpPath ( findnameT, findname );

	IoTimer_t ioTimer( s_IoFind );
	return ::FindFirstFile(findname, dat);
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
bool CFileSystem_Stdio::FS_FindNextFile(HANDLE handle, WIN32_FIND_DATA *dat)
{

	if (INVALID_HANDLE_VALUE == handle)  // invalid handle should return false
		return false;

	return (::FindNextFile(handle, dat) != 0);
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
bool CFileSystem_Stdio::FS_FindClose(HANDLE handle)
{
	return (::FindClose(handle) != 0);
}

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
int CFileSystem_Stdio::FS_GetSectorSize( FILE *fp )
{
	auto *pFile = reinterpret_cast<CStdFilesystemFile *>(fp);
	return pFile->FS_GetSectorSize();
}

//-----------------------------------------------------------------------------
// Purpose: files are always immediately available on disk
//-----------------------------------------------------------------------------
bool CFileSystem_Stdio::IsFileImmediatelyAvailable(const char *pFileName)
{
	return true;
}

// enable this if you want the stdio filesystem to pretend it's steam, and make people wait for resources
//#define DEBUG_WAIT_FOR_RESOURCES_API

#if defined(DEBUG_WAIT_FOR_RESOURCES_API)
static float g_flDebugProgress = 0.0f;
#endif

//-----------------------------------------------------------------------------
// Purpose: steam call, unnecessary in stdio
//-----------------------------------------------------------------------------
WaitForResourcesHandle_t CFileSystem_Stdio::WaitForResources( const char *resourcelist )
{
#if defined(DEBUG_WAIT_FOR_RESOURCES_API)
	g_flDebugProgress = 0.0f;
#endif

	return 1;
}

//-----------------------------------------------------------------------------
// Purpose: steam call, unnecessary in stdio
//-----------------------------------------------------------------------------
bool CFileSystem_Stdio::GetWaitForResourcesProgress( WaitForResourcesHandle_t handle, float *progress /* out */ , bool *complete /* out */ )
{
#if defined(DEBUG_WAIT_FOR_RESOURCES_API)
	g_flDebugProgress += 0.002f;
	if (g_flDebugProgress < 1.0f)
	{
		*progress = g_flDebugProgress;
		*complete = false;
		return true;
	}
#endif

	// always return that we're complete
	*progress = 0.0f;
	*complete = true;
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: steam call, unnecessary in stdio
//-----------------------------------------------------------------------------
void CFileSystem_Stdio::CancelWaitForResources( WaitForResourcesHandle_t handle )
{
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CFileSystem_Stdio::GetLocalCopy( const char *pFileName )
{
	// do nothing. . everything is local.
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
int CFileSystem_Stdio::HintResourceNeed( const char *hintlist, int forgetEverything )
{
	// do nothing. . everything is local.
	return 0;
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
CStdioFile *CStdioFile::FS_fopen( const char *filenameT, const char *options, int64 *size )
{
	char filename[MAX_PATH];
	V_strcpy_safe( filename, filenameT );
	
	{
		// stop newline characters at end of filename
		char *p = strchr( filename, '\n' );
		if ( p )
			*p = '\0';
		p = strchr( filename, '\r' );
		if ( p )
			*p = '\0';
	}

	// dimhotepus: stat buffer.
	struct STAT_TYPE buf;

	FILE *pFile = fopen( filename, options );

#ifdef LINUX
	// Try opening the lower cased version.
	if ( !pFile && !strchr(options, 'w') && !strchr(options, '+') )
	{
		char caseFixedName[ MAX_PATH ];
		if ( findFileInDirCaseInsensitive_safe( filename, caseFixedName ) )
		{
			pFile = fopen( caseFixedName, options );
			// dimhotepus: filename should contain valid file name.
			if ( pFile )
			{
				V_strcpy_safe( filename, caseFixedName );
			}
		}
	}
#endif  // LINUX

	if (pFile && size)
	{
		// todo: replace with filelength()? 
		int rt = _stat( filename, &buf );
		if (rt == 0)
		{
			*size = buf.st_size;
		}
	}

	if ( pFile )
	{
		const bool bWriteable = strchr(options,'w') || strchr(options,'a');
		
#ifdef POSIX
		if ( bWriteable )
		{
			CThreadMutex *pMutex = nullptr;

			{
				AUTO_LOCK( m_MutexLockedFD );
				// Win32 has an undocumented feature that is serialized ALL writes to a file across threads (i.e only 1 thread can open a file at a time)
				// so add a lock here to mimic that behavior

				auto iLockID = m_LockedFDMap.Find( buf.st_ino );
				if ( iLockID != m_LockedFDMap.InvalidIndex() )
				{
					pMutex = m_LockedFDMap[iLockID];
				}
				else
				{
					auto *newMutex = new CThreadMutex;
					pMutex = m_LockedFDMap[m_LockedFDMap.Insert( buf.st_ino, newMutex )];
				}
			}

			// grab the lock once we have UNLOCKED m_MutexLockedFD so we don't deadlock on a close
			pMutex->Lock();

			rewind( pFile );

			// we need to get the file size again after the lock returns
			if ( size )
			{
				int rt = _stat( filename, &buf );
				if (rt == 0)
				{
					*size = buf.st_size;
				}
			}
		}
#endif  // POSIX

		return new CStdioFile( pFile, bWriteable );
	}

	return nullptr;
}


//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
void CStdioFile::FS_setbufsize( unsigned nBytes )
{
#ifdef _WIN32
	if ( nBytes )
	{
		setvbuf( m_pFile, NULL, _IOFBF, 32768 );
	}
	else
	{
		setvbuf( m_pFile, NULL, _IONBF, 0 );
	}
#endif
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
void CStdioFile::FS_fclose()
{
#ifdef POSIX
	if ( m_bWriteable )
	{
		AUTO_LOCK( m_MutexLockedFD );

		struct STAT_TYPE buf;
		int fd = fileno_unlocked( m_pFile );
		fstat( fd, &buf );

		fflush( m_pFile );
		auto iLockID = m_LockedFDMap.Find( buf.st_ino );
		if ( iLockID != m_LockedFDMap.InvalidIndex() )
		{
			m_LockedFDMap[iLockID]->Unlock();
		}
	}
#endif
	fclose(m_pFile);
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
// dimhotepus: FS_fseek now returns offset.
int CStdioFile::FS_fseek( int64 pos, int seekType )
{
	if ( fseek( m_pFile, pos, seekType ) == 0 )
	{
		// dimhotepus: Return new position.
		return FS_ftell();
	}

	// dimhotepus: Return error flag.
	return -1;
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
long CStdioFile::FS_ftell()
{
	return ftell(m_pFile);
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
int CStdioFile::FS_feof()
{
	return feof(m_pFile);
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
size_t CStdioFile::FS_fread( OUT_BYTECAP(destSize) void *dest, size_t destSize, size_t size )
{
	tmZone( TELEMETRY_LEVEL0, TMZF_NONE, "%s %t", __FUNCTION__, tmSendCallStack( TELEMETRY_LEVEL0, 0 ) );
	if( ThreadInMainThread() )
	{
		tmPlotI32( TELEMETRY_LEVEL0, TMPT_MEMORY, 0, size, "FileBytesRead" );
	}

	// read (size) of bytes to ensure truncated reads returns bytes read and not 0
	return fread( dest, 1, size, m_pFile );
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
size_t CStdioFile::FS_fwrite( IN_BYTECAP(size) const void *src, size_t size )
{
	tmZone( TELEMETRY_LEVEL0, TMZF_NONE, "%s %t", __FUNCTION__, tmSendCallStack( TELEMETRY_LEVEL0, 0 ) );
	if( ThreadInMainThread() )
	{
		tmPlotI32( TELEMETRY_LEVEL0, TMPT_MEMORY, 0, size, "FileBytesWrite" );
	}

	// return number of bytes written (because we have size = 1, count = bytes, so it return bytes)
	// dimhotepus: Write entire file at once, not by chunks as original bug has been fixed.
	const size_t written_bytes{fwrite(src, 1, size, m_pFile)};
	Assert(size == written_bytes);

	return written_bytes;
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
bool CStdioFile::FS_setmode( FileMode_t mode )
{
#ifdef _WIN32
	int fd = _fileno( m_pFile );
	int newMode = ( mode == FM_BINARY ) ? _O_BINARY : _O_TEXT;
	return ( _setmode( fd, newMode) != -1 );
#else
	return false;
#endif
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
size_t CStdioFile::FS_vfprintf( const char *fmt, va_list list )
{
	return vfprintf(m_pFile, fmt, list);
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
int CStdioFile::FS_ferror()
{
	return ferror(m_pFile);
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
int CStdioFile::FS_fflush()
{
	return fflush(m_pFile);
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
char *CStdioFile::FS_fgets( OUT_Z_CAP(destSize) char *dest, int destSize )
{
	return fgets(dest, destSize, m_pFile);
}

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
#ifdef _WIN32

ConVar filesystem_use_overlapped_io( "filesystem_use_overlapped_io", "1", 0, "" );
#define UseOverlappedIO() filesystem_use_overlapped_io.GetBool()

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
int GetSectorSize( const char *pszFilename )
{
	if ( ( !pszFilename[0] || !pszFilename[1] ) ||
		 ( pszFilename[0] == '\\' && pszFilename[1] == '\\' ) ||
		 ( pszFilename[0] == '/' && pszFilename[1] == '/' ) )
	{
		// Cannot determine sector size with a UNC path (need volume identifier)
		return 0;
	}

#if defined( _WIN32 ) && !defined( FILESYSTEM_STEAM )
	char szAbsoluteFilename[MAX_FILEPATH];
	if ( pszFilename[1] != ':' )
	{
		V_MakeAbsolutePath( szAbsoluteFilename, pszFilename );
		pszFilename = szAbsoluteFilename;
	}

	DWORD sectorSize = 1;

	struct DriveSectorSize_t
	{
		char volume;
		DWORD sectorSize;
	};

	static DriveSectorSize_t cachedSizes[8];

	// dimhotepus: tolower -> V_tolower.
	char volume = V_tolower( *pszFilename );

	size_t i;
	for ( i = 0; i < std::size(cachedSizes) && cachedSizes[i].volume; i++ )
	{
		if ( cachedSizes[i].volume == volume )
		{
			sectorSize = cachedSizes[i].sectorSize;
			break;
		}
	}

	if ( sectorSize == 1 )
	{
		char root[4] = "X:\\";
		root[0] = *pszFilename;

		DWORD ignored;
		if ( !GetDiskFreeSpace( root, &ignored, &sectorSize, &ignored, &ignored ) )
		{
			sectorSize = 0;
		}

		if ( i < std::size(cachedSizes) )
		{
			cachedSizes[i].volume = volume;
			cachedSizes[i].sectorSize = sectorSize;
		}
	}

	return sectorSize;
#else
	return 0;
#endif
}


//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------

class CThreadIOEventPool
{
public:
	~CThreadIOEventPool()
	{
		CThreadEvent *pEvent;

		while ( m_Events.PopItem( &pEvent ) )
		{
			delete pEvent;
		}
	}

	CThreadEvent *GetEvent()
	{
		CThreadEvent *pEvent;

		if ( m_Events.PopItem( &pEvent ) )
		{
			return pEvent;
		}

		return new CThreadEvent;
	}

	void ReleaseEvent( CThreadEvent *pEvent )
	{
		m_Events.PushItem( pEvent );
	}

private:
	CTSList<CThreadEvent *> m_Events;
};


CThreadIOEventPool g_ThreadIOEvents;


//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
bool CWin32ReadOnlyFile::CanOpen( const char *, const char *options )
{
	return ( options[0] == 'r' && options[1] == 'b' && options[2] == 0 && filesystem_native.GetBool() );
}

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------

static HANDLE OpenWin32File( const char *filename, bool bOverlapped, bool bUnbuffered, int64 *pFileSize )
{
	DWORD createFlags = FILE_ATTRIBUTE_NORMAL;
		
	if ( bOverlapped )
	{
		createFlags |= FILE_FLAG_OVERLAPPED;
	}

	if ( bUnbuffered )
	{
		createFlags |= FILE_FLAG_NO_BUFFERING;
	}

	HANDLE hFile = ::CreateFile( filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, createFlags, NULL );
	if ( hFile != INVALID_HANDLE_VALUE && !*pFileSize )
	{
		LARGE_INTEGER fileSize;
		if ( !GetFileSizeEx( hFile, &fileSize ) )
		{
			CloseHandle( hFile );
			hFile = INVALID_HANDLE_VALUE;

			// dimhotepus: Ensure 0 size when file is not opened.
			*pFileSize = 0;
		}
		else
		{
			*pFileSize = fileSize.QuadPart;
		}
	}
	return hFile;
}

CWin32ReadOnlyFile *CWin32ReadOnlyFile::FS_fopen( const char *filename, const char *options, int64 *size )
{
	Assert( CanOpen( filename, options ) );

	int sectorSize = 0;
	bool bTryUnbuffered = ( UseUnbufferedIO() && ( sectorSize = GetSectorSize( filename ) ) != 0 );
	bool bOverlapped = UseOverlappedIO();

	HANDLE hFileUnbuffered = INVALID_HANDLE_VALUE;
	int64 fileSize = 0;

	if ( bTryUnbuffered )
	{
		hFileUnbuffered = OpenWin32File( filename, bOverlapped, true, &fileSize );
		if ( hFileUnbuffered == INVALID_HANDLE_VALUE )
		{
			return NULL;
		}
	}

	HANDLE hFileBuffered = OpenWin32File( filename, bOverlapped, false, &fileSize );
	if ( hFileBuffered == INVALID_HANDLE_VALUE )
	{
		if ( hFileUnbuffered != INVALID_HANDLE_VALUE )
		{
			CloseHandle( hFileUnbuffered );
		}
		return NULL;
	}

	if ( size )
	{
		*size = fileSize;
	}

	return new CWin32ReadOnlyFile( hFileUnbuffered, hFileBuffered, ( sectorSize ) ? sectorSize : 1, fileSize, bOverlapped );
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
void CWin32ReadOnlyFile::FS_fclose()
{
	if ( m_hFileUnbuffered != INVALID_HANDLE_VALUE )
	{
		CloseHandle( m_hFileUnbuffered );
	}

	if ( m_hFileBuffered != INVALID_HANDLE_VALUE )
	{
		CloseHandle( m_hFileBuffered );
	}
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
// dimhotepus: FS_fseek now returns offset.
int CWin32ReadOnlyFile::FS_fseek( int64 pos, int seekType )
{
	switch ( seekType )
	{
	case SEEK_SET:
		m_ReadPos = pos;
		// dimhotepus: Return read position.
		return m_ReadPos;

	case SEEK_CUR:
		m_ReadPos += pos;
		// dimhotepus: Return read position.
		return m_ReadPos;

	case SEEK_END:
		m_ReadPos = m_Size - pos;
		// dimhotepus: Return read position.
		return m_ReadPos;

	default:
		// dimhotepus: Handle invalid parameter.
		return -1;
	}
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
long CWin32ReadOnlyFile::FS_ftell()
{
	return m_ReadPos;	
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
int CWin32ReadOnlyFile::FS_feof()
{
	return ( m_ReadPos >= m_Size );	
}

// ends up on a thread's stack, don't blindly increase without awareness of that implication
// dimhotepus: Double read buffer size for performance.
constexpr inline size_t READ_TEMP_BUFFER{64 * 1024};

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
size_t CWin32ReadOnlyFile::FS_fread( OUT_BYTECAP(destSize) void *dest, size_t destSize, size_t size )
{
	VPROF_BUDGET( "CWin32ReadOnlyFile::FS_fread", VPROF_BUDGETGROUP_OTHER_FILESYSTEM );
	tmZone( TELEMETRY_LEVEL0, TMZF_NONE, "%s %t", __FUNCTION__, tmSendCallStack( TELEMETRY_LEVEL0, 0 ) );
	if( ThreadInMainThread() )
	{
		tmPlotI32( TELEMETRY_LEVEL0, TMPT_MEMORY, 0, size, "FileBytesRead" );
	}

	if ( !size || ( m_hFileUnbuffered == INVALID_HANDLE_VALUE && m_hFileBuffered == INVALID_HANDLE_VALUE ) )
	{
		return 0;
	}

	if ( destSize == std::numeric_limits<size_t>::max() )
	{
		destSize = size;
	}

	byte tempBuffer[READ_TEMP_BUFFER];
	int64 offset = m_ReadPos;
	CThreadEvent *pEvent = nullptr;
	HANDLE hReadFile = m_hFileBuffered;
	int nBytesToRead = size;
	byte *pDest = static_cast<byte *>(dest);

	if ( m_hFileUnbuffered != INVALID_HANDLE_VALUE )
	{
		const int destBaseAlign = m_SectorSize;
		bool bDestBaseIsAligned = ( (uintp)dest % destBaseAlign == 0 );
		bool bCanReadUnbufferedDirect = ( bDestBaseIsAligned && ( destSize % m_SectorSize == 0 ) && ( m_ReadPos % m_SectorSize == 0 ) );

		if ( bCanReadUnbufferedDirect )
		{
			// fastest path, unbuffered
			nBytesToRead = AlignValue( size, m_SectorSize );
			hReadFile = m_hFileUnbuffered;
		}
		else
		{
			// not properly aligned, snap to alignments
			// attempt to perform single unbuffered operation using stack buffer
			int64 alignedOffset = AlignValue( ( m_ReadPos - m_SectorSize ) + 1, m_SectorSize );
			const unsigned int alignedBytesToRead = AlignValue( ( m_ReadPos - alignedOffset ) + size, m_SectorSize );
			if ( alignedBytesToRead + destBaseAlign <= sizeof( tempBuffer ) )
			{
				// read operation can be performed as unbuffered follwed by a post fixup
				nBytesToRead = alignedBytesToRead;
				offset = alignedOffset;
				pDest = AlignValue( tempBuffer, destBaseAlign );
				hReadFile = m_hFileUnbuffered;
			}
		}
	}

	OVERLAPPED overlapped = {};
	if ( m_bOverlapped )
	{
		pEvent = g_ThreadIOEvents.GetEvent();
		overlapped.hEvent = *pEvent;
	}

#ifdef REPORT_BUFFERED_IO
	if ( hReadFile == m_hFileBuffered && filesystem_report_buffered_io.GetBool() )
	{
		Msg( "Buffered Operation :(\n" );
	}
#endif

	// some disk drivers will fail if read is too large
	static int MAX_READ = filesystem_max_stdio_read.GetInt()*1024*1024;
	constexpr int MIN_READ = 64 * 1024;

	bool bReadOk = true;
	DWORD nBytesRead = 0;
	size_t result = 0;
	int64 currentOffset = offset;

	while ( bReadOk && nBytesToRead > 0 )
	{
		int nCurBytesToRead = min( nBytesToRead, MAX_READ );
		DWORD nCurBytesRead = 0;

		overlapped.Offset = currentOffset & 0xFFFFFFFF;
		overlapped.OffsetHigh = ( currentOffset >> 32 ) & 0xFFFFFFFF;

		bReadOk = ( ::ReadFile( hReadFile, pDest + nBytesRead, nCurBytesToRead, &nCurBytesRead, &overlapped ) != 0 );
		if ( !bReadOk )
		{
			if ( m_bOverlapped && GetLastError() == ERROR_IO_PENDING )
			{
				bReadOk = true;
			}
		}

		if ( bReadOk )
		{
			if ( !m_bOverlapped || GetOverlappedResult( hReadFile, &overlapped, &nCurBytesRead, true ) )
			{
				nBytesRead += nCurBytesRead;
				nBytesToRead -= nCurBytesRead;
				currentOffset += nCurBytesRead;
			}
			else
			{
				if ( m_bOverlapped )
				{
					if ( GetLastError() == ERROR_HANDLE_EOF )
					{
						nBytesToRead = 0; // we have hit the end of the file
					}
					else
					{
						bReadOk = false;
					}
				}
				else
				{
					bReadOk = false;
				}
			}

			if ( !m_bOverlapped && nCurBytesRead == 0 )
			{
				nBytesToRead = 0; // we have hit the end of the file
			}

		}

		if ( nBytesToRead > 0 && nCurBytesRead == 0 ) // if you failed to ready anything this time then bail the loop
		{
			DevMsg( "Got zero length read" );
			bReadOk = false;
		}
		else if ( !bReadOk  )
		{
			DWORD dwError = GetLastError();

			if ( dwError == ERROR_NO_SYSTEM_RESOURCES && MAX_READ > MIN_READ )
			{
				MAX_READ /= 2;
				bReadOk = true;
				DevMsg( "ERROR_NO_SYSTEM_RESOURCES: Reducing max read to %d bytes\n", MAX_READ );
			}
			else
			{
				DevMsg( "Unknown read error '%s'.\n", std::system_category().message(dwError).c_str() );
			}
		}
	}

	if ( bReadOk )
	{
		if ( nBytesRead && hReadFile == m_hFileUnbuffered && pDest != dest )
		{
			int64 nBytesExtra = ( m_ReadPos - offset );
			nBytesRead -= nBytesExtra;
			if ( nBytesRead )
			{
				memcpy( dest, (byte *)pDest + nBytesExtra, size );
			}
		}

		result = min( (size_t)nBytesRead, size );
	}

	if ( m_bOverlapped )
	{
		pEvent->Reset();
		g_ThreadIOEvents.ReleaseEvent( pEvent );
	}

	m_ReadPos += result;

	return result;
}

//-----------------------------------------------------------------------------
// Purpose: low-level filesystem wrapper
//-----------------------------------------------------------------------------
char *CWin32ReadOnlyFile::FS_fgets( OUT_Z_CAP(destSize) char *dest, int destSize ) 
{  
	if ( FS_feof() )
	{
		// dimhotepus: Zero-terminate on failure.
		if (destSize > 0)
			dest[0] = '\0';
		return nullptr;
	}

	int64 nStartPos = m_ReadPos;
	int nBytesRead = FS_fread( dest, destSize, destSize );
	if ( !nBytesRead )
	{
		// dimhotepus: Zero-terminate on failure.
		if (destSize > 0)
			dest[0] = '\0';
		return NULL;
	}

	dest[min( nBytesRead, destSize - 1)] = 0;
	char *pNewline = strchr( dest, '\n' );
	if ( pNewline )
	{
		// advance past, leave \n
		pNewline++;
		*pNewline = 0;
	}
	else
	{
		pNewline = &dest[min( nBytesRead, destSize - 1)];
	}

	m_ReadPos = nStartPos + ( pNewline - dest ) + 1;

	return dest; 
}


#endif // _WIN32
