# Regression suite for the zstd VFS. -ExpectKillFail asserts PRE-FIX behavior.
param([switch]$ExpectKillFail)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repo 'tools\out\zstd-vfs-test.exe'
if (-not (Test-Path $exe)) { throw "build first: tools\build-vfs-test.ps1" }
$env:PATH = "c:\gtk-build\gtk4\x64\release\bin;$env:PATH"
$db = Join-Path $env:TEMP ("vfs-test-{0}.db" -f [guid]::NewGuid())

function Step($name, $cmdargs, $expect) {
  & $exe $db @cmdargs | Write-Host
  if ($LASTEXITCODE -ne $expect) { throw "FAIL ${name}: exit $LASTEXITCODE, expected $expect" }
  Write-Host "PASS: $name"
}

# 1. clean lifecycle
Step 'fill-clean' @('fill','500') 0
Step 'check-clean' @('check') 0

# 2. commit + process death (the field failure: kill the app, reopen)
& $exe $db kill 500 | Write-Host
if ($LASTEXITCODE -ne 9) { throw "FAIL kill: exit $LASTEXITCODE, expected 9" }
& $exe $db geom | Write-Host
$killExpect = if ($ExpectKillFail) { 1 } else { 0 }
Step 'check-after-kill' @('check') $killExpect

# 3. busy: another process holds the outer DB -> must be error(2), never corrupt(1)
if (-not $ExpectKillFail) {
  Step 'reset-fill' @('fill','10') 0   # ensure db is healthy for the busy test
}
$hold = Start-Process $exe -ArgumentList "`"$db`"",'hold','8' -PassThru -NoNewWindow
Start-Sleep 2
& $exe $db check | Write-Host
if ($LASTEXITCODE -ne 2) { throw "FAIL busy-check: exit $LASTEXITCODE, expected 2" }
Write-Host 'PASS: busy-check'
$hold.WaitForExit()
if (-not $ExpectKillFail) { Step 'check-after-hold' @('check') 0 }

Remove-Item $db -ErrorAction SilentlyContinue
Write-Host 'ALL PASS'
