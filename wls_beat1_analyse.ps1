# ===================================================================================================
# BEAT-1 ANALYSIS - DEVELOPMENT TOOL
# ===================================================================================================
# Uses the tap PAIRS for the clock offset (measured, consistent to ~1 s), refines it over a small range,
# labels the beats from the user's taps, then scores a candidate beat-1 algorithm against those labels.
# The algorithm never sees the taps - they are the ground truth it is measured against.
# ===================================================================================================
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

$btMs = New-Object System.Collections.ArrayList
$btBar = New-Object System.Collections.ArrayList
$btSn = New-Object System.Collections.ArrayList
$btMd = New-Object System.Collections.ArrayList
foreach ($ln in (Get-Content 'beat1_beats_TEST.csv')) {
  if ($ln -notmatch '^[0-9]') { continue }
  $f = $ln -split ','
  [void]$btMs.Add([int]$f[0]); [void]$btBar.Add([int]$f[1])
  [void]$btSn.Add([double]$f[2]); [void]$btMd.Add([double]$f[3])
}
$n = $btMs.Count
Write-Host ("beats: {0}   {1}..{2} ms" -f $n, $btMs[0], $btMs[$n-1])

# the clock offset from the pairs, as the median of (board - laptop)
$offs = New-Object System.Collections.ArrayList
foreach ($ln in (Get-Content 'beat1_pairs_TEST.csv')) {
  if ($ln -notmatch '^[0-9]') { continue }
  $f = $ln -split ','
  [void]$offs.Add([int]$f[1] - [int]$f[0])
}
$offsSort = $offs | Sort-Object
$K = $offsSort[[int]($offsSort.Count / 2)]
Write-Host ("clock offset from {0} pairs: {1}  (spread {2} ms)" -f $offs.Count, $K, ($offsSort[-1] - $offsSort[0]))

# the taps: split into runs at the long gaps, take the last
$rows = New-Object System.Collections.ArrayList
foreach ($ln in (Get-Content 'beat1_taps_LABEL.csv' | Select-Object -Skip 1)) {
  $f = $ln -split ','
  [void]$rows.Add([pscustomobject]@{ tick = [int]$f[1]; wall = [DateTime]::Parse($f[2]) })
}
$runs = New-Object System.Collections.ArrayList
$cur = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt $rows.Count; $i++) {
  if ($i -gt 0 -and ($rows[$i].tick - $rows[$i-1].tick) -gt 6000) { [void]$runs.Add($cur); $cur = New-Object System.Collections.ArrayList }
  [void]$cur.Add($rows[$i])
}
if ($cur.Count -gt 0) { [void]$runs.Add($cur) }
$run = @($runs[$runs.Count - 1])
$tapMs = New-Object int[] $run.Count
for ($i = 0; $i -lt $run.Count; $i++) { $tapMs[$i] = $run[$i].tick + $K }
Write-Host ("taps: {0} in {1} runs, using the last ({2})   board ms {3}..{4}" -f $rows.Count, $runs.Count, $run.Count, $tapMs[0], $tapMs[$tapMs.Length-1])

# how many taps land within 140 ms of a beat, for a given shift
function Hits-At([int]$s) {
  $h = 0
  foreach ($m0 in $tapMs) {
    $m = $m0 + $s
    $j = 0
    while ($j -lt $n - 1 -and $btMs[$j + 1] -le $m) { $j++ }
    $d = [Math]::Abs($btMs[$j] - $m)
    if ($j -gt 0) { $d2 = [Math]::Abs($btMs[$j - 1] - $m); if ($d2 -lt $d) { $d = $d2 } }
    if ($d -le 140) { $h++ }
  }
  return $h
}

# THE REFINE, SEGMENTED (22 Oct). A single shift CANNOT align this run: the board's clock runs ~1.2 per
# cent fast, so the mismatch grows - measured, it reaches about 2.3 s across the run. That is the drift the
# user has been hearing. So the run is split into segments, the best shift is found for each, and a line
# through those shifts gives BOTH the offset and the drift.
$seg = 6
$per = [int][Math]::Ceiling($run.Count / $seg)
$segShift = New-Object int[] $seg
$segMid = New-Object double[] $seg
$segHit = New-Object int[] $seg
for ($g = 0; $g -lt $seg; $g++) {
  $lo = $g * $per
  $hi = [Math]::Min($run.Count - 1, $lo + $per - 1)
  if ($hi -lt $lo) { continue }
  $bs = 0; $bh = -1
  for ($s = -3000; $s -le 3000; $s += 20) {
    $h = 0
    for ($i = $lo; $i -le $hi; $i++) {
      $m = $tapMs[$i] + $s
      $j = 0
      while ($j -lt $n - 1 -and $btMs[$j + 1] -le $m) { $j++ }
      $d = [Math]::Abs($btMs[$j] - $m)
      if ($j -gt 0) { $d2 = [Math]::Abs($btMs[$j - 1] - $m); if ($d2 -lt $d) { $d = $d2 } }
      if ($d -le 140) { $h++ }
    }
    if ($h -gt $bh) { $bh = $h; $bs = $s }
  }
  $segShift[$g] = $bs; $segMid[$g] = $tapMs[[int](($lo + $hi) / 2)]; $segHit[$g] = $bh
  Write-Host ("  segment {0}: taps {1}-{2}  shift {3,6} ms  hits {4}/{5}" -f $g, $lo, $hi, $bs, $bh, ($hi - $lo + 1))
}
# a line through the segment shifts: intercept = the offset, slope = the drift in ms per ms
$ns = 0; $sx = 0.0; $sy = 0.0; $sxx = 0.0; $sxy = 0.0
for ($g = 0; $g -lt $seg; $g++) {
  if ($segHit[$g] -le 0) { continue }
  $ns++; $x = $segMid[$g]; $y = [double]$segShift[$g]
  $sx += $x; $sy += $y; $sxx += $x*$x; $sxy += $x*$y
}
$den = $ns*$sxx - $sx*$sx
$drift = 0.0; $inter = 0.0
if ($den -ne 0) { $drift = ($ns*$sxy - $sx*$sy) / $den; $inter = ($sy - $drift*$sx) / $ns }
$midT = ($btMs[0] + $btMs[$n-1]) / 2.0
$bestS = [int]($inter + $drift * $midT)
Write-Host ("DRIFT FITTED: {0:N1} ms per second  (a beat-period error of {1:N2} per cent)" -f ($drift*1000.0), (100.0*$drift*0.45))
$bestH = Hits-At $bestS
Write-Host ("with a single shift {0} ms: {1} of {2} taps on a beat ({3:N1} per cent)" -f $bestS, $bestH, $run.Count, (100.0*$bestH/$run.Count))
$tapOn = New-Object int[] $run.Count
for ($i = 0; $i -lt $run.Count; $i++) { $tapOn[$i] = [int]($tapMs[$i] + $inter + $drift * $tapMs[$i]) }

# label: the beat nearest each tap is beat 1, and the three after it are 2,3,4
$lab = New-Object int[] $n
$labelled = 0
foreach ($m in $tapOn) {
  $j = 0
  while ($j -lt $n - 1 -and $btMs[$j + 1] -le $m) { $j++ }
  if ([Math]::Abs($btMs[$j] - $m) -gt 200) { continue }
  for ($k = 0; $k -lt 4; $k++) { if ($j + $k -lt $n) { $lab[$j + $k] = $k + 1; $labelled++ } }
}
Write-Host ("labelled {0} beats from the taps" -f $labelled)

# how the CURRENT model does: it says beat 1 when the bar column is 0
$cur = 0; $tot = 0
for ($i = 0; $i -lt $n; $i++) {
  if ($lab[$i] -eq 0) { continue }
  $tot++
  if ( ($btBar[$i] -eq 0) -eq ($lab[$i] -eq 1) ) { $cur++ }
}
if ($tot -gt 0) { Write-Host ("CURRENT MODEL: {0} of {1} = {2:N1} per cent" -f $cur, $tot, (100.0*$cur/$tot)) }

# save the labelled set for the algorithm script
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('ms,bar,sn,md,label')
for ($i = 0; $i -lt $n; $i++) { [void]$sb.AppendLine(('{0},{1},{2},{3},{4}' -f $btMs[$i], $btBar[$i], $btSn[$i], $btMd[$i], $lab[$i])) }
[System.IO.File]::WriteAllText((Join-Path $here 'beat1_labelled_TEST.csv'), $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host 'wrote beat1_labelled_TEST.csv'