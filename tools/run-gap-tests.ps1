# Runs the gap-ledger test harness in a scratch dir.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repo 'tools\out\gap-ledger-test.exe'
if (-not (Test-Path $exe)) { throw 'build first: pwsh tools\build-gap-test.ps1' }
$deps = 'c:\gtk-build\gtk4\x64\release'
$env:PATH = "$deps\bin;$env:PATH"
$scratch = Join-Path $env:TEMP ("gap-test-{0}" -f [guid]::NewGuid())
New-Item -ItemType Directory -Force $scratch | Out-Null
& $exe $scratch
$code = $LASTEXITCODE
Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
if ($code -ne 0) { throw "gap-ledger tests FAILED ($code)" }
Write-Host 'gap-ledger tests PASS'
