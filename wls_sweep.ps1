# ---------------------------------------------------------------------------
# THRESHOLD SWEEP - changes the Chase's Bass Sensitivity DURING playback, so one
# track tests several thresholds instead of one per run.
#
#   powershell -ExecutionPolicy Bypass -File wls_sweep.ps1 -Label t7 -Plan "0.083@0,0.25@100,0.375@200,0.5@300"
#
# The plan is "sensitivity@seconds, ..." measured from the MOMENT THE MUSIC STARTS - and
# the start is found by waiting for the board first onset, so nothing has to be
# synchronised by hand. Every step is logged with its exact board time, and the onset
# record carries thr= while the trace carries npk=, so the fraction in force at each beat
# is recoverable afterwards.
#
# The payload carries the FULL parameter set for the Chase, because /live-tweak writes
# every parameter it knows about and would otherwise reset Wrap to false and lose the
# metronome. Only beatSens is deliberately different.
# ---------------------------------------------------------------------------
param(
  [Parameter(Mandatory = $true)][string]$Label,
  [Parameter(Mandatory = $true)][string]$Plan
)
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$ip   = '192.168.4.1'
$log  = Join-Path $here ("beat_${Label}_sweep.log")
Remove-Item $log -ErrorAction SilentlyContinue
function Say([string]$m) {
  $l = '{0}  {1}' -f (Get-Date).ToString('HH:mm:ss.fff'), $m
  Add-Content -Path $log -Value $l
}
function GetDiag {
  try { return ((Invoke-WebRequest -Uri ('http://' + $ip + '/diag') -UseBasicParsing -TimeoutSec 5).Content | ConvertFrom-Json) } catch { return $null }
}
Say ("sweep start label={0} plan={1}" -f $Label, $Plan)
# wait for the music (up to 5 minutes) so the steps align with the track
$base = -1
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt 300) {
  $o = GetDiag
  if ($null -ne $o) {
    $h = [int]$o.onsHead
    if ($base -lt 0) { $base = $h }
    if ($h -gt $base) { Say ("music detected at onboard ms={0} (onsHead {1} to {2})" -f $o.uptimeSec, $base, $h); break }
  }
  Start-Sleep -Seconds 2
}
$t0 = Get-Date
foreach ($step in $Plan.Split(',')) {
  $p = $step.Split('@')
  $sens = [double]$p[0]
  $at = [double]$p[1]
  $wait = $at - ((Get-Date) - $t0).TotalSeconds
  if ($wait -gt 0) { Start-Sleep -Seconds $wait }
  $sv = $sens.ToString('0.###', [Globalization.CultureInfo]::InvariantCulture)
  $json = '{"section":"LED","name":"Chase","speed":1,"quantity":1,"size":5,"frequency":61,"beatMult":1,"attack":0.15,"decay":0.01,"wrap":true,"invert":false,"beatSens":' + $sv + '}'
  $enc = [uri]::EscapeDataString($json)
  try {
    $r = Invoke-WebRequest -Uri ('http://' + $ip + '/live-tweak?plain=' + $enc) -Method POST -UseBasicParsing -TimeoutSec 8
    $o = GetDiag
    Say ("step sens={0} at t={1:F1}s  http={2}  onboard sens={3} thr={4} novPk={5}" -f $sens, ((Get-Date) - $t0).TotalSeconds, $r.StatusCode, $o.p0sens, [math]::Round([double]$o.p0thr,1), [math]::Round([double]$o.novPk,1))
  } catch { Say ("FAILED sens={0}: {1}" -f $sens, $_.Exception.Message) }
}
Say 'sweep done'