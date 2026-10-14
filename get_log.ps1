# ---------------------------------------------------------------------------
# Fetches the ESP32's diagnostic log and saves it NEXT TO THIS SKETCH as
# board_log.txt, so its contents can be read/copied without retyping them or
# e-mailing anything.
#
#   1) Connect this PC's Wi-Fi to the board's own network:
#          SSID: ESP32-LightShow      password: 12345678
#      (the PC will warn that there is no internet - that is expected, the
#       board is a stand-alone access point)
#   2) Run:   powershell -ExecutionPolicy Bypass -File get_log.ps1
#   3) The log is printed and saved to  board_log.txt  in this folder.
# ---------------------------------------------------------------------------
$out = Join-Path $PSScriptRoot 'board_log.txt'
try {
    Invoke-WebRequest -Uri 'http://192.168.4.1/log.txt' -OutFile $out -UseBasicParsing -TimeoutSec 25
    Write-Host "OK - saved to $out"
    Write-Host '---------------- log ----------------'
    Get-Content $out
} catch {
    Write-Host "FAILED: $($_.Exception.Message)"
    Write-Host "Is this PC connected to the 'ESP32-LightShow' Wi-Fi network (password 12345678)?"
}
