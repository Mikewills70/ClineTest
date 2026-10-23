# ===================================================================================================
# ONSET LOG RECORDER - DEVELOPMENT TOOL
# ===================================================================================================
# WHY: the board's onset ring holds only 1024 entries - about 9 seconds at the 10 onsets/s burst rate - so
# a run must be captured as it happens. This follows /onsets with its own cursor (the reply's `next=`
# field), so nothing is missed and nothing is duplicated.
# WHAT IT CAPTURES, per onset: the LEADING edge (ms=), the TRAILING edge (fall=) and the MID POINT
# (mid=) the user asked for, plus the band readings. That makes the midpoint idea measurable directly
# instead of inferred.
# HOW TO USE: run it, then play the track. Leave it running while you listen. Press Q to stop.
# ===================================================================================================
param(
  [string]$Board = '192.168.4.1',
  [string]$Out   = 'onsetlog.txt',
  [int]$EveryMs  = 400
)

$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$outPath = Join-Path $here $Out

Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host '  ONSET LOG RECORDER  -  leading edge, trailing edge, mid point' -ForegroundColor Cyan
Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host ('  Board: ' + $Board + '   every ' + $EveryMs + ' ms   ->  ' + $Out) -ForegroundColor Gray
Write-Host '  Play the track normally. Leave this running while you listen.' -ForegroundColor Gray
Write-Host '  Press Q in this window to stop.' -ForegroundColor Gray
Write-Host ''

[System.IO.File]::WriteAllText($outPath, "# ms=leading edge  fall=trailing edge  mid=midpoint`r`n", (New-Object System.Text.UTF8Encoding($false)))
$cursor = 0
$rows = 0
$polls = 0
$fails = 0

function GetOnsets([uint32]$since) {
  try {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.Connect($Board, 80)
    $s = $c.GetStream()
    $req = [System.Text.Encoding]::ASCII.GetBytes("GET /onsets?since=$since&max=150 HTTP/1.0`r`nHost: $Board`r`nConnection: close`r`n`r`n")
    $s.Write($req, 0, $req.Length)
    $sb = New-Object System.Text.StringBuilder
    $buf = New-Object byte[] 16384
    while (($n = $s.Read($buf, 0, 16384)) -gt 0) { [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n)) }
    $c.Close()
    return $sb.ToString()
  } catch {
    return $null
  }
}

while ($true) {
  if ([Console]::KeyAvailable) { $k = [Console]::ReadKey($true); if ($k.Key -eq 'Q') { break } }
  $txt = GetOnsets $cursor
  $polls++
  if ($null -eq $txt) {
    $fails++
    if ($fails -eq 1 -or ($fails % 10) -eq 0) { Write-Host ('  read failed (' + $fails + ') - is the wifi joined?') -ForegroundColor Red }
    Start-Sleep -Milliseconds $EveryMs
    continue
  }
  $fails = 0
  # the reply's header carries the cursor to come back with
  if ($txt -match 'next=(\d+)') { $cursor = [uint32]$Matches[1] }
  $add = New-Object System.Text.StringBuilder
  $new = 0
  foreach ($ln in ($txt -split "`n")) {
    $ln = $ln.Trim()
    if ($ln -notmatch '^ms=') { continue }
    [void]$add.AppendLine($ln)
    $new++
    $rows++
  }
  if ($new -gt 0) {
    [System.IO.File]::AppendAllText($outPath, $add.ToString(), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ('  +{0,-3} onsets   total {1,-5}   cursor {2}' -f $new, $rows, $cursor) -ForegroundColor Green
  }
  Start-Sleep -Milliseconds $EveryMs
}

Write-Host ''
Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host ('  STOPPED.  {0} onsets recorded to {1}  ({2} polls)' -f $rows, $Out, $polls) -ForegroundColor Cyan
Write-Host '  Tell me when you are done and I will read the file.' -ForegroundColor Gray
Write-Host '=====================================================================' -ForegroundColor Cyan