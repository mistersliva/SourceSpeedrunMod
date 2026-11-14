// Copyright Source Authors. All rights reserved.
//
// Static content server for the HTML GameUI. See UiAssets.h.

#include "UiAssets.h"

#include "filesystem.h"
#include "tier1/strtools.h"
#include "tier1/utlmemory.h"
#include "tier2/tier2.h"
#include "bitmap/imageformat.h"
#include "bitmap/tgaloader.h"
#include "vtf/vtf.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#pragma warning( push )
#pragma warning( disable : 4996 4244 4456 4457 4100 4245 4505 4701 4703 )
#include "../thirdparty/stb/stb_image_write.h"
#pragma warning( pop )

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace
{

struct MimeEntry
{
	const char *ext;
	const char *mime;
};

constexpr MimeEntry kMimeTypes[] = {
	{ "html", "text/html" },
	{ "htm", "text/html" },
	{ "js", "application/javascript" },
	{ "mjs", "application/javascript" },
	{ "css", "text/css" },
	{ "json", "application/json" },
	{ "txt", "text/plain" },
	{ "cfg", "text/plain" },
	{ "res", "text/plain" },
	{ "png", "image/png" },
	{ "jpg", "image/jpeg" },
	{ "jpeg", "image/jpeg" },
	{ "gif", "image/gif" },
	{ "webp", "image/webp" },
	{ "svg", "image/svg+xml" },
	{ "ico", "image/x-icon" },
	{ "ttf", "font/ttf" },
	{ "otf", "font/otf" },
	{ "woff", "font/woff" },
	{ "woff2", "font/woff2" },
	{ "wav", "audio/wav" },
	{ "mp3", "audio/mpeg" },
	{ "ogg", "audio/ogg" },
	{ "mp4", "video/mp4" },
	{ "webm", "video/webm" },
};

const char *MimeForPath( const char *path )
{
	const char *ext = V_GetFileExtension( path );
	if ( !ext )
		return "application/octet-stream";
	for ( const auto &m : kMimeTypes )
	{
		if ( V_stricmp( ext, m.ext ) == 0 )
			return m.mime;
	}
	return "application/octet-stream";
}

// Decodes %XX sequences and validates the relative path (no "..", no drive
// letters, no absolute paths). Returns false when unsafe.
bool SanitizeRelativePath( const char *in, char *out, int outSize )
{
	int o = 0;
	for ( const char *p = in; *p && o < outSize - 1; ++p )
	{
		char c = *p;
		if ( c == '%' && V_isxdigit( p[1] ) && V_isxdigit( p[2] ) )
		{
			char hex[3] = { p[1], p[2], 0 };
			c = static_cast<char>( strtol( hex, nullptr, 16 ) );
			p += 2;
		}
		if ( c == '\\' ) c = '/';
		if ( c == '\0' || c == ':' ) return false;
		out[o++] = c;
	}
	out[o] = '\0';

	if ( out[0] == '/' || out[0] == '\0' )
		return false;

	// Reject any ".." segment.
	for ( const char *seg = out; *seg; )
	{
		const char *slash = strchr( seg, '/' );
		const intp len = slash ? slash - seg : V_strlen( seg );
		if ( len == 2 && seg[0] == '.' && seg[1] == '.' )
			return false;
		if ( !slash ) break;
		seg = slash + 1;
	}
	return true;
}

void WriteToUtlBuffer( void *context, void *data, int size )
{
	static_cast<CUtlBuffer *>( context )->Put( data, size );
}

int ServeText( CUtlBuffer &out, const char *&mime, int status, const char *text )
{
	out.Put( text, V_strlen( text ) );
	mime = "text/plain";
	return status;
}

int ServeUiFile( const char *rel, CUtlBuffer &out, const char *&mime )
{
	char path[MAX_PATH];
	V_sprintf_safe( path, "htmlui/%s", rel );

	if ( !g_pFullFileSystem->ReadFile( path, "GAME", out ) &&
		 !g_pFullFileSystem->ReadFile( path, "PLATFORM", out ) )
	{
		return ServeText( out, mime, 404, "not found" );
	}
	mime = MimeForPath( rel );
	return 200;
}

int ServeGameFile( const char *rel, CUtlBuffer &out, const char *&mime )
{
	if ( !g_pFullFileSystem->ReadFile( rel, "GAME", out ) )
		return ServeText( out, mime, 404, "not found" );
	mime = MimeForPath( rel );
	return 200;
}

int ServeVTF( const char *rel, CUtlBuffer &out, const char *&mime )
{
	char path[MAX_PATH];
	V_sprintf_safe( path, "materials/%s", rel );
	V_SetExtension( path, ".vtf", sizeof( path ) );

	CUtlBuffer file;
	if ( !g_pFullFileSystem->ReadFile( path, "GAME", file ) )
		return ServeText( out, mime, 404, "not found" );

	IVTFTexture *tex = CreateVTFTexture();
	if ( !tex )
		return ServeText( out, mime, 500, "vtf alloc failed" );

	int status = 415;
	if ( tex->Unserialize( file ) )
	{
		tex->ConvertImageFormat( IMAGE_FORMAT_RGBA8888, false );
		const unsigned char *pixels = tex->ImageData( 0, 0, 0 );
		if ( pixels && UiAssets_EncodePNG( pixels, tex->Width(), tex->Height(), out ) )
		{
			mime = "image/png";
			status = 200;
		}
	}
	DestroyVTFTexture( tex );

	if ( status != 200 )
		return ServeText( out, mime, status, "unsupported vtf" );
	return status;
}

int ServeTGA( const char *rel, CUtlBuffer &out, const char *&mime )
{
	const char *ext = V_GetFileExtension( rel );
	if ( !ext || V_stricmp( ext, "tga" ) != 0 )
		return ServeText( out, mime, 403, "forbidden" );

	CUtlBuffer file;
	if ( !g_pFullFileSystem->ReadFile( rel, "MOD", file ) &&
		 !g_pFullFileSystem->ReadFile( rel, "GAME", file ) )
	{
		return ServeText( out, mime, 404, "not found" );
	}

	CUtlMemory<unsigned char> pixels;
	int w = 0, h = 0;
	if ( !TGALoader::LoadRGBA8888( file, pixels, w, h ) || w <= 0 || h <= 0 )
		return ServeText( out, mime, 415, "unsupported tga" );

	if ( !UiAssets_EncodePNG( pixels.Base(), w, h, out ) )
		return ServeText( out, mime, 500, "png encode failed" );

	mime = "image/png";
	return 200;
}

}  // namespace

bool UiAssets_EncodePNG( const unsigned char *rgba, int width, int height, CUtlBuffer &out )
{
	if ( !rgba || width <= 0 || height <= 0 )
		return false;
	return stbi_write_png_to_func( WriteToUtlBuffer, &out, width, height, 4, rgba, width * 4 ) != 0;
}

int UiAssets_Serve( const char *path, const char *query, CUtlBuffer &out, const char *&mime )
{
	(void)query;

	if ( !g_pFullFileSystem )
		return ServeText( out, mime, 503, "filesystem unavailable" );

	if ( !path || !*path || V_strcmp( path, "/" ) == 0 )
		return ServeUiFile( "index.html", out, mime );

	struct Route
	{
		const char *prefix;
		int ( *handler )( const char *, CUtlBuffer &, const char *& );
	};
	static constexpr Route kRoutes[] = {
		{ "/ui/", ServeUiFile },
		{ "/game/", ServeGameFile },
		{ "/img/vtf/", ServeVTF },
		{ "/img/tga/", ServeTGA },
	};

	for ( const auto &route : kRoutes )
	{
		const intp len = V_strlen( route.prefix );
		if ( V_strncmp( path, route.prefix, len ) != 0 )
			continue;

		char rel[MAX_PATH];
		if ( !SanitizeRelativePath( path + len, rel, sizeof( rel ) ) )
			return ServeText( out, mime, 403, "forbidden" );

		return route.handler( rel, out, mime );
	}

	return ServeText( out, mime, 404, "not found" );
}
