# ---------------------------------------------------------------------------
# THE BACKGROUND SAMPLER (21 Oct 2026, v2) - runs while nothing else does.
#
#   powershell -ExecutionPolicy Bypass -File wls_sampler.ps1 -Label t1 -Minutes 8
#
# v2 EXISTS BECAUSE v1 LOST 54% OF THE RECORD. Run 1 on Daft Punk showed onsHead
# reaching 8811 (801 distinct onsets) while the file held only 370: the detector fires
# in bursts of ~10 onsets/s and 11 listeners write 11 entries each, so a 5 s drain
# overflowed the 512-entry ring. v2 drains /onsets AND /trace EVERY pass with max=300
# (which covers a 2 s pass even at the burst rate), and only asks for /diag every 5th
# pass because it is the big reply and is only the context around the beats.
#
# Files written beside this script (all gitignored):
#   beat_<Label>_diag.csv     /diag as a wide table: t,wall,<every key>
#   beat_<Label>_onsets.txt   /onsets, drained with a CURSOR so nothing is lost
#   beat_<Label>_trace.txt    /trace at 50 Hz, drained with a cursor
#   beat_<Label>_sampler.log  progress, and /tail at the end
#
# Stop early with beat_<Label>.stop; it also stops on the timeout.
# Windows drops an AP with no internet, so a failed fetch triggers a re-join.
# ---------------------------------------------------------------------------
param(
  [Parameter(Mandatory = $true)][string]$Label,
  [int]$Minutes  = 8,
  [int]$EverySec = 2
)
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$ssid = 'ESP32-LightShow'
$pass = '12345678'
$ip   = '192.168.4.1'
$csv  = Join-Path $here ("beat_${Label}_diag.csv")
$ons  = Join-Path $here ("beat_${Label}_onsets.txt")
$trc  = Join-Path $here ("beat_${Label}_trace.txt")
$log  = Join-Path $here ("beat_${Label}_sampler.log")
$stop = Join-Path $here ("beat_${Label}.stop")
foreach ($f in @($csv, $ons, $trc, $log, $stop)) { Remove-Item $f -ErrorAction SilentlyContinue }

function Say([string]$m) {
  $line = '{0}  {1}' -f (Get-Date).ToString('HH:mm:ss'), $m
  Add-Content -Path $log -Value $line
  Write-Host $line
}
function CurSsid {
  $o = netsh wlan show interfaces | Out-String
  if ($o -match '(?m)^\s*SSID\s+:\s*(.+)$') { return $Matches[1].Trim() }
  return ''
}
function JoinAp {
  if ((CurSsid) -eq $ssid) { return $true }
  $xml = '<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1"><name>' + $ssid + '</name><SSIDConfig><SSID><name>' + $ssid + '</name></SSID></SSIDConfig><connectionType>ESS</connectionType><connectionMode>manual</connectionMode><MSM><security><authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption><sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>' + $pass + '</keyMaterial></sharedKey></security></MSM></WLANProfile>'
  $xml | Out-File "$env:TEMP\esp32_sampler_profile.xml" -Encoding ascii
  netsh wlan add profile filename="$env:TEMP\esp32_sampler_profile.xml" user=current | Out-Null
  netsh wlan connect name="$ssid" ssid="$ssid" | Out-Null
  for ($i = 0; $i -lt 12; $i++) {
    Start-Sleep -Milliseconds 700
    if ((CurSsid) -eq $ssid) { return $true }
  }
  return $false
}
function Fetch([string]$path) {
  for ($try = 0; $try -lt 2; $try++) {
    try { return (Invoke-WebRequest -Uri ('http://' + $ip + $path) -UseBasicParsing -TimeoutSec 8).Content }
    catch { Start-Sleep -Milliseconds 300 }
  }
  return $null
}

$t0 = Get-Date
$deadline = $t0.AddMinutes($Minutes)
Say ("sampler v2 start label={0} minutes={1}" -f $Label, $Minutes)
if (JoinAp) { Say 'joined the board AP' } else { Say 'WARNING: could not join the AP yet - still trying' }

$hdr = $null
$onsCursor = 0
$trcCursor = 0
$polls = 0
$fails = 0
$onsLines = 0
$trcLines = 0
$lastSay = Get-Date
$onsBuf = New-Object System.Collections.ArrayList
$trcBuf = New-Object System.Collections.ArrayList

while (((Get-Date) -lt $deadline) -and (-not (Test-Path $stop))) {
  $polls++

  $on = Fetch ('/onsets?since=' + $onsCursor + '&max=60')
  if ($null -ne $on) {
    foreach ($ln in ($on -split "`n")) {
      if ($ln -like '# onsets*') { if ($ln -match 'next=(\d+)') { $onsCursor = [int]$Matches[1] } }
      elseif ($ln -match '^ms=') { [void]$onsBuf.Add($ln.Trim()); $onsLines++ }
    }
  } else { $fails++ }

  $tr = Fetch ('/trace?since=' + $trcCursor + '&max=60')
  if ($null -ne $tr) {
    foreach ($ln in ($tr -split "`n")) {
      if ($ln -like '# trace*') { if ($ln -match 'next=(\d+)') { $trcCursor = [int]$Matches[1] } }
      elseif ($ln -match '^ms=') { [void]$trcBuf.Add($ln.Trim()); $trcLines++ }
    }
  } else { $fails++ }

  if (($polls % 8) -eq 1) {
    $d = Fetch '/diag'
    if ($null -ne $d) {
      $o = $null
      try { $o = $d | ConvertFrom-Json } catch { $o = $null }
      if ($null -ne $o) {
        $keys = @($o.PSObject.Properties.Name | Sort-Object)
        if ($null -eq $hdr) {
          $hdr = @('t', 'wall') + $keys
          Add-Content -Path $csv -Value ($hdr -join ',')
          Say ("diag header captured: {0} keys" -f $keys.Count)
        }
        $tSec = ((Get-Date) - $t0).TotalSeconds
        $vals = @($tSec.ToString('F2'), (Get-Date).ToString('HH:mm:ss.fff'))
        foreach ($k in $keys) { $vals += ([string]$o.$k) }
        Add-Content -Path $csv -Value ($vals -join ',')
      }
    } else { $fails++ }
  }

  if ($onsBuf.Count -gt 0) { Add-Content -Path $ons -Value $onsBuf.ToArray(); $onsBuf.Clear() }
  if ($trcBuf.Count -gt 0) { Add-Content -Path $trc -Value $trcBuf.ToArray(); $trcBuf.Clear() }

  if (((Get-Date) - $lastSay).TotalSeconds -ge 60) {
    $lastSay = Get-Date
    Say ("progress: polls={0} onsetLines={1} traceLines={2} onsCursor={3} trcCursor={4} fails={5}" -f $polls, $onsLines, $trcLines, $onsCursor, $trcCursor, $fails)
  }
  if ($fails -ge 6) { if (JoinAp) { Say 're-joined the AP' }; $fails = 0 }
  Start-Sleep -Milliseconds 250
}
$tail = Fetch '/tail'
if ($null -ne $tail) { $tail -split "`n" | Where-Object { $_.Trim() -ne '' } | ForEach-Object { Add-Content -Path $log -Value ('TAIL  ' + $_) } }
$lg = Fetch '/log.txt'
if ($null -ne $lg) { $lg -split "`n" | Where-Object { $_.Trim() -ne '' } | Select-Object -Last 60 | ForEach-Object { Add-Content -Path $log -Value ('LOG   ' + $_) } }
Say ("sampler done: polls={0} onsetLines={1} traceLines={2} onsCursor={3} trcCursor={4}" -f $polls, $onsLines, $trcLines, $onsCursor, $trcCursor)