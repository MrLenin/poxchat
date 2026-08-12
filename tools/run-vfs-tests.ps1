# Regression suite for the zstd VFS. -ExpectKillFail asserts PRE-FIX behavior.
param([switch]$ExpectKillFail)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repo 'tools\out\zstd-vfs-test.exe'
if (-not (Test-Path $exe)) { throw "build first: tools\build-vfs-test.ps1" }
$env:PATH = "c:\gtk-build\gtk4\x64\release\bin;$env:PATH"
$db = Join-Path $env:TEMP ("vfs-test-{0}.db" -f [guid]::NewGuid())

function Step($name, $cmdargs, $expect) {
  $out = & $exe $db @cmdargs
  $out | Write-Host
  if ($LASTEXITCODE -ne $expect) { throw "FAIL ${name}: exit $LASTEXITCODE, expected $expect" }
  Write-Host "PASS: $name"
  return $out
}

function AssertRows($name, $out, $expected) {
  if (-not ($out -match [regex]::Escape($expected))) {
    throw "FAIL ${name}: expected '$expected' in output, got: $($out -join "`n")"
  }
  Write-Host "PASS: $name row count ($expected)"
}

# 1. clean lifecycle
Step 'fill-clean' @('fill','500') 0
Step 'check-clean' @('check') 0

# 2. commit + process death (the field failure: kill the app, reopen)
& $exe $db kill 500 | Write-Host
if ($LASTEXITCODE -ne 9) { throw "FAIL kill: exit $LASTEXITCODE, expected 9" }
& $exe $db geom | Write-Host
$killExpect = if ($ExpectKillFail) { 1 } else { 0 }
$killOut = Step 'check-after-kill' @('check') $killExpect
if (-not $ExpectKillFail) {
  # The kill leg only regression-tests the geometry self-heal if recovery
  # actually finds all the rows -- a VFS bug that presents a populated DB
  # as an empty-but-valid one would still print "quick_check: ok" and exit
  # 0, so the row count must be asserted explicitly.
  AssertRows 'check-after-kill' $killOut 'rows: 1000'
}

# 2b. stale-meta self-heal: sabotage meta.page_count directly (simulating a
#     desync between meta and the pages table that isn't caused by process
#     death) and confirm the VFS derives geometry from the pages table
#     instead of trusting the corrupted meta row.
if (-not $ExpectKillFail) {
  # Passed as a file rather than `python -c "..."` -- PowerShell's native-argument
  # quoting mangles embedded double quotes when reconstructing the arg for the
  # child process, which silently corrupts an inline -c string.
  $pyScript = Join-Path $env:TEMP ("vfs-sabotage-{0}.py" -f [guid]::NewGuid())
  @"
import sqlite3
db = sqlite3.connect(r'$db')
db.execute("UPDATE meta SET value='1' WHERE key='page_count'")
db.commit()
db.close()
"@ | Set-Content -Path $pyScript -Encoding ASCII
  & python $pyScript
  $pyExit = $LASTEXITCODE
  Remove-Item $pyScript -ErrorAction SilentlyContinue
  if ($pyExit -ne 0) { throw "FAIL stale-meta-self-heal: python sabotage step exited $pyExit" }
  $staleOut = Step 'stale-meta-self-heal' @('check') 0
  AssertRows 'stale-meta-self-heal' $staleOut 'rows: 1000'
}

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
