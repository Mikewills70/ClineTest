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
$ErrorActionPreference = 'Continue'
$here   = $PSScriptRoot
$cli    = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$sketch = $here
$fqbn   = 'esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,FlashMode=qio,USBMode=default'
# BOARD OPTIONS FOR THIS HARDWARE: an ESP32-S3 **N16R8** - 16 MB flash, 8 MB OCTAL PSRAM.
#   FlashSize=16M          the board has 16 MB, not the 4 MB the default assumes
#   PartitionScheme=huge_app   "Huge APP (3MB No OTA/1MB SPIFFS)" - the app slot goes from 1.25 MB
#                          to 3 MB, which is what stops us running out: the sketch was at 96% of
#                          the default slot with 47 KB to spare. LittleFS lives in the 1 MB SPIFFS
#                          partition (far more than the 16 KB log needs) and OTA is unused.
#   PSRAM=opi              8 MB of octal PSRAM, as this module has
#   FlashMode=qio          QIO 80 MHz, the module's normal mode
#   USBMode=default          the USB-serial log path
# CHANGING THE PARTITION SCHEME REFORMATS THE FILE SYSTEM AND MOVES NVS: the saved settings and the
# timeline are wiped. /load-timeline, /load-playlist and /settings were backed up to board_*.json
# before this was applied.
# The sketchbook on THIS machine is D:\Dell 7720\Arduino, not the usual Documents\Arduino, so the
# IDE's bundled CLI does not find its libraries by itself ("ArduinoJson.h: No such file or
# directory"). --libraries below points it at them.
$libs   = 'D:\Dell 7720\Arduino\libraries'
$log    = Join-Path $here 'wls_build.log'

"=== build started $(Get-Date -Format 'HH:mm:ss') fqbn=$fqbn ===" | Out-File $log -Encoding utf8
& $cli compile --fqbn $fqbn --libraries $libs $sketch *>&1 | Out-File $log -Append -Encoding utf8
"=== build finished $(Get-Date -Format 'HH:mm:ss') ===" | Out-File $log -Append -Encoding utf8
