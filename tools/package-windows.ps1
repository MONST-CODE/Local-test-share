param([string]$BuildDir = 'out/build/clang-ui', [string]$StageDir = 'out/stage-windows')
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
$cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path $cacheFile)) { throw 'Configure clang-ui first' }
$cache = Get-Content $cacheFile
if (-not ($cache -match '^CMAKE_BUILD_TYPE:STRING=Release$') -or -not ($cache -match '^PLPT_BUILD_UI:BOOL=ON$')) {
    throw 'Packaging requires a Release build with PLPT_BUILD_UI=ON'
}
# Toute erreur native doit arrêter la chaîne (PowerShell ne le fait pas implicitement).
& cmake --build $BuildDir --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& ctest --test-dir $BuildDir --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
& cmake --install $BuildDir --prefix $StageDir
if ($LASTEXITCODE -ne 0) { throw 'Install staging failed' }
$stage = (Resolve-Path $StageDir).Path
$makensis = Get-Command makensis.exe -ErrorAction SilentlyContinue
if ($makensis) { $nsis = $makensis.Source }
else { $nsis = "${env:ProgramFiles(x86)}\NSIS\makensis.exe" }
if (-not (Test-Path $nsis)) { throw 'NSIS 3.11 is required to generate the installer; ZIP remains available through CPack.' }
$version = (& $nsis '/VERSION').Trim()
if ($version -ne 'v3.11') { throw "NSIS 3.11 required; found $version" }
$output = Join-Path (Get-Location) 'out/PLPT-0.1.0-preview-windows-x64-setup.exe'
& $nsis "/DSTAGE=$stage" "/DOUTPUT=$output" 'packaging/windows/plpt-preview.nsi'
if ($LASTEXITCODE -ne 0) { throw 'NSIS compilation failed' }
Get-FileHash -Algorithm SHA256 $output
Write-Host 'Unsigned development installer only. Install/uninstall and SmartScreen tests still required.'
