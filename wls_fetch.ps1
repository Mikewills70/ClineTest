# ===================================================================================================
# WLS FETCH - join the board's own access point and read it in ONE SHORT BURST.
# ===================================================================================================
# WHY THIS EXISTS. CODE_NOTES.md already had the answer, written down and then not read:
#
#   "READ over Wi-Fi from the PC: netsh wlan connect name="ESP32-LightShow" (the profile is already
#    saved), then fetch the board's own endpoints - /diag, /log.txt, ... SHORT BURSTS ONLY: Windows
#    drops an access point with no internet, so EACH FETCH NEEDS A FRESH JOIN, and long retry loops
#    exceed the command time limit."
#
# That is the whole story of the "unreliable connection". The board's access point has NO INTERNET, so
# Windows silently drops it. A single fetch immediately after a fresh join works every time; a fetch
# after any delay fails, and a retry loop just burns the command's time limit. So: join, then fetch
# once, immediately. Never trust a connection that is more than a few seconds old.
#
# USB SERIAL IS NOT AN OPTION on this hardware - the note says so and it is right: with USBMode=hwcdc
# the sketch's Serial needs DTR asserted, and asserting it resets the port, so the host sees the ROM
# banner and nothing else.
#
# HOW TO USE:
#   powershell -ExecutionPolicy Bypass -File .\wls_fetch.ps1
#   powershell -ExecutionPolicy Bypass -File .\wls_fetch.ps1 -Path /onsets
#   powershell -ExecutionPolicy Bypass -File .\wls_fetch.ps1 -Raw          (the raw body, no parsing)
# ===================================================================================================
param(
  [string]$Board   = '192.168.4.1',
  [string]$SSID    = 'ESP32-LightShow',
  [string]$Path    = '/diag',
  [int]$WaitSec    = 5,
  [switch]$Raw
)

$ErrorActionPreference = 'Continue'

function Join-Board {
  $p = netsh wlan show profiles | Select-String -Pattern ([regex]::Escape($SSID))
  if (-not $p) {
    Write-Host ('  PROFILE NOT SAVED: ' + $SSID) -ForegroundColor Red
    Write-Host '  Connect to it once from the Wi-Fi list and tick "connect automatically";' -ForegroundColor Red
    Write-Host '  Windows saves the profile and this script can then join it on demand.' -ForegroundColor Red
    return $false
  }
  $out = netsh wlan connect name="$SSID" 2>&1
  if ($out -match 'completed successfully') { Write-Host ('  joined ' + $SSID) -ForegroundColor DarkGray; return $true }
  Write-Host ('  join said: ' + ($out -join ' ')) -ForegroundColor Yellow
  return $true          # it often says nothing useful and has still worked; the fetch is the real test
}

Write-Host '=====================================================================' -ForegroundColor Cyan
Write-Host ('  WLS FETCH   ' + $Path) -ForegroundColor Cyan
Write-Host '=====================================================================' -ForegroundColor Cyan

if (-not (Join-Board)) { exit 1 }
Start-Sleep -Seconds $WaitSec

$body = $null
try {
  $r = Invoke-WebRequest -Uri ('http://' + $Board + $Path) -UseBasicParsing -TimeoutSec 12
  $body = $r.Content
  Write-Host ('  board reached, ' + $body.Length + ' bytes') -ForegroundColor Green
} catch {
  Write-Host ('  FETCH FAILED: ' + $_.Exception.Message) -ForegroundColor Red
  Write-Host '  If this is the first failure, run the command again - the join sometimes needs one retry.' -ForegroundColor Red
  Write-Host '  If it keeps failing, check the Wi-Fi list: if ESP32-LightShow is absent, power-cycle the board.' -ForegroundColor Red
  exit 1
}

if ($Raw) { Write-Host ''; Write-Output $body; exit 0 }

# ---- /diag gets the beat numbers, because those are what matter ----
if ($Path -eq '/diag') {
  try { $j = $body | ConvertFrom-Json } catch { Write-Host '  (not JSON)'; Write-Output $body; exit 0 }
  $P = [int]$j.fitPer
  Write-Host ''
  Write-Host '  ---- THE BEAT ----' -ForegroundColor White
  Write-Host ('  found     = ' + $j.beatFound + '   (1 = four valid intervals; the prediction is running)') -ForegroundColor $(if ([int]$j.beatFound -eq 1) { 'Green' } else { 'Yellow' })
  Write-Host ('  long      = ' + $j.longBeat + '   (1 = the last beat was a 1 or a 3 - what LED 10 shows)') -ForegroundColor $(if ([int]$j.longBeat -eq 1) { 'Green' } else { 'DarkGray' })
  Write-Host ('  fitN      = ' + $j.fitN + '   P = ' + $P + ' ms' + $(if ($P -ge 444 -and $P -le 632) { '   (legal)' } else { '   *** NOT 444-632 ***' })) -ForegroundColor $(if ($P -ge 444 -and $P -le 632) { 'Gray' } else { 'Red' })
  Write-Host ('  X         = ' + $j.X + ' ms   (must fall toward zero)')
  Write-Host ('  BeatLen   = ' + $j.len0 + ' / ' + $j.len1 + ' / ' + $j.len2 + ' / ' + $j.len3 + '   (the four; 1 and 3 should be the longer pair)')
  Write-Host ('  transients= ' + $j.lvlRej + '   leadFlashes = ' + $j.leadFlash + '   io48 = ' + $j.io48)
  Write-Host ''
  Write-Host '  ---- THE LINE AND THE ENVELOPE ----' -ForegroundColor White
  Write-Host ('  thr       = ' + [math]::Round($j.thr,2) + '   lvlLo = ' + [math]::Round($j.lvlLo,2) + '   lvlHi = ' + [math]::Round($j.lvlHi,2) + '   (MIN_RISE is 12, so thr = lvlLo + 12 at the floor)')
  Write-Host ('  band      = ' + $j.odfLo + '-' + $j.odfHi + ' Hz    sens = ' + $j.p0sens + '    freq = ' + $j.v0fq)
  Write-Host ''
  Write-Host ('  clock p0iv= ' + $j.p0iv + '   p0clk = ' + $j.p0clk + '   heap = ' + $j.freeHeap) -ForegroundColor DarkGray
  exit 0
}

Write-Host ''
Write-Output $body
