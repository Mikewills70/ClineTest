# ---------------------------------------------------------------------------
# BUILD the sketch with the Arduino IDE's bundled arduino-cli.
#
#   powershell -ExecutionPolicy Bypass -File wls_build.ps1
#
# Created 19 Oct 2026, when the %TEMP% copies this session had been using were
# cleaned away by Windows. These live WITH THE PROJECT now so that cannot
# happen again. wls_upload.ps1 is the matching flash step. Both write a log
# next to themselves (and *.log is gitignored).
#
# The board is an ESP32-S3 (LCD_CAM parallel output, GPIO2 = microphone), hence
# the fqbn below. VERIFY A BUILD by its size: 1255691 bytes flash / 99132 bytes
# RAM was the last known-good figure, and a different fqbn or option set will
# not reproduce those numbers exactly.
# ---------------------------------------------------------------------------
param(
  # THE ONLY WAY PAST THE BASS BEAT GUARD. Deliberately not a default, so it has to be typed.
  [switch]$AllowBeatChange
)

$ErrorActionPreference = 'Continue'
$here   = $PSScriptRoot
$cli    = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$sketch = $here
$fqbn   = 'esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,FlashMode=qio,USBMode=hwcdc'
# BOARD OPTIONS FOR THIS HARDWARE: an ESP32-S3 **N16R8** - 16 MB flash, 8 MB OCTAL PSRAM.
#   FlashSize=16M          the board has 16 MB, not the 4 MB the default assumes
#   PartitionScheme=huge_app   "Huge APP (3MB No OTA/1MB SPIFFS)" - the app slot goes from 1.25 MB
#                          to 3 MB, which is what stops us running out: the sketch was at 96% of
#                          the default slot with 47 KB to spare. LittleFS lives in the 1 MB SPIFFS
#                          partition (far more than the 16 KB log needs) and OTA is unused.
#   PSRAM=opi              8 MB of octal PSRAM, as this module has
#   FlashMode=qio          QIO 80 MHz, the module's normal mode
#   USBMode=hwcdc          the USB-serial log path
# CHANGING THE PARTITION SCHEME REFORMATS THE FILE SYSTEM AND MOVES NVS: the saved settings and the
# timeline are wiped. /load-timeline, /load-playlist and /settings were backed up to board_*.json
# before this was applied.
# The sketchbook on THIS machine is D:\Dell 7720\Arduino, not the usual Documents\Arduino, so the
# IDE's bundled CLI does not find its libraries by itself ("ArduinoJson.h: No such file or
# directory"). --libraries below points it at them.
$libs   = 'D:\Dell 7720\Arduino\libraries'
$log    = Join-Path $here 'wls_build.log'

# ---- THE BASS BEAT GUARD (RULE 10) ----------------------------------------------------------------
# The region of the sketch between the BASS BEAT sentinels IS the beat - the onset detector and the
# fitted clock that make the light flash in time with the music. RULE 10 freezes it: "UNDER NO
# CIRCUMSTANCES ARE YOU ALLOWED TO ALTER, CHANGE, DELETE, ADD OR DO ANYTHING TO THE AUDIO SETTINGS."
# The guard runs FIRST and this build REFUSES TO CONTINUE if that region has changed, so a modified
# beat cannot be flashed by accident. -AllowBeatChange is the only way past and has to be typed.
$guard = Join-Path $here 'wls_beatguard.ps1'
if (Test-Path $guard) {
  & powershell -ExecutionPolicy Bypass -File $guard
  if ($LASTEXITCODE -ne 0) {
    if (-not $AllowBeatChange) {
      Write-Host ''
      Write-Host '###########################################################################' -ForegroundColor Red
      Write-Host '#  BUILD REFUSED - the BASS BEAT region has changed (details above).     #' -ForegroundColor Red
      Write-Host '#  Rule 10 forbids changing the audio settings. See Rules.docx.          #' -ForegroundColor Red
      Write-Host '###########################################################################' -ForegroundColor Red
      Write-Host 'If the USER has authorised this change in writing, record it and rebuild:' -ForegroundColor Yellow
      Write-Host '  wls_beatguard.ps1 -Freeze -Reason "what changed and who authorised it"' -ForegroundColor Gray
      Write-Host 'To build anyway WITHOUT recording it (not recommended):' -ForegroundColor Yellow
      Write-Host '  wls_build.ps1 -AllowBeatChange' -ForegroundColor Gray
      exit 1
    }
    Write-Host 'WARNING: building with a CHANGED BASS BEAT region (-AllowBeatChange was passed).' -ForegroundColor Yellow
  }
}


"=== build started $(Get-Date -Format 'HH:mm:ss') fqbn=$fqbn ===" | Out-File $log -Encoding utf8
& $cli compile --fqbn $fqbn --libraries $libs $sketch *>&1 | Out-File $log -Append -Encoding utf8
"=== build finished $(Get-Date -Format 'HH:mm:ss') ===" | Out-File $log -Append -Encoding utf8
