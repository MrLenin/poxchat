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
if (Test-Path "$db-wal") { throw 'FAIL fill-clean: -wal left behind after clean close' }
Step 'check-clean' @('check') 0

# 2. commit + process death (the field failure: kill the app, reopen)
& $exe $db kill 500 | Write-Host
if ($LASTEXITCODE -ne 9) { throw "FAIL kill: exit $LASTEXITCODE, expected 9" }
if (-not $ExpectKillFail -and -not (Test-Path "$db-wal")) {
  throw 'FAIL kill: no -wal after kill -- outer DB is not actually in WAL mode'
}
& $exe $db geom | Write-Host
$killExpect = if ($ExpectKillFail) { 1 } else { 0 }
# Captured with stderr merged (not via Step): the WAL-replay diagnostic is a
# g_message, and GLib routes g_message/g_warning to stderr in this
# environment, which a plain `$out = & $exe ...` capture (as Step uses)
# would silently drop.
$killOut = & $exe $db check 2>&1 | Out-String
Write-Host $killOut
if ($LASTEXITCODE -ne $killExpect) { throw "FAIL check-after-kill: exit $LASTEXITCODE, expected $killExpect" }
Write-Host 'PASS: check-after-kill'
if (-not $ExpectKillFail) {
  # The kill leg only regression-tests the geometry self-heal if recovery
  # actually finds all the rows -- a VFS bug that presents a populated DB
  # as an empty-but-valid one would still print "quick_check: ok" and exit
  # 0, so the row count must be asserted explicitly.
  AssertRows 'check-after-kill' $killOut 'rows: 1000'
  # The tail rows live in the -wal until this reopen; the open must log the
  # replay diagnostic, and the clean close must checkpoint the -wal away.
  if (-not ($killOut -match 'WAL sidecar present')) {
    throw 'FAIL check-after-kill: WAL-replay diagnostic not logged at open'
  }
  if (Test-Path "$db-wal") {
    throw 'FAIL check-after-kill: -wal not checkpointed away on clean close'
  }
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

# 2c. backup-carries-WAL: after a kill the un-checkpointed tail lives in the
#     -wal sidecar.  Moving a DB aside (the scrollback_backup_corrupt path)
#     must move the PAIR: renaming only the main file would strand a -wal
#     that SQLite replays into the next database created at that path
#     (corrupting it), and the backup would silently lose the tail.
if (-not $ExpectKillFail) {
  & $exe $db kill 500 | Write-Host
  if ($LASTEXITCODE -ne 9) { throw "FAIL backup-leg kill: exit $LASTEXITCODE, expected 9" }
  if (-not (Test-Path "$db-wal")) { throw 'FAIL backup-leg: no -wal after kill' }
  $bk = "$db.corrupt.test"
  Step 'backup-pair' @('backup', $bk) 0
  if (Test-Path $db) { throw 'FAIL backup-pair: original DB still present' }
  if (Test-Path "$db-wal") { throw 'FAIL backup-pair: -wal stranded at original path' }
  if (-not (Test-Path $bk)) { throw 'FAIL backup-pair: backup missing' }
  if (-not (Test-Path "$bk-wal")) { throw 'FAIL backup-pair: backup -wal sidecar missing' }
  $bkOut = & $exe $bk check
  $bkOut | Write-Host
  if ($LASTEXITCODE -ne 0) { throw "FAIL check-backup: exit $LASTEXITCODE, expected 0" }
  AssertRows 'check-backup' $bkOut 'rows: 1500'
  if (Test-Path "$bk-wal") { throw 'FAIL check-backup: backup -wal not checkpointed away on clean close' }
  Remove-Item "$bk*" -ErrorAction SilentlyContinue
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

Remove-Item "$db*" -ErrorAction SilentlyContinue
Write-Host 'ALL PASS'
