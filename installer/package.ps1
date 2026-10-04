# Builds the release zip (Nexus layout, mod-manager installable: SKSE/ at the archive root): the
# SKSE plugin and its ini, the asset installer (installer/install.py frozen with PyInstaller, so
# players need no Python) in "Skydive Installer/", and the readme. No JC2 data goes in.
# Build the plugin first (skse/README.md). Needs: python -m pip install pyinstaller numpy lz4 miniaudio
# The installer is a one-folder build: one-file PyInstaller exes unpack themselves at runtime,
# which virus scanners (Nexus's included) often flag.
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root "build\package"
$stage = Join-Path $out "Skydive"
$dll = Join-Path $root "build\skse\Release\Skydive.dll"
if (-not (Test-Path $dll)) { throw "build the plugin first: $dll is missing" }
$version = (Select-String -Path (Join-Path $root "skse\CMakeLists.txt") -Pattern 'project\(\S+ VERSION (\S+)').Matches[0].Groups[1].Value

# PowerShell 5.1 turns a native tool's stderr (PyInstaller logs there) into errors under "Stop";
# the tools are judged by their exit codes instead.
$ErrorActionPreference = "Continue"
python -m PyInstaller --noconfirm --onedir --windowed --name Skydive-Installer `
    --paths (Join-Path $root "tools\anim") --paths (Join-Path $root "tools\re") `
    --collect-all miniaudio --hidden-import _cffi_backend `
    --distpath (Join-Path $out "dist") --workpath (Join-Path $out "work") --specpath $out `
    (Join-Path $root "installer\install.py")
if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }
$ErrorActionPreference = "Stop"

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage "SKSE\Plugins") | Out-Null
Copy-Item $dll (Join-Path $stage "SKSE\Plugins")
Copy-Item (Join-Path $root "skse\Skydive.ini") (Join-Path $stage "SKSE\Plugins")
Copy-Item -Recurse (Join-Path $out "dist\Skydive-Installer") (Join-Path $stage "Skydive Installer")
Copy-Item (Join-Path $PSScriptRoot "README.txt") (Join-Path $stage "Skydive - README.txt")

# Python's zipfile: Compress-Archive on PowerShell 5.1 writes backslashes into entry names.
$zip = Join-Path $out "Skydive-$version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
python -c "import os, sys, zipfile; src, dst = sys.argv[1:]; z = zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED); [z.write(os.path.join(d, f), os.path.relpath(os.path.join(d, f), src).replace(os.sep, '/')) for d, _, fs in os.walk(src) for f in fs]; z.close()" $stage $zip
if ($LASTEXITCODE -ne 0) { throw "zip failed" }
Write-Host "release: $zip"
