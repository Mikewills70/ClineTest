# ===================================================================================================
# BEAT MONITOR - watches the music gate and the clock, so a missed or late flash is RECORDED, not
# remembered. Built 23 Oct 2026 at the user's request: "run a powershell so I can identify which beat
# was missed". It never relies on reaction time - the board's own counters carry the precision.
# ===================================================================================================
# WHAT IT IS LOOKING AT, and why each field matters:
#   p0lst   ms since listener 0's last REAL onset. THIS IS THE MUSIC GATE'S OWN INPUT.
#           musicNow = !(lastReal == 0 || (now - lastReal) > 1500)  - so when p0lst passes 1500 the
#           gate closes and the flash is SUPPRESSED, by design.
#   io48    IO48 flashes since boot - one per MUSIC beat.
#   p0clk   beats the CLOCK delivered. A GAP between io48 and p0clk IS the music gate working.
#   p0iv    the period the clock is running at. 2000 means it never left the IDLE PACE (the Speed
#           slider), which would show as a consistently SLOW flash that never corrects.
#   bOns/bActs  onsets seen / real beats delivered.
#   wid, conf, fitBpm, fitAcc, fitRej, locked - the FIT's quality: WHY onsets were missed.
#   odfHz, fLedUs, fLoopUs - the detector frame rate and the load that may be degrading it.
#
# HOW TO USE:  powershell -ExecutionPolicy Bypass -File .\wls_beatmon.ps1
#   Play the track. Press Q to stop. If you hear a missed or late flash, note the rough time - the
#   counters give the precision, your ear confirms we are looking at the right moment.
#
# IT IS DELIBERATELY LIGHT. The board's web server is single-client and slow (70-1300 ms), so every
# request is SEQUENTIAL and the rate is modest. HONEST CAVEAT: measuring loads the same CPU that
# detects the beat, so fLedUs / fLoopUs are recorded to let us see if the monitor itself changes the
# symptom. Nothing is written to the board and no firmware is touched.
# ===================================================================================================
param(
  [string]$Board   = '192.168.4.1',
  [string]$Out     = 'beatmon.csv',
  [string]$OnsOut  = 'beatmon_onsets.txt',
  [int]$EveryMs    = 1000,
  [int]$OnsEveryMs = 5000
)

$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$outPath = Join-Path $here $Out
$onsPath = Join-Path $here $OnsOut
$enc = New-Object System.Text.UTF8Encoding($false)

Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host '  BEAT MONITOR' -ForegroundColor Cyan
Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host ('  board ' + $Board + '   diag/trav every ' + $EveryMs + ' ms   onsets every ' + $OnsEveryMs + ' ms') -ForegroundColor Gray
Write-Host ('  -> ' + $Out + '  and  ' + $OnsOut) -ForegroundColor Gray
Write-Host '  Play the track. Press Q in this window to stop.' -ForegroundColor Gray
Write-Host ''

function Get-Text([string]$path) {
  try {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.Connect($Board, 80)
    $s = $c.GetStream()
    $req = [System.Text.Encoding]::ASCII.GetBytes("GET $path HTTP/1.0`r`nHost: $Board`r`nConnection: close`r`n`r`n")
    $s.Write($req, 0, $req.Length)
    $sb = New-Object System.Text.StringBuilder
    $buf = New-Object byte[] 8192
    while (($n = $s.Read($buf, 0, 8192)) -gt 0) { [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n)) }
    $c.Close()
    $txt = $sb.ToString()
    $i = $txt.IndexOf("`r`n`r`n")
    if ($i -ge 0) { return $txt.Substring($i + 4) }
    return $txt
  } catch { $script:lastNetErr = $_.Exception.Message; return $null }
}

[System.IO.File]::WriteAllText($outPath, "t,io48,p0clk,p0lst,p0iv,bOns,bActs,led48,displayActive,odfHz,fLedUs,fLoopUs,wid,conf,fitBpm,fitAcc,fitRej,locked,freeHeap`r`n", $enc)
[System.IO.File]::WriteAllText($onsPath, "# raw /onsets lines, as fetched. Each fetch is preceded by a cursor marker.`r`n", $enc)

$rows = 0; $onsCursor = 0; $lastOns = 0; $lastFlash = -1; $lastClk = -1; $fails = 0; $script:lastNetErr = ''
$started = Get-Date

while ($true) {
  if ([Console]::KeyAvailable) { $k = [Console]::ReadKey($true); if ($k.Key -eq 'Q') { break } }

  $dTxt = Get-Text '/diag'
  if ($null -eq $dTxt) {
    # SILENT FAILURE WAS THE BUG (24 Oct 2026). This used to continue with no output at all, so a run
    # where the board was never reached looked exactly like a run where nothing happened: a blank window
    # and a CSV with only its header. Now it says so, and says what to check.
    $fails++
    if ($fails -eq 1 -or ($fails % 5) -eq 0) {
      Write-Host ('  *** CANNOT REACH THE BOARD at ' + $Board + ':80 - ' + $fails + ' attempts') -ForegroundColor Red
      if ($script:lastNetErr) { Write-Host ('      ' + $script:lastNetErr) -ForegroundColor Red }
      Write-Host '      is the board powered, and on the same network as this PC?' -ForegroundColor Red
      Write-Host '      THE BOARD SERVES ONE CLIENT AT A TIME - close its web page, then start this again.' -ForegroundColor Red
      Write-Host '      if the board is on your home network, pass its address: -Board 192.168.1.50' -ForegroundColor Red
    }
    Start-Sleep -Milliseconds $EveryMs; continue
  }
  if ($fails -gt 0) {
    Write-Host ('  board reached again (after ' + $fails + ' failed attempts).') -ForegroundColor Green
    $fails = 0
  }
  $tTxt = Get-Text '/trav'
  try { $d = $dTxt | ConvertFrom-Json } catch { Start-Sleep -Milliseconds $EveryMs; continue }
  $v = $null
  if ($tTxt) { try { $v = $tTxt | ConvertFrom-Json } catch { $v = $null } }

  $t = (Get-Date).ToString('HH:mm:ss')
  $row = @(
    $t,
    [string]$d.io48, [string]$d.p0clk, [string]$d.p0lst, [string]$d.p0iv,
    [string]$d.bOns, [string]$d.bActs, [string]$d.led48, [string]$d.displayActive,
    [string]$d.odfHz, [string]$d.fLedUs, [string]$d.fLoopUs,
    $(if ($v) { [string]$v.wid } else { '' }),
    $(if ($v) { [string]$v.conf } else { '' }),
    $(if ($v) { [string]$v.fitBpm } else { '' }),
    $(if ($v) { [string]$v.fitAcc } else { '' }),
    $(if ($v) { [string]$v.fitRej } else { '' }),
    $(if ($v) { [string]$v.locked } else { '' }),
    [string]$d.freeHeap
  ) -join ','
  [System.IO.File]::AppendAllText($outPath, ($row + "`r`n"), $enc)
  $rows++

  # LIVE WARNINGS - the three things we are hunting, flagged as they happen
  # p0lst can WRAP: lastReal is sometimes a few ms AHEAD of the reader,s millis(), so now-lastReal goes
  # negative and prints as ~4294967287. Unwrap it so the warning logic stays sane.
  $fl = [int]$d.io48; $ck = [int]$d.p0clk; $iv = [int]$d.p0iv
  $lstL = [int64]$d.p0lst
  if ($lstL -gt 2147483647) { $lstL = $lstL - 4294967296 }
  $lst = [int]$lstL
  $lstWrap = ($lstL -lt 0)
  $da = [int]$d.displayActive
  $note = ''
  if ($lstWrap) { $note += ('  *** p0lst WRAPPED (' + $lstL + 'ms) - lastReal AHEAD of now, which also makes musicNow read FALSE ***') }
  if ($lastFlash -ge 0 -and $ck -gt $lastClk -and $fl -eq $lastFlash) { $note += '  *** CLOCK BEAT WITH NO FLASH ***' }
  if ($lst -gt 1500 -and $da -eq 1) { $note += '  *** MUSIC GATE CLOSED (p0lst>1500) ***' }
  if ($iv -gt 1500 -and $da -eq 1) { $note += '  *** IDLE PACE: clock not locked to music ***' }
  # THE DRIFT WARNING - this is the one for "very slightly slower than the beat, drifting out and back".
  # The clock runs at p0iv; the fit says the tempo is fitBpm. If those two disagree, the clock is
  # running at the wrong period and the phase WILL slide - slowly out of time and, if the error changes
  # sign, back into it. 10 ms is the threshold because fitBpm is a whole number, so 60000/fitBpm is
  # quantised to a few ms on its own.
  if ($v -and $da -eq 1 -and [int]$v.fitBpm -gt 0) {
    $fitIv = [int](60000 / [int]$v.fitBpm)
    $gap = [Math]::Abs($iv - $fitIv)
    if ($gap -gt 10) { $note += ('  *** CLOCK ' + $iv + 'ms vs FIT ' + $fitIv + 'ms (off by ' + $gap + 'ms) ***') }
  }
  $lastFlash = $fl; $lastClk = $ck

  $bpm = 0
  if ($iv -gt 0) { $bpm = [int](60000 / $iv) }
  $line = '  {0}  io48={1,-6} p0clk={2,-6} p0lst={3,-6} p0iv={4,-5} bpm~{5,-4}{6}' -f $t, $fl, $ck, $lst, $iv, $bpm, $note
  if ($note -ne '') { Write-Host $line -ForegroundColor Yellow } else { Write-Host $line -ForegroundColor Gray }

  # the onset ring, with a cursor so nothing is ever lost
  if (((Get-Date) - $started).TotalMilliseconds - $lastOns -ge $OnsEveryMs) {
    $lastOns = ((Get-Date) - $started).TotalMilliseconds
    $oTxt = Get-Text ('/onsets?since=' + $onsCursor)
    if ($oTxt) {
      [System.IO.File]::AppendAllText($onsPath, ('# fetch at ' + $t + '  since=' + $onsCursor + "`r`n"), $enc)
      [System.IO.File]::AppendAllText($onsPath, ($oTxt + "`r`n"), $enc)
      if ($oTxt -match 'next=(\d+)') { $onsCursor = [int]$Matches[1] }
    }
  }

  Start-Sleep -Milliseconds $EveryMs
}

Write-Host ''
Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host ('  STOPPED.  {0} rows -> {1}' -f $rows, $Out) -ForegroundColor Cyan
Write-Host ('  onsets -> {0}   (cursor reached {1})' -f $OnsOut, $onsCursor) -ForegroundColor Cyan
Write-Host '  Tell me when you are done and I will read both files.' -ForegroundColor Gray
Write-Host '=====================================================================' -ForegroundColor Cyan