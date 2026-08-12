# Builds tools\out\zstd-vfs-test.exe (x64). Requires VS 2022 + gvsbuild deps.
$ErrorActionPreference = 'Stop'
$devShell = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\Tools\Launch-VsDevShell.ps1' | Select-Object -First 1
if (-not $devShell) { throw 'VS 2022 Launch-VsDevShell.ps1 not found' }
$repo = Split-Path $PSScriptRoot -Parent
& $devShell.FullName -Arch amd64 -SkipAutomaticLocation | Out-Null
$deps = 'c:\gtk-build\gtk4\x64\release'
$out = Join-Path $repo 'tools\out'
New-Item -ItemType Directory -Force $out | Out-Null
cl /nologo /O1 /W3 /MD /D_CRT_SECURE_NO_WARNINGS `
  "/I$repo\src\common" "/I$repo\src\common\zstd" `
  "/I$deps\include" "/I$deps\include\glib-2.0" "/I$deps\lib\glib-2.0\include" `
  "$repo\tools\zstd-vfs-test.c" "$repo\src\common\sqlite-zstd-vfs.c" "$repo\src\common\zstd\zstd.c" `
  /Fo"$out\" /Fe"$out\zstd-vfs-test.exe" `
  /link "/LIBPATH:$deps\lib" sqlite3.lib glib-2.0.lib intl.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }
Write-Host "built $out\zstd-vfs-test.exe"
