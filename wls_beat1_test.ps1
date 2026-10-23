# ===================================================================================================
# BEAT-1 ALGORITHM TEST HARNESS - DEVELOPMENT TOOL, NOT PART OF THE FIRMWARE
# ===================================================================================================
# 1. loads the board's beat log (time, bar position, snare reading, mid reading)
# 2. loads the user's taps - ground truth for which beat is beat 1
# 3. ALIGNS the two OFFLINE. The taps are on the laptop clock, the beats on the board's, and the board's
#    clock runs ~1.2% fast, so the fit needs an offset AND a rate. That alignment is part of the TEST, not
#    the algorithm: the algorithm never sees the taps.
# 4. LABELS each beat from the taps: the tapped ones are beat 1, the three between are 2,3,4
# 5. scores a candidate algorithm's beat-1 choice against those labels
# The point is to iterate offline, quickly, before flashing anything.
# ===================================================================================================
param(
  [string]$BeatsCsv = 'beat1_beats_TEST.csv',
  [string]$TapsCsv  = 'beat1_taps_LABEL.csv'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

# ---- the beat log ---------------------------------------------------------------------------------
$bt = @()
foreach ($ln in (Get-Content $BeatsCsv)) {
  if ($ln -notmatch '^[0-9]') { continue }
  $f = $ln -split ','
  $bt += [pscustomobject]@{ ms = [int]$f[0]; bar = [int]$f[1]; sn = [double]$f[2]; md = [double]$f[3] }
}
Write-Host ("beats loaded: {0}   span {1}..{2} ms" -f $bt.Count, $bt[0].ms, $bt[-1].ms)

# ---- the taps -------------------------------------------------------------------------------------
# ---- place the taps on the board's clock, then refine -----------------------------------------------
# The beat log contains IDLE beats too (the slow ticks when the music stopped), so mapping "tap k to beat
# 4k" by INDEX is invalid - the number of beats between two presses is not always four. The alignment has
# to work by TIME.
# The coarse offset comes from the files: the beats file was saved at a known wall time, and its last row
# carries the board's millis at that moment, so the board's boot wall time follows to within a second.
# The taps carry both their tick and their wall time, so each one can be placed on the board's clock.
$beatsMtime = (Get-Item $BeatsCsv).LastWriteTime
$bootWall   = $beatsMtime.AddSeconds(-($bt[-1].ms / 1000.0))
$tapRows = @(Get-Content $TapsCsv | Select-Object -Skip 1 | ForEach-Object {
  $f = $_ -split ','
  [pscustomobject]@{ n = [int]$f[0]; tick = [int]$f[1]; wall = [DateTime]::Parse($f[2]) }
})
$rs = New-Object System.Collections.ArrayList
$cs = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt $tapRows.Count; $i++) {
  if ($i -gt 0 -and ($tapRows[$i].tick - $tapRows[$i-1].tick) -gt 6000) {
    [void]$rs.Add($cs); $cs = New-Object System.Collections.ArrayList
  }
  [void]$cs.Add($tapRows[$i])
}
if ($cs.Count -gt 0) { [void]$rs.Add($cs) }
$lastRun = @($rs[$rs.Count - 1])
$sizes = ($rs | ForEach-Object { $_.Count }) -join '/'
Write-Host ("taps: {0} in {1} run(s) [{2}]  - using the last ({3})" -f $tapRows.Count, $rs.Count, $sizes, $lastRun.Count)
$tapMs = @($lastRun | ForEach-Object { [int](($_.wall - $bootWall).TotalMilliseconds) })

# A PLAIN INT ARRAY OF THE BEAT TIMES. Indexing $bt[$i].ms in the scan below meant ~2.5 million property
# accesses, which took longer than the command timeout. This is the same data with no wrapper.
$btMs = New-Object int[] $bt.Count
for ($i = 0; $i -lt $bt.Count; $i++) { $btMs[$i] = $bt[$i].ms }

# THE PHASE SCAN, BY TIME: coarse pass then fine. Shift every tap by s ms and count how many land within
# 150 ms of a beat. The range covers a whole second - the precision of the boot-wall estimate - plus a bar
# and a half, so the correct phase cannot fall outside it.
function Hits-At([int]$s) {
  $h = 0
  foreach ($m0 in $tapMs) {
    $m = $m0 + $s
    $lo = 0; $hi = $btMs.Length - 1
    while ($lo -lt $hi) { $mid = [int](($lo + $hi) / 2); if ($btMs[$mid] -lt $m) { $lo = $mid + 1 } else { $hi = $mid } }
    $d = [Math]::Abs($btMs[$lo] - $m)
    if ($lo -gt 0) { $d2 = [Math]::Abs($btMs[$lo-1] - $m); if ($d2 -lt $d) { $d = $d2 } }
    if ($d -le 150) { $h++ }
  }
  return $h
}
$bestShift = 0; $bestHits = -1
for ($s = -3000; $s -le 3000; $s += 40) { $h = Hits-At $s; if ($h -gt $bestHits) { $bestHits = $h; $bestShift = $s } }
for ($s = $bestShift - 40; $s -le $bestShift + 40; $s += 4) { $h = Hits-At $s; if ($h -gt $bestHits) { $bestHits = $h; $bestShift = $s } }
Write-Host ("PHASE SCAN: best shift {0} ms puts {1} of {2} taps on a beat ({3:N1} per cent)" -f $bestShift, $bestHits, $lastRun.Count, (100.0*$bestHits/$lastRun.Count))
if ($bestHits -lt $lastRun.Count * 0.5) {
  Write-Host 'THE PHASE SCAN DID NOT FIND THE RUN - the boot-wall estimate must be wrong.' -ForegroundColor Red
}
$taps = @($tapMs | ForEach-Object { $_ + $bestShift })

# ---- the alignment: offset + rate, fitted ----------------------------------------------------------
# The taps are ~4 beats apart, so tap k should sit at beat index (b0 + 4k). For each b0 the residuals are
# fitted with a straight line (a clock offset plus a rate difference); the smallest residual wins.
$best = $null
for ($b0 = 0; $b0 -lt [Math]::Min(60, $bt.Count - 4 * $taps.Count); $b0++) {
  $xs = New-Object System.Collections.ArrayList
  $ys = New-Object System.Collections.ArrayList
  $ok = $true
  for ($k = 0; $k -lt $taps.Count; $k++) {
    $bi = $b0 + 4 * $k
    if ($bi -ge $bt.Count) { $ok = $false; break }
    [void]$xs.Add([double]$taps[$k])
    [void]$ys.Add([double]$bt[$bi].ms)
  }
  if (-not $ok -or $xs.Count -lt 20) { continue }
  $n = $xs.Count; $sx = 0.0; $sy = 0.0; $sxx = 0.0; $sxy = 0.0
  for ($i = 0; $i -lt $n; $i++) { $sx += $xs[$i]; $sy += $ys[$i]; $sxx += $xs[$i]*$xs[$i]; $sxy += $xs[$i]*$ys[$i] }
  $den = $n*$sxx - $sx*$sx
  if ($den -eq 0) { continue }
  $b = ($n*$sxy - $sx*$sy) / $den
  $a = ($sy - $b*$sx) / $n
  $ss = 0.0
  for ($i = 0; $i -lt $n; $i++) { $r = $ys[$i] - ($a + $b*$xs[$i]); $ss += $r*$r }
  $rms = [Math]::Sqrt($ss / $n)
  if ($null -eq $best -or $rms -lt $best.rms) { $best = [pscustomobject]@{ b0 = $b0; a = $a; b = $b; rms = $rms } }
}
if ($null -eq $best) { throw 'the alignment failed: no candidate produced enough pairs' }
Write-Host ("ALIGNED: first tapped beat index {0}   rms residual {1:N0} ms   rate {2:N6}" -f $best.b0, $best.rms, $best.b)

# ---- label the beats from the taps -----------------------------------------------------------------
foreach ($x in $bt) { $x | Add-Member -NotePropertyName label -NotePropertyValue 0 -Force }
$labelled = 0
for ($k = 0; $k -lt $taps.Count; $k++) {
  $bi = $best.b0 + 4 * $k
  if ($bi -lt 0 -or $bi -ge $bt.Count) { continue }
  for ($j = 0; $j -lt 4; $j++) {
    $ix = $bi + $j
    if ($ix -ge 0 -and $ix -lt $bt.Count) { if ($bt[$ix].label -eq 0) { $bt[$ix].label = $j + 1; $labelled++ } }
  }
}
Write-Host ("labels applied: {0} beats" -f $labelled)

# ---- how the CURRENT model does --------------------------------------------------------------------
$cur = 0; $tot = 0
for ($i = 0; $i -lt $bt.Count; $i++) {
  if ($bt[$i].label -eq 0) { continue }
  $tot++
  if ( ($bt[$i].bar -eq 0) -eq ($bt[$i].label -eq 1) ) { $cur++ }
}
if ($tot -gt 0) {
  Write-Host ''
  Write-Host ("CURRENT MODEL: {0} of {1} correct  =  {2:N1} per cent" -f $cur, $tot, (100.0*$cur/$tot))
}

$out = Join-Path $here 'beat1_labelled_TEST.csv'
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('ms,bar,sn,md,label')
foreach ($x in $bt) { [void]$sb.AppendLine(('{0},{1},{2},{3},{4}' -f $x.ms, $x.bar, $x.sn, $x.md, $x.label)) }
[System.IO.File]::WriteAllText($out, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("wrote {0} - the algorithm script scores against this" -f $out)