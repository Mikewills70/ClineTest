# ===================================================================================================
# BEAT LOG RECORDER - DEVELOPMENT TOOL
# ===================================================================================================
# WHY: the board keeps only a 512-beat ring (about 124-227 seconds), so any run not captured promptly is
# lost - which is how the first attempt at aligning the taps failed. And the user asked: "you record this
# data too, so I dont have to keep playing it."
# WHAT IT DOES: polls /beats every couple of seconds and MERGES the new beats into one growing CSV, so a
# whole session is captured no matter how long it runs. Beats are deduplicated on their millisecond
# timestamp, so overlapping snapshots cannot double-count.
# HOW TO USE: run it, then play the track normally. Leave it running while you listen. Press Q to stop.
# It owns the board's single HTTP connection while it runs, so I cannot read /trav at the same time - that
# is normal.
# ===================================================================================================
param(
  [string]$Board = '192.168.4.1',
  [string]$Out   = 'beatlog.csv',
  [int]$EveryMs  = 2000
)

$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$outPath = Join-Path $here $Out

Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host '  BEAT LOG RECORDER' -ForegroundColor Cyan
Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host ('  Board: ' + $Board + '   every ' + $EveryMs + ' ms   ->  ' + $Out) -ForegroundColor Gray
Write-Host '  Play the track normally. Leave this running while you listen.' -ForegroundColor Gray
Write-Host '  Press Q in this window to stop.' -ForegroundColor Gray
Write-Host ''

# the file starts fresh each run, with its header
[System.IO.File]::WriteAllText($outPath, "ms,bar,sn,md`r`n", (New-Object System.Text.UTF8Encoding($false)))
$lastMs = -1
$rows = 0
$polls = 0
$fails = 0

function ReadBeats {
  try {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.Connect($Board, 80)
    $s = $c.GetStream()
    $req = [System.Text.Encoding]::ASCII.GetBytes("GET /beats HTTP/1.0`r`nHost: $Board`r`nConnection: close`r`n`r`n")
    $s.Write($req, 0, $req.Length)
    $sb = New-Object System.Text.StringBuilder
    $buf = New-Object byte[] 8192
    while (($n = $s.Read($buf, 0, 8192)) -gt 0) { [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n)) }
    $c.Close()
    return $sb.ToString()
  } catch {
    return $null
  }
}

while ($true) {
  if ([Console]::KeyAvailable) { $k = [Console]::ReadKey($true); if ($k.Key -eq 'Q') { break } }
  $txt = ReadBeats
  $polls++
  if ($null -eq $txt) {
    $fails++
    if ($fails -eq 1 -or ($fails % 10) -eq 0) { Write-Host ('  read failed (' + $fails + ') - is the wifi joined?') -ForegroundColor Red }
    Start-Sleep -Milliseconds $EveryMs
    continue
  }
  $fails = 0
  # MERGE: keep only the beats NEWER than the last one saved, so successive snapshots chain together
  $add = New-Object System.Text.StringBuilder
  $new = 0
  foreach ($ln in ($txt -split "`n")) {
    if ($ln -notmatch '^[0-9]') { continue }
    $f = $ln -split ','
    $ms = [int]$f[0]
    if ($ms -le $lastMs) { continue }
    [void]$add.AppendLine(($ms.ToString() + ',' + $f[1] + ',' + $f[2] + ',' + $f[3]))
    $lastMs = $ms
    $new++
    $rows++
  }
  if ($new -gt 0) {
    [System.IO.File]::AppendAllText($outPath, $add.ToString(), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ('  +{0,-3} beats   total {1,-5}   latest board ms {2}' -f $new, $rows, $lastMs) -ForegroundColor Green
  }
  Start-Sleep -Milliseconds $EveryMs
}

Write-Host ''
Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host ('  STOPPED.  {0} beats recorded to {1}  ({2} polls)' -f $rows, $Out, $polls) -ForegroundColor Cyan
Write-Host '  Tell me when you are done and I will read the file.' -ForegroundColor Gray
Write-Host '=====================================================================' -ForegroundColor Cyan