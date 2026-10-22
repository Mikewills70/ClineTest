# ===================================================================================================
# *** TEMPORARY BEAT-1 TAP - REMOVE THIS FILE FROM THE LAPTOP WHEN THE TESTING PHASE IS OVER ***
# ===================================================================================================
# USER, 22 Oct: "Ensure you remove the script from the laptop once this testing phase is over."
#
# WHY THE MOUSE, NOT THE SPACEBAR: "The audio is playing from my laptop and the space bar acts as a pause
# button. We cannot use that either. If I move my mouse button off-screen and click the left button, it
# does not stop the music or affect any programs. Can you detect a LB push on the mouse instead?"
# Yes - the left button's state is readable DIRECTLY and GLOBALLY, so it works wherever the pointer is and
# whatever has focus. No window needs focus, no hook is installed, and nothing else is affected.
#
# WHY THE TIMESTAMP IS SENT: measured, the board's web server answers anywhere from 70 to 1300 ms after a
# request, because it only replies when its main loop reaches it. A press timestamped on ARRIVAL would be
# up to two beats late. So the press time is taken HERE, sent as ?t=<ms>, and the board self-calibrates
# the offset between the two clocks from the median of the recent taps.
#
# WHAT A PRESS DOES: the board finds the nearest beat it has ALREADY DETECTED and LABELS it as beat 1.
# THE USER WAS EXPLICIT: "The timing must come from the beats only, nothing to do when I hit the
# spacebar!!!" So this never sets, adjusts or influences any timing - it only says WHICH beat.
#
# HOW TO USE: run it, play the track, and LEFT-CLICK on every beat 1. The pointer can sit anywhere - off
# the edge of the screen is fine.
#
# TO REMOVE: delete this file. Nothing on the board depends on it.
# ===================================================================================================
param(
  [string]$Board = '192.168.4.1',
  [int]$PollMs = 4
)

$ErrorActionPreference = 'Continue'

Add-Type -Namespace W32 -Name Btn -MemberDefinition @'
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
'@

function NowMs { return [int]([Environment]::TickCount -band 0x7FFFFFFF) }

# THE PRESS LOG AND THE CLOCK SYNC (22 Oct). The press log records EVERY press the moment it happens, on
# the LAPTOP's clock - that is the whole point: the late reply on screen cannot corrupt it. The sync asks
# the board for its own millis() and notes the laptop's time either side of the request, which is the
# shared reference the two logs are aligned by afterwards.
$script:logCsv  = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'beat1_taps_LABEL.csv'
$script:syncCsv = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'beat1_sync_LABEL.csv'
$script:syncN   = 0

function SyncAt {
  $w1 = [DateTime]::Now
  try {
    $raw = (Invoke-WebRequest ("http://" + $Board + "/now") -UseBasicParsing -TimeoutSec 12).Content.Trim()
    $w2 = [DateTime]::Now
    $board = [int]$raw
    # NOTE: this board's web server is slow to REPLY (measured 70-1300 ms), and that delay is one-sided -
    # it is the server reaching the request, not the network. So the board's millis() is best matched to
    # the moment the REPLY arrived (w2), not the midpoint. Averaging several samples takes the worst of it
    # out; the remaining bias is roughly half the typical delay, well inside the quarter beat the
    # alignment needs.
    Add-Content -Path $script:syncCsv -Value ("{0},{1}" -f $board, $w2.ToString('yyyy-MM-dd HH:mm:ss.fff'))
    return $board
  } catch {
    return -1
  }
}

# Several samples, so one slow reply cannot set the reference on its own.
function Sync-Once {
  $done = 0
  for ($k = 0; $k -lt 5; $k++) {
    if ((SyncAt) -ge 0) { $done++ }
    Start-Sleep -Milliseconds 150
  }
  $script:syncN = $done
  if ($done -eq 0) { return -1 }
  return 0
}

function Send-Tap([int]$tMs) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  try {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.Connect($Board, 80)
    $s = $c.GetStream()
    $req = [System.Text.Encoding]::ASCII.GetBytes("GET /tap?t=$tMs HTTP/1.0`r`nHost: $Board`r`nConnection: close`r`n`r`n")
    $s.Write($req, 0, $req.Length)
    $sb = New-Object System.Text.StringBuilder
    $buf = New-Object byte[] 512
    while (($n = $s.Read($buf, 0, 512)) -gt 0) { [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n)) }
    $c.Close()
    $body = ($sb.ToString() -split "`r`n`r`n")[-1].Trim()
    return @{ ok = $true; body = $body; ms = $sw.ElapsedMilliseconds }
  } catch {
    return @{ ok = $false; body = $_.Exception.Message; ms = $sw.ElapsedMilliseconds }
  }
}


Clear-Host
Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host '  BEAT-1 TAP  -  LEFT-CLICK on every BEAT 1' -ForegroundColor Cyan
Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host '  TEMPORARY TEST TOOL. Remove this file when testing is over.' -ForegroundColor Yellow
Write-Host ''
Write-Host ('  Board: ' + $Board + '   polling every ' + $PollMs + ' ms') -ForegroundColor Gray
Write-Host '  The pointer can sit ANYWHERE - off the edge of the screen is fine.' -ForegroundColor Gray
Write-Host '  Nothing else is affected and no window needs focus.' -ForegroundColor Gray
Write-Host '  Each click labels the NEAREST DETECTED BEAT as beat 1.' -ForegroundColor Gray
Write-Host '  It never changes the timing - labelling only.' -ForegroundColor Gray
Write-Host ''
Write-Host '  Press Q in this window to quit.' -ForegroundColor Gray
Write-Host ''

$probe = Send-Tap (NowMs)
if (-not $probe.ok) {
  Write-Host ('  WARNING: the board did not answer (' + $probe.body + ').') -ForegroundColor Red
  Write-Host '  Rejoin the ESP32-LightShow wifi, then run this again.' -ForegroundColor Red
  Write-Host ''
} else {
  Write-Host ('  Board answered: ' + $probe.body) -ForegroundColor DarkGreen
  Write-Host ''
}


$n = 0
$agree = 0
$dis = 0
$stray = 0
while ($true) {
  if ([Console]::KeyAvailable) {
    $k = [Console]::ReadKey($true)
    if ($k.Key -eq 'Q') { break }
  }
  $state = [W32.Btn]::GetAsyncKeyState(0x01)      # VK_LBUTTON
  $isDown = (($state -band 0x8000) -ne 0)
  if ($isDown -and -not $down) {
    $n++
    # THE PRESS TIME IS TAKEN HERE, BEFORE THE SEND (22 Oct). The reply may arrive a second later; that
    # cannot touch this line. See the note at the top of the file.
    $pressTick = NowMs
    $pressWall = [DateTime]::Now
    Add-Content -Path $script:logCsv -Value ("{0},{1},{2}" -f $n, $pressTick, $pressWall.ToString('HH:mm:ss.fff'))
    $r = Send-Tap $pressTick
    $stamp = (Get-Date -Format 'HH:mm:ss.fff')
    if (-not $r.ok) {
      Write-Host ("  {0}  #{1,-3}  NO ANSWER ({2} ms)" -f $stamp, $n, $r.ms) -ForegroundColor Red
    } elseif ($r.body -like '*stray*') {
      $stray++
      Write-Host ("  {0}  #{1,-3}  ----   no beat near that click ({2} ms)" -f $stamp, $n, $r.ms) -ForegroundColor DarkYellow
    } elseif ($r.body -like '*AGREE*') {
      $agree++
      Write-Host ("  {0}  #{1,-3}  BEAT 1  board agrees ({2} ms)" -f $stamp, $n, $r.ms) -ForegroundColor Green
    } else {
      $dis++
      Write-Host ("  {0}  #{1,-3}  beat 1  board says: {2} ({3} ms)" -f $stamp, $n, $r.body, $r.ms) -ForegroundColor Magenta
    }
    Write-Host ("           tally: agree {0}   disagree {1}   stray {2}" -f $agree, $dis, $stray) -ForegroundColor Gray
  }
  $down = $isDown
  Start-Sleep -Milliseconds $PollMs
}

Write-Host ''
Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host ("  DONE.  clicks {0}   agree {1}   disagree {2}   stray {3}" -f $n, $agree, $dis, $stray) -ForegroundColor Cyan
Write-Host '  If agree is close to the click count, the detector picks the same beat as your ear.' -ForegroundColor Gray
Write-Host ''
Write-Host '  *** REMINDER: REMOVE THIS SCRIPT FROM THE LAPTOP - the testing phase is over. ***' -ForegroundColor Yellow
Write-Host '=====================================================================' -ForegroundColor Cyan