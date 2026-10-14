# ---------------------------------------------------------------------------
# Fetches the ESP32's log and diagnostics DIRECTLY, so nobody has to copy and
# paste them: connects this PC to the board's own Wi-Fi, downloads /log.txt and
# /diag, then reconnects to whichever network the PC was on before.
#
#   powershell -ExecutionPolicy Bypass -File fetch_log.ps1
#
# Output: board_log.txt and board_diag.txt next to this script.
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Continue'
$here   = $PSScriptRoot
$ssid   = 'ESP32-LightShow'
$pass   = '12345678'

function Get-CurSSID {
    $o = netsh wlan show interfaces | Out-String
    if ($o -match '(?m)^\s*SSID\s+:\s*(.+)$') { return $Matches[1].Trim() }
    return ''
}

$orig = Get-CurSSID
Write-Host "Current Wi-Fi: '$orig'"

$xml = @"
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>$ssid</name>
  <SSIDConfig><SSID><name>$ssid</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM><security>
    <authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>
    <sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>$pass</keyMaterial></sharedKey>
  </security></MSM>
</WLANProfile>
"@
$xml | Out-File "$env:TEMP\esp32_profile.xml" -Encoding ascii
netsh wlan add profile filename="$env:TEMP\esp32_profile.xml" user=current | Out-Null
netsh wlan connect name="$ssid" ssid="$ssid" | Out-Null

$joined = $false
for ($i = 0; $i -lt 15; $i++) {
    Start-Sleep -Seconds 2
    if ((Get-CurSSID) -eq $ssid) { $joined = $true; break }
}
Write-Host "Joined '$ssid': $joined"

if ($joined) {
    Start-Sleep -Seconds 2
    foreach ($pair in @(@('/log.txt','board_log.txt'), @('/diag','board_diag.txt'))) {
        for ($try = 0; $try -lt 5; $try++) {
            try {
                Invoke-WebRequest -Uri ("http://192.168.4.1" + $pair[0]) `
                                  -OutFile (Join-Path $here $pair[1]) `
                                  -UseBasicParsing -TimeoutSec 15
                Write-Host "OK  $($pair[0]) -> $($pair[1])"
                break
            } catch {
                Write-Host "retry $($pair[0]): $($_.Exception.Message)"
                Start-Sleep -Seconds 2
            }
        }
    }
}

if ($orig -and $orig -ne $ssid) {
    netsh wlan connect name="$orig" ssid="$orig" | Out-Null
    Write-Host "Reconnecting to '$orig'"
}
Write-Host 'DONE'
