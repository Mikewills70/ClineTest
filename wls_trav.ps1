# ---------------------------------------------------------------------------
# WLS traverse poller. Samples /trav about once a second into a CSV, so an
# INTERMITTENT fault (the dot drifting to the centre of the strip) is captured
# whenever it happens rather than having to be caught by eye.
#
#   powershell -ExecutionPolicy Bypass -File wls_trav.ps1 -Label t1 -Minutes 30
#
# /trav is a tiny reply on its own endpoint, so polling it does not disturb the
# display the way /diag would. Windows drops an access point with no internet,
# so a failed read re-joins and retries; failures are logged as FAIL rows.
# ---------------------------------------------------------------------------
param(
  [string]$Label = 'trav',
  [int]$Minutes = 30,
  [int]$EveryMs = 1000
)
$ErrorActionPreference = 'Continue'
# A HIDDEN process has no console, and Invoke-WebRequest writes a progress bar to it - which BLOCKS the
# request until the 5 s timeout expires. That is why the poller logged nothing but FAIL rows while a
# direct read from a visible session worked fine.
$ProgressPreference = 'SilentlyContinue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$csv  = Join-Path $here ("beat_{0}_trav.csv" -f $Label)
$log  = Join-Path $here ("beat_{0}_trav.log" -f $Label)
$base = 'http://192.168.4.1'

function Say($m) {
  $s = (Get-Date -Format 'HH:mm:ss') + '  ' + $m
  Add-Content -Path $log -Value $s
  Write-Host $s
}

Say ("poller START label={0} every={1}ms for {2} min -> {3}" -f $Label, $EveryMs, $Minutes, $csv)
$end = (Get-Date).AddMinutes($Minutes)
$hdr = $false
$fails = 0
$rows = 0

while ((Get-Date) -lt $end) {
  try {
    $j = (Invoke-WebRequest "$base/trav" -UseBasicParsing -TimeoutSec 12).Content | ConvertFrom-Json
    $row = [ordered]@{
      t            = (Get-Date -Format 'HH:mm:ss')
      travN        = $j.travN
      travPer      = $j.travPer
      travDone     = $j.travDone
      travFromLast = $j.travFromLast
      travGapB     = $j.travGapB
      travG1       = $j.travG1
      travG2       = $j.travG2
      travG3p      = $j.travG3p
      p0iv         = $j.p0iv
      p0clk        = $j.p0clk
      multLED      = $j.multLED
      v0mu         = $j.v0mu
      v0wr         = $j.v0wr
      v0pat        = $j.v0pat
      v0sp         = $j.v0sp
      tlPlaying    = $j.tlPlaying
      tlLoaded     = $j.tlLoaded
      pageOwns     = $j.pageOwns
      idlePace     = $j.idlePace
      beatPos      = $j.beatPos
      beatPosMin   = $j.beatPosMin
      beatPosMax   = $j.beatPosMax
      swing        = $j.swing
      swingMin     = $j.swingMin
      onsPos       = $j.onsPos
      onsPosMin    = $j.onsPosMin
      onsPosMax    = $j.onsPosMax
      halfRun      = $j.halfRun
      halfRe       = $j.halfRe
      snOn         = $j.snOn
      snOff        = $j.snOff
      mdOn         = $j.mdOn
      mdOff        = $j.mdOff
      snNov        = $j.snNov
      mdNov        = $j.mdNov
      barPos       = $j.barPos
      barConf      = $j.barConf
      dbAct        = $j.dbAct
      beatOff      = $j.beatOff
      halfSh       = $j.halfSh
      dbBeat       = $j.dbBeat
      dbMid        = $j.dbMid
      dotPos       = $j.dotPos
      pllTrim      = $j.pllTrim
      clkPer       = $j.clkPer
      paceNow      = $j.paceNow
      paceTgt      = $j.paceTgt
      mBeats       = $j.mBeats
      settled      = $j.settled
      setRun       = $j.setRun
      arrRevs      = $j.arrRevs
      halfAge      = $j.halfAge
    }
    if (-not $hdr) { ($row.Keys -join ',') | Set-Content -Path $csv; $hdr = $true }
    ($row.Values -join ',') | Add-Content -Path $csv
    $rows++
    if ($fails -gt 0) { Say ("recovered after {0} failed read(s), {1} rows so far" -f $fails, $rows) }
    $fails = 0
  } catch {
    $fails++
    Add-Content -Path $csv -Value ((Get-Date -Format 'HH:mm:ss') + ',FAIL')
    if ($fails -eq 1 -or ($fails % 15) -eq 0) {
      Say ("read failed ({0}) - re-joining the board's AP" -f $fails)
      netsh wlan connect name="ESP32-LightShow" | Out-Null
    }
  }
  Start-Sleep -Milliseconds $EveryMs
}
Say ("poller DONE label={0} rows={1}" -f $Label, $rows)
