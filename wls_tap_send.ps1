# *** TEMPORARY BEAT-1 TAP SENDER - REMOVE WITH wls_tap.ps1 WHEN TESTING IS OVER ***
# WHY THIS IS A SEPARATE FILE (22 Oct): the board's reply takes 70-1300 ms. When wls_tap.ps1 sent the
# request itself, its poll loop was BLOCKED for that whole time, so any press made during a send was
# missed - about 50 presses produced only 4 lines. The main loop now logs the press instantly and starts
# THIS script as a separate hidden process, which returns at once and never blocks the polling.
# The press time is passed in ?t= so the board uses the moment of the CLICK, not the moment of arrival -
# so how long this takes does not matter.
param(
  [Parameter(Mandatory=$true)][int]$T,
  [string]$Board = '192.168.4.1'
)
$ErrorActionPreference = 'SilentlyContinue'
try {
  $c = New-Object System.Net.Sockets.TcpClient
  $c.Connect($Board, 80)
  $s = $c.GetStream()
  $req = [System.Text.Encoding]::ASCII.GetBytes("GET /tap?t=$T HTTP/1.0`r`nHost: $Board`r`nConnection: close`r`n`r`n")
  $s.Write($req, 0, $req.Length)
  $buf = New-Object byte[] 256
  $null = $s.Read($buf, 0, 256)
  $c.Close()
} catch { }
