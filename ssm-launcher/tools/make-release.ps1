# Builds SourceSpeedrunMod-<version>.zip from a game folder, for the launcher.
#
#   powershell -ExecutionPolicy Bypass -File tools\make-release.ps1 -GameDir ..\game -Version v1.0.1 -OutDir ..\release
#
# - copies the game without player data / debug files,
# - adds the Visual C++ runtime next to the binaries (app-local, so players
#   don't need the VC++ redistributable),
# - refuses to package anything that would not start on a clean PC: DLLs that
#   need Visual Studio's debug runtime, or imports that are neither in the
#   package nor part of Windows.
param(
  [Parameter(Mandatory)] [string] $GameDir,
  [Parameter(Mandatory)] [string] $Version,
  [string] $OutDir = (Join-Path $PSScriptRoot '..\..\release')
)
$ErrorActionPreference = 'Stop'

$GameDir = (Resolve-Path $GameDir).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$stage = Join-Path $OutDir 'SourceSpeedrunMod'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }

# --- Copy without player data and build leftovers.
$excludeDirs = 'bin\x64\cef\cache', 'hl2\save', 'hl2\cache', 'hl2\screenshots', 'hl2\splits', 'hl2\downloadlists', 'hl2\maps', 'hl2\sound', '.download' | ForEach-Object { Join-Path $GameDir $_ }
$excludeFiles = '*.pdb', '*.log', '*.bak', '*.tmp', '*.ilk', '*.exp', '*.lib', 'config.cfg', 'autoexec.cfg', 'modelsounds.cache', 'voice_ban.dt', 'stats.txt', 'GameState.txt', 'ssm-version.txt', 'SSM-Launcher*.exe'
robocopy $GameDir $stage /E /NFL /NDL /NJH /NJS /XD @excludeDirs /XF @excludeFiles | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
New-Item -ItemType Directory -Force (Join-Path $stage 'hl2\splits') | Out-Null
'Put LiveSplit .lss files here, then pick them in Settings -> Timer.' | Set-Content (Join-Path $stage 'hl2\splits\README.txt')

# --- Visual C++ runtime (newest toolset installed with Visual Studio).
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending |
  ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue } | Select-Object -First 1
if (-not $crt) { throw 'Visual C++ redistributable files not found in the Visual Studio install.' }
foreach ($dir in $stage, (Join-Path $stage 'bin\x64')) {
  foreach ($f in 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') { Copy-Item (Join-Path $crt.FullName $f) $dir -Force }
}
Write-Host "VC runtime: $($crt.FullName)"

# --- Check every binary's imports.
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$bins = Get-ChildItem $stage -Recurse -Include *.dll, *.exe | Where-Object { $_.FullName -notmatch '\\bin\\x64\\cef\\' }
$list = Join-Path $env:TEMP "ssm-release-bins.txt"
$bins.FullName | Set-Content $list
$dump = cmd /c "set NoDefaultCurrentDirectoryInExePath=& call `"$vcvars`" >NUL && for /f `"usebackq delims=`" %f in (`"$list`") do @(echo ##FILE %f & dumpbin /nologo /dependents `"%f`")"
$packaged = $bins.Name.ToLower() + (Get-ChildItem $stage -Recurse -Include *.dll).Name.ToLower() | Sort-Object -Unique
$problems = @()
$cur = ''
foreach ($line in $dump) {
  if ($line -like '##FILE*') { $cur = Split-Path $line.Substring(7).Trim() -Leaf; continue }
  $d = $line.Trim().ToLower()
  if ($d -notmatch '^[\w.-]+\.dll$') { continue }
  if ($d -match '^(ucrtbased|vcruntime140d|vcruntime140_1d|msvcp140d|concrt140d)\.dll$') { $problems += "$cur needs the DEBUG runtime $d (ship the Release build)"; continue }
  if ($d -match '^(api-ms-win|ext-ms-win)') { continue }
  if ($packaged -contains $d) { continue }
  if (Test-Path (Join-Path $env:WINDIR "System32\$d")) {
    if ($d -match '^(msvcp|vcruntime|concrt|vcomp|msvcr|d3dx|xinput1_[0-3]|xaudio2_[0-7]|mfc)') { $problems += "$cur needs $d, which is not part of Windows" }
    continue
  }
  $problems += "$cur needs $d, which is missing"
}
if ($problems) { $problems | Sort-Object -Unique | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }; throw 'Release would not start on a clean PC.' }
Write-Host "Imports OK: $($bins.Count) binaries checked"

# --- Zip (one top-level SourceSpeedrunMod/ folder, as the launcher expects).
$zip = Join-Path $OutDir "SourceSpeedrunMod-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Push-Location $OutDir
try { tar.exe -a -c -f $zip SourceSpeedrunMod } finally { Pop-Location }
if ($LASTEXITCODE) { throw 'zip failed' }
"{0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB)
