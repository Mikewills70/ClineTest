# ---------------------------------------------------------------------------
# Reads the ESP32's log over the USB serial port into board_log.txt, so nobody
# has to copy and paste diagnostics out by hand.
#
#   powershell -ExecutionPolicy Bypass -File read_log_serial.ps1
#
# DTR and RTS are explicitly de-asserted BEFORE the port is opened, because on
# the ESP32-S3 those lines are wired to reset/boot; that is the whole reason the
# serial port is avoided during normal running.
# Output: board_log.txt next to this script.
# ---------------------------------------------------------------------------
$here = $PSScriptRoot
$out  = Join-Path $here 'board_log.txt'

$port = New-Object System.IO.Ports.SerialPort('COM5', 115200, 'None', 8, 'One')
try {
    $port.DtrEnable = $false
    $port.RtsEnable = $false
    $port.Open()
} catch {
    Write-Host "FAILED to open COM5: $($_.Exception.Message)"
    exit 1
}

Start-Sleep -Milliseconds 400
$port.Write('L')            # ask the firmware to dump /log.txt

# The firmware ALSO prints its 30 s heartbeat live to serial (logAppend does both),
# because the flash log is deliberately NOT written while a show is running - a
# LittleFS write is a flash erase that freezes both cores, which was the display
# stall. So capture long enough to guarantee a live heartbeat arrives, and do NOT
# stop at the dump's END marker or the live lines after it would be missed.
$sb = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddSeconds(45)
while ((Get-Date) -lt $deadline) {
    if ($port.BytesToRead -gt 0) {
        [void]$sb.Append($port.ReadExisting())
    }
    Start-Sleep -Milliseconds 150
}
$port.Close()

$text = $sb.ToString()
$text | Out-File $out -Encoding utf8
Write-Host "Saved $($text.Length) chars to $out"
Write-Host '---------------- log ----------------'
Write-Host $text
