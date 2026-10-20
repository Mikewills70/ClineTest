# ---------------------------------------------------------------------------
# FLASH the sketch: same toolchain as wls_build.ps1, with the port.
#
#   powershell -ExecutionPolicy Bypass -File wls_upload.ps1
#
# Build first - this only uploads what is already compiled. The port is COM5 on
# this machine (check with: arduino-cli board list). Closing the Arduino IDE's
# serial monitor is not required, but anything else holding COM5 will fail the
# upload with "access denied".
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
$here   = $PSScriptRoot
$cli    = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$sketch = $here
$fqbn   = 'esp32:esp32:esp32s3'
$port   = 'COM5'
$log    = Join-Path $here 'wls_upload.log'

"=== upload started $(Get-Date -Format 'HH:mm:ss') port=$port ===" | Out-File $log -Encoding utf8
& $cli upload -p $port --fqbn $fqbn $sketch *>&1 | Out-File $log -Append -Encoding utf8
"=== upload finished $(Get-Date -Format 'HH:mm:ss') ===" | Out-File $log -Append -Encoding utf8
