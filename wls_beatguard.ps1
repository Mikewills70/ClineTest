# ===================================================================================================
# BASS BEAT GUARD - protects the audio signal that flashes the light in time with the music.
# ===================================================================================================
# WHY THIS EXISTS. Rule 10 of Rules.docx, word for word:
#
#   "UNDER NO CIRCUMSTANCES ARE YOU ALLOWED TO ALTER, CHANGE, DELETE, ADD OR DO ANYTHING TO THE AUDIO
#    SETTINGS. THIS WAS A HARD WON VICTORY OF CODING AND MUST NOT BE TOUCHED IN ANY WAY."
#
# WHAT IT DOES. The region of WIFI_LightShow.ino between the two BASS BEAT sentinels IS the beat - the
# onset detector and the fitted clock. This script hashes that region and compares it with the hash
# recorded when it was frozen. If anything differs it says so loudly, prints the exact lines that
# changed, quotes Rule 10, and exits with code 1.
#
#   wls_beatguard.ps1                      verify (run this when suspicious)
#   wls_beatguard.ps1 -Freeze -Reason ".." record the current region as the frozen state, with a reason
#
# WIRED INTO THE BUILD. wls_build.ps1 runs this FIRST and REFUSES TO BUILD if it fails, unless it is
# called with -AllowBeatChange. So a modified beat cannot be flashed by accident.
#
# WHAT IT CANNOT DO - so nobody trusts it further than it goes. This script lives beside the code it
# protects, so an agent that edits the sketch can also edit this script or the recorded hash. It is a
# TRIPWIRE, not a lock. What cannot be quietly undone: bassbeat_history.txt is APPEND-ONLY and in git,
# so a deleted entry shows in `git diff`; the region text is stored in bassbeat_region.txt, so a change
# is a visible diff; and the user can verify by running this script or diffing the baseline folder.
# ===================================================================================================
param(
  [switch]$Freeze,
  [string]$Reason = '',
  [string]$Ino    = 'WIFI_LightShow.ino'
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
Set-Location $here

$START = '// ==== BASS BEAT PROTECTED REGION - START - DO NOT EDIT BELOW THIS LINE (RULE 10) ===='
$END   = '// ==== BASS BEAT PROTECTED REGION - END ===='

$baseDir    = Join-Path $here 'baseline'
$regionFile = Join-Path $baseDir 'bassbeat_region.txt'
$hashFile   = Join-Path $baseDir 'bassbeat_hash.txt'
$histFile   = Join-Path $baseDir 'bassbeat_history.txt'

if (-not (Test-Path $baseDir)) { New-Item -ItemType Directory -Path $baseDir | Out-Null }

# ---- read the sketch and cut out the protected region ---------------------------------------------
# DIRECT TEXT INDEXING, not a line array: the sentinels are exact strings, so finding them in the raw
# text cannot be thrown off by line endings, blank lines or trailing spaces.
$path = Join-Path $here $Ino
if (-not (Test-Path $path)) { Write-Host ('GUARD ERROR: cannot find ' + $path) -ForegroundColor Red; exit 2 }
$raw  = [System.IO.File]::ReadAllText($path)
$nrm  = $raw.Replace("`r`n", "`n").Replace("`r", "`n")   # hash on LF endings only

$sIdx = $nrm.IndexOf($START)
$eIdx = $nrm.IndexOf($END)
if ($sIdx -lt 0) { Write-Host 'GUARD ERROR: the START sentinel is MISSING from the sketch.' -ForegroundColor Red; exit 2 }
if ($eIdx -lt 0) { Write-Host 'GUARD ERROR: the END sentinel is MISSING from the sketch.'   -ForegroundColor Red; exit 2 }
if ($eIdx -le $sIdx) { Write-Host 'GUARD ERROR: the sentinels are in the wrong order.'      -ForegroundColor Red; exit 2 }

$regionText  = $nrm.Substring($sIdx, ($eIdx - $sIdx) + $END.Length)
$regionLines = $regionText -split "`n"
$startLine   = ($nrm.Substring(0, $sIdx) -split "`n").Count

$sha  = [System.Security.Cryptography.SHA256]::Create()
$hash = ([System.BitConverter]::ToString($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($regionText)))).Replace('-','')

Write-Host ''
Write-Host ('BASS BEAT region: line {0} of {1}, {2} lines, {3} bytes' -f $startLine, $Ino, $regionLines.Count, $regionText.Length) -ForegroundColor Gray
Write-Host ('  hash ' + $hash) -ForegroundColor Gray
Write-Host ''

# ---- FREEZE: record the current region as the frozen state ----------------------------------------
if ($Freeze) {
  if ([string]::IsNullOrWhiteSpace($Reason)) { throw 'A -Reason is required when freezing. Say WHY the beat region is changing.' }
  [System.IO.File]::WriteAllText($regionFile, $regionText, (New-Object System.Text.UTF8Encoding($false)))
  [System.IO.File]::WriteAllText($hashFile,   $hash,       (New-Object System.Text.UTF8Encoding($false)))
  $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
  $entry = '{0}  {1}  {2}' -f $stamp, $hash, $Reason
  if (-not (Test-Path $histFile)) {
    [System.IO.File]::WriteAllText($histFile, "# BASS BEAT PROTECTED REGION - append only. One line per authorised change.`n# date                hash                                                              reason`n", (New-Object System.Text.UTF8Encoding($false)))
  }
  [System.IO.File]::AppendAllText($histFile, ($entry + "`n"), (New-Object System.Text.UTF8Encoding($false)))
  Write-Host 'FROZEN.' -ForegroundColor Green
  Write-Host ('  region  -> ' + $regionFile) -ForegroundColor Gray
  Write-Host ('  history += ' + $entry)     -ForegroundColor Gray
  Write-Host ''
  exit 0
}

# ---- VERIFY ---------------------------------------------------------------------------------------
if (-not (Test-Path $hashFile)) {
  Write-Host 'NO FROZEN HASH YET. Run:  wls_beatguard.ps1 -Freeze -Reason "initial freeze"' -ForegroundColor Yellow
  exit 2
}
$expected = ([System.IO.File]::ReadAllText($hashFile)).Trim()

if ($hash -eq $expected) {
  Write-Host 'BASS BEAT REGION INTACT - unchanged since it was frozen.' -ForegroundColor Green
  if (Test-Path $histFile) {
    Write-Host '  authorised changes on record:' -ForegroundColor Gray
    Get-Content $histFile | Where-Object { $_ -notmatch '^\s*#' -and $_.Trim() -ne '' } |
      ForEach-Object { '    ' + $_ } | Select-Object -Last 5
  }
  Write-Host ''
  exit 0
}

# ---- CHANGED: say so as loudly as possible --------------------------------------------------------
Write-Host '###########################################################################################' -ForegroundColor Red
Write-Host '#                                                                                         #' -ForegroundColor Red
Write-Host '#   BASS BEAT REGION HAS BEEN CHANGED.                                                    #' -ForegroundColor Red
Write-Host '#                                                                                         #' -ForegroundColor Red
Write-Host '###########################################################################################' -ForegroundColor Red
Write-Host ''
Write-Host 'RULE 10 of the project rules (Rules.docx) says, word for word:' -ForegroundColor Yellow
Write-Host ''
Write-Host '  "UNDER NO CIRCUMSTANCES ARE YOU ALLOWED TO ALTER, CHANGE, DELETE, ADD OR DO ANYTHING' -ForegroundColor White
Write-Host '   TO THE AUDIO SETTINGS. THIS WAS A HARD WON VICTORY OF CODING AND MUST NOT BE TOUCHED' -ForegroundColor White
Write-Host '   IN ANY WAY."' -ForegroundColor White
Write-Host ''
Write-Host 'Someone - the user, or an AI assistant - has edited the code between the BASS BEAT' -ForegroundColor Yellow
Write-Host 'sentinels. That region is the beat itself, and it was declared FINISHED.' -ForegroundColor Yellow
Write-Host ''
Write-Host ('  expected  ' + $expected) -ForegroundColor Gray
Write-Host ('  found     ' + $hash)     -ForegroundColor Gray
Write-Host ''

if (Test-Path $regionFile) {
  $was   = ([System.IO.File]::ReadAllText($regionFile)).Replace("`r`n","`n") -split "`n"
  $max   = [Math]::Max($was.Count, $regionLines.Count)
  $shown = 0
  Write-Host 'WHAT CHANGED (frozen -> now):' -ForegroundColor Yellow
  for ($i = 0; $i -lt $max; $i++) {
    $a = if ($i -lt $was.Count)         { $was[$i] }         else { '<no line>' }
    $b = if ($i -lt $regionLines.Count) { $regionLines[$i] } else { '<no line>' }
    if ($a -cne $b) {
      $shown++
      if ($shown -le 40) {
        Write-Host ('  line ' + ($i + 1) + ' of the region') -ForegroundColor Gray
        Write-Host ('    was: ' + $a.TrimEnd()) -ForegroundColor DarkGray
        Write-Host ('    now: ' + $b.TrimEnd()) -ForegroundColor White
      }
    }
  }
  if ($shown -eq 0) { Write-Host '  (no line-level difference)' -ForegroundColor Gray }
  if ($shown -gt 40) { Write-Host ('  ... and {0} more changed lines' -f ($shown - 40)) -ForegroundColor Gray }
} else {
  Write-Host 'No stored copy of the region to compare against.' -ForegroundColor Yellow
}

Write-Host ''
Write-Host 'IF THIS WAS NOT AUTHORISED, restore the beat from git:' -ForegroundColor Yellow
Write-Host '  git diff baseline/                        <- see every change to the record' -ForegroundColor Gray
Write-Host '  git checkout <tag> -- WIFI_LightShow.ino  <- put the beat back' -ForegroundColor Gray
Write-Host ''
Write-Host 'IF IT WAS AUTHORISED by the user, in writing, record it with:' -ForegroundColor Yellow
Write-Host '  wls_beatguard.ps1 -Freeze -Reason "what changed and who authorised it"' -ForegroundColor Gray
Write-Host ''
exit 1