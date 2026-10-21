# ---------------------------------------------------------------------------
# WLS audio probe: join the board's AP, read /settings and /diag, and POST
# parameters. Used by the beat-detection exercise to drive the hardware
# without the Arduino IDE (which must never touch COM5).
#
#   powershell -ExecutionPolicy Bypass -File wls_probe.ps1 -Mode read
#   powershell -ExecutionPolicy Bypass -File wls_probe.ps1 -Mode setup
#   powershell -ExecutionPolicy Bypass -File wls_probe.ps1 -Mode trace
#
# Short bursts only: Windows drops an access point with no internet, so every
# call re-joins first.
# ---------------------------------------------------------------------------
param(
  [ValidateSet('read','setup','trace','config')]
  [string]$Mode = 'read',
  [int]$Freq = 61,
  [double]$Sens = 0.08,
  [int]$Id = 16
)
$ErrorActionPreference = 'Continue'
$base = 'http://192.168.4.1'

function Join-Ap {
  netsh wlan connect name="ESP32-LightShow" | Out-Null
  Start-Sleep -Seconds 6
}

function Get-Text($path, $timeout = 15) {
  try { return (Invoke-WebRequest "$base$path" -UseBasicParsing -TimeoutSec $timeout).Content }
  catch { return "FETCHFAIL: $($_.Exception.Message)" }
}

function Post-Json($path, $body) {
  try {
    Invoke-WebRequest "$base$path" -Method Post -Body $body -ContentType 'application/json' -UseBasicParsing -TimeoutSec 15 | Out-Null
    Write-Host "  POST $path ok"
  } catch { Write-Host "  POST $path FAIL: $($_.Exception.Message)" }
}

# /live-tweak takes its JSON as the ?plain= ARGUMENT, not as a request body - a body
# arrives as an empty plain= and the handler answers 400.
function Post-Tweak($body) {
  try {
    $enc = [uri]::EscapeDataString($body)
    Invoke-WebRequest "$base/live-tweak?plain=$enc" -Method Post -UseBasicParsing -TimeoutSec 15 | Out-Null
    Write-Host "  tweak ok"
  } catch { Write-Host "  tweak FAIL: $($_.Exception.Message)" }
}

Join-Ap

if ($Mode -eq 'read' -or $Mode -eq 'trace') {
  $s = Get-Text '/settings'
  Write-Host "SETTINGS: $($s.Substring(0, [Math]::Min(300, $s.Length)))"
  $p = Get-Text '/' 25
  Write-Host "PAGE len=$($p.Length) defMarkFreqLED=$($p -match 'defMarkFreqLED') snap=$($p -match 'audioSnapAndSend') follow=$($p -match 'audioFollowBoard') chip=$($p -match 'def-chip')"
  $j = Get-Text '/diag' 18 | ConvertFrom-Json
  Write-Host "DIAG pat=$($j.p0pat) p0iv=$($j.p0iv) p0clk=$($j.p0clk) p0sens=$($j.p0sens) v0wr=$($j.v0wr) display=$($j.displayActive) uptime=$($j.uptimeSec) boots=$($j.boots)"
  # THE DOT'S POSITION (sP0) and the parameters actually applied (v0*): the landing can be
  # verified from these without the user having to describe it.
  Write-Host "DOT sP0=$($j.sP0) sP1=$($j.sP1) sP2=$($j.sP2) sP3=$($j.sP3)  v0sz=$($j.v0sz) v0sp=$($j.v0sp) v0wr=$($j.v0wr) v0fq=$($j.v0fq)"
  Write-Host "AUDIO aBass=$($j.aBass) aBlvl=$($j.aBlvl) aEnv=$($j.aEnv) aSlow=$($j.aSlow) novPk=$($j.novPk) p0thr=$($j.p0thr) bThr=$($j.bThr)"
}

if ($Mode -eq 'setup') {
  Post-Tweak ('{"section":"LED","id":16,"beatSens":' + $Sens + ',"frequency":' + $Freq + ',"pattern":"Chase","audioSource":2}')
  Post-Tweak ('{"section":"LED","id":6,"beatSens":' + $Sens + ',"frequency":' + $Freq + ',"pattern":"Chase","audioSource":2}')
  Start-Sleep -Seconds 2
  Post-Json '/save-playlist' '{"items":[{"name":"BASS Beat","enableLED":true,"patternLED":"Chase","audioEnabledLED":true,"speedLED":1,"brightnessLED":255,"paletteLED":"White","musicLED":"Bass Level","freqLED":61,"qtyLED":100,"sizeLED":1,"multLED":1,"attackLED":0.05,"releaseLED":0.3,"enable240V":false,"audioEnabled240V":false,"antifreeze240V":0,"speed240V":1,"qty240V":1,"size240V":1,"freq240V":61,"mult240V":1,"attack240V":0.05,"release240V":0.3,"pattern240V":"Chase"}]}'
  Post-Json '/save-timeline' '{"bpm":127,"cells":[],"defaultMs":60000}'
  Start-Sleep -Seconds 3
  $j = Get-Text '/diag' 18 | ConvertFrom-Json
  Write-Host "AFTER SETUP pat=$($j.m1.pat) fre=$($j.m1.fre) pos=$($j.m1.pos) p0thr=$($j.p0thr) bThr=$($j.bThr) aBass=$($j.aBass)"
}

if ($Mode -eq 'config') {
  # The FULL parameter set: /live-tweak writes every parameter it knows about from the
  # payload, so a partial one would silently reset Wrap to false and lose the metronome.
  Post-Tweak ('{"section":"LED","name":"Chase","speed":1,"quantity":1,"size":5,"frequency":' +
              $Freq + ',"beatMult":1,"attack":0.15,"decay":0.01,"wrap":true,"invert":false,"beatSens":' + $Sens + '}')
  Start-Sleep -Seconds 2
  $j = Get-Text '/diag' 18 | ConvertFrom-Json
  Write-Host "AFTER CONFIG p0sens=$($j.p0sens) p0frac=$($j.p0frac) p0thr=$($j.p0thr) v0wr=$($j.v0wr) v0fq=$($j.v0fq) p0pat=$($j.p0pat) p0iv=$($j.p0iv) p0clk=$($j.p0clk) display=$($j.displayActive)"
}
