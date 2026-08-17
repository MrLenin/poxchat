# Builds tools\out\gap-ledger-test.exe (x64). Requires VS 2022 + gvsbuild deps
# and a prior solution build (for the generated config.h).
$ErrorActionPreference = 'Stop'
$devShell = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\Tools\Launch-VsDevShell.ps1' | Select-Object -First 1
if (-not $devShell) { throw 'VS 2022 Launch-VsDevShell.ps1 not found' }
$repo = Split-Path $PSScriptRoot -Parent
& $devShell.FullName -Arch amd64 -SkipAutomaticLocation | Out-Null
$deps = 'c:\gtk-build\gtk4\x64\release'
$cfg = Join-Path $repo '..\poxchat-build-gtk4\x64\lib'
if (-not (Test-Path (Join-Path $cfg 'config.h'))) { throw "generated config.h not found at $cfg - build the solution first" }
# The generated config.h defines USE_OPENSSL, so scrollback.c's poxchat.h
# include pulls in <openssl/ssl.h> (win32\poxchat.props' YourOpenSSLPath).
$openssl = 'C:\Program Files\OpenSSL\include'
if (-not (Test-Path (Join-Path $openssl 'openssl\ssl.h'))) { throw "OpenSSL headers not found at $openssl - see win32\poxchat.props YourOpenSSLPath" }
$out = Join-Path $repo 'tools\out'
New-Item -ItemType Directory -Force $out | Out-Null
cl /nologo /O1 /W3 /MD /D_CRT_SECURE_NO_WARNINGS `
  "/I$cfg" "/I$repo\src\common" "/I$repo\src\common\zstd" "/I$openssl" `
  "/I$deps\include" "/I$deps\include\glib-2.0" "/I$deps\lib\glib-2.0\include" "/I$deps\include\gio-win32-2.0" `
  "$repo\tools\gap-ledger-test.c" "$repo\src\common\scrollback.c" "$repo\src\common\sqlite-zstd-vfs.c" "$repo\src\common\zstd\zstd.c" `
  /Fo"$out\" /Fe"$out\gap-ledger-test.exe" `
  /link "/LIBPATH:$deps\lib" sqlite3.lib glib-2.0.lib gio-2.0.lib gobject-2.0.lib intl.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }
Write-Host "built $out\gap-ledger-test.exe"
