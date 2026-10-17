# ---------------------------------------------------------------------------
# READ THE BOARD'S LOG - the fast half of the working loop.
#
#   powershell -ExecutionPolicy Bypass -File read_board_log.ps1
#
# Assumes this PC is ALREADY on the board's Wi-Fi (ESP32-LightShow / 12345678).
# It fetches /log.txt and /diag next to this sketch as board_log.txt and
# board_diag.txt, then prints only the lines that matter: the requests, the
# display transitions, the tweaks, the boots, page errors, the slow renders and
# the heap floor. No serial port is opened (opening it resets the board).
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot

$ip = '192.168.4.1'

# Caching defeated on purpose: Windows will replay a cached /diag (it once showed a board
# state ten minutes old), and the firmware sends no cache headers on /diag.
function Get-NoCache([string]$url, [int]$timeout) {
    $r = [System.Net.HttpWebRequest]::Create($url)
    $r.CachePolicy = New-Object System.Net.Cache.RequestCachePolicy([System.Net.Cache.RequestCacheLevel]::NoCacheNoStore)
    $r.Timeout = $timeout
    $resp = $r.GetResponse()
    $sr = New-Object System.IO.StreamReader($resp.GetResponseStream())
    $body = $sr.ReadToEnd(); $sr.Close(); $resp.Close()
    return $body
}

$stamp = [DateTime]::Now.Ticks
foreach ($pair in @(@('/diag', 'board_diag.txt'), @('/log.txt', 'board_log.txt'), @('/tail', 'board_tail.txt'))) {
    try {
        $b = Get-NoCache ("http://" + $ip + $pair[0] + "?t=$stamp") 20000
        $b | Out-File (Join-Path $here $pair[1]) -Encoding utf8
        Write-Host "OK   $($pair[0]) -> $($pair[1])  ($($b.Length) chars)"
    } catch { Write-Host "FAIL $($pair[0]) : $($_.Exception.Message)" }
}

if (Test-Path (Join-Path $here 'board_diag.txt')) {
    Write-Host '---------------- diag ----------------'
    Get-Content (Join-Path $here 'board_diag.txt') -Raw
}

# /tail is the LIVE log: logService() never flushes to flash while a show is running, so
# /log.txt stops at the moment a show starts and the tail is the only record of what
# happened next. It is printed in full, because it is short.
if (Test-Path (Join-Path $here 'board_tail.txt')) {
    Write-Host '---------------- live tail (RAM, /tail) ----------------'
    Get-Content (Join-Path $here 'board_tail.txt')
}


if ($gotLog) {
    $f = Join-Path $here 'board_log.txt'
    $rx = 'req |display (ON|OFF)|blank |tweak |BOOT #|HEAP FLOOR|\[slow\]|PAGE ERROR|motion play|beats onsets|cells row|listen chains'
    Write-Host '---------------- log ----------------'
    Select-String -Path $f -Pattern $rx | ForEach-Object { $_.Line } | Select-Object -Last 120
}
Write-Host 'DONE'
