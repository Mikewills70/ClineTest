# ===================================================================================================
# BEAT-1 ANALYSIS v2 - DEVELOPMENT TOOL
# ===================================================================================================
# THE USER'S METHOD: "the nearest 1 of the 4 beats is the one that I was highlighting as beat 1... mark
# that as beat one, and then apply your timing calculations."
# That is right. The complication is the board's clock runs ~1.2 per cent fast, so the mismatch GROWS
# through the run (about 2.3 s over this one) and no single offset can align it. So both are fitted:
#   for each candidate drift rate, the offset is taken as the MEDIAN residual from the taps to their
#   nearest beats, and the pair with the smallest mean distance wins. The median absorbs the taps that
#   are far out because the user's own timing is approximate.
# Then: the nearest beat to each tap is labelled beat 1, the three after it 2,3,4, and the model's own
# answer (its bar column) is scored against those labels.
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
Write-Host ("beats in log: {0}   {1}..{2} ms" -f $n, $btMs[0], $btMs[$n-1])

# MUSIC BEATS ONLY (22 Oct). USER: "The intro and quiet sections (talking) were all included in this
# dataset... there were many occasions where I hit the button and there was no beat - that is me being the
# metronome." The board keeps its slow IDLE ticks (~2000 ms) when nothing is playing, and those are NOT
# music beats. Left in, they absorb the taps made during the quiet sections and pollute the whole label
# set. The user's own timing is therefore no guide here either - what marks a music section is simply that
# the beats are close together, so the log is cut into runs of closely-spaced beats and only those runs
# are kept.
$GAP = 700
$keepIdx = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt $n; $i++) {
  $prevOk = $false; $nextOk = $false
  if ($i -gt 0)     { if (($btMs[$i] - $btMs[$i-1]) -le $GAP) { $prevOk = $true } }
  if ($i -lt $n - 1) { if (($btMs[$i+1] - $btMs[$i]) -le $GAP) { $nextOk = $true } }
  if ($prevOk -or $nextOk) { [void]$keepIdx.Add($i) }
}
$tot0 = $n
$fMs = New-Object System.Collections.ArrayList
$fBar = New-Object System.Collections.ArrayList
$fSn = New-Object System.Collections.ArrayList
$fMd = New-Object System.Collections.ArrayList
foreach ($i in $keepIdx) {
  [void]$fMs.Add($btMs[$i]); [void]$fBar.Add($btBar[$i])
  [void]$fSn.Add($btSn[$i]); [void]$fMd.Add($btMd[$i])
}
$btMs = $fMs; $btBar = $fBar; $btSn = $fSn; $btMd = $fMd
$n = $btMs.Count
Write-Host ("music beats kept: {0} of {1}" -f $n, $tot0)
Write-Host ("  music span {0}..{1} ms" -f $btMs[0], $btMs[$n-1])

$rows = New-Object System.Collections.ArrayList
foreach ($ln in (Get-Content 'beat1_taps_LABEL.csv' | Select-Object -Skip 1)) {
  $f = $ln -split ','
  [void]$rows.Add([pscustomobject]@{ tick = [int]$f[1] })
}
$runs = New-Object System.Collections.ArrayList
$cur = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt $rows.Count; $i++) {
  if ($i -gt 0 -and ($rows[$i].tick - $rows[$i-1].tick) -gt 6000) { [void]$runs.Add($cur); $cur = New-Object System.Collections.ArrayList }
  [void]$cur.Add($rows[$i])
}
if ($cur.Count -gt 0) { [void]$runs.Add($cur) }
$run = @($runs[$runs.Count - 1])
# CRITICAL: keep ONLY the taps the beat log actually covers. The board keeps a 512-beat ring, so the log
# holds the last ~124 s while the run was ~195 s - and a tap outside the log gets its "nearest beat" as
# the log's end, which wrecked the fit (a mean distance of 13.7 SECONDS). The pairs give the offset well
# enough to know which taps are inside.
$offs = New-Object System.Collections.ArrayList
foreach ($ln in (Get-Content 'beat1_pairs_TEST.csv')) {
  if ($ln -notmatch '^[0-9]') { continue }
  $f = $ln -split ','
  [void]$offs.Add([int]$f[1] - [int]$f[0])
}
$offsSort = @($offs | Sort-Object)
$K0 = $offsSort[[int]($offsSort.Count / 2)]
$keep = New-Object System.Collections.ArrayList
foreach ($x in $run) {
  $m = $x.tick + $K0
  if ($m -ge $btMs[0] -and $m -le $btMs[$n-1]) { [void]$keep.Add($x) }
}
$run = @($keep)
$tk = New-Object double[] $run.Count
for ($i = 0; $i -lt $run.Count; $i++) { $tk[$i] = [double]$run[$i].tick }
Write-Host ("taps: {0} of the last run are INSIDE the beat log (offset {1} from the pairs)" -f $run.Count, $K0)

# nearest beat index for a mapped time
function NearBeat([double]$m) {
  $j = 0
  while ($j -lt $n - 1 -and $btMs[$j + 1] -le $m) { $j++ }
  if ($j -gt 0) { if ([Math]::Abs($btMs[$j-1] - $m) -lt [Math]::Abs($btMs[$j] - $m)) { return $j - 1 } }
  return $j
}

$bestR = 0.0; $bestK = 0.0; $bestScore = 1e18
$t0 = $tk[0]
for ($r = -0.002; $r -le 0.020; $r += 0.0005) {
  # provisional mapping, then the median residual becomes the offset
  $res = New-Object System.Collections.ArrayList
  for ($i = 0; $i -lt $tk.Count; $i++) {
    $m = ($tk[$i] - $t0) * (1.0 + $r) + $btMs[0]
    $j = NearBeat $m
    [void]$res.Add($btMs[$j] - $m)
  }
  $rs = $res | Sort-Object
  $k = $rs[[int]($rs.Count / 2)]
  # score with that offset applied
  $sum = 0.0
  for ($i = 0; $i -lt $tk.Count; $i++) {
    $m = ($tk[$i] - $t0) * (1.0 + $r) + $btMs[0] + $k
    $j = NearBeat $m
    $d = [Math]::Abs($btMs[$j] - $m)
    if ($j -gt 0) { $d2 = [Math]::Abs($btMs[$j-1] - $m); if ($d2 -lt $d) { $d = $d2 } }
    $sum += $d
  }
  $sc = $sum / $tk.Count
  if ($sc -lt $bestScore) { $bestScore = $sc; $bestR = $r; $bestK = $k }
}
Write-Host ("BEST FIT: drift {0:N4} ({1:N2} per cent)   offset {2:N0} ms   mean tap-to-beat distance {3:N0} ms" -f $bestR, ($bestR*100), $bestK, $bestScore)

# label: the nearest beat to each tap is beat 1
$lab = New-Object int[] $n
$hits = 0
for ($i = 0; $i -lt $tk.Count; $i++) {
  $m = ($tk[$i] - $t0) * (1.0 + $bestR) + $btMs[0] + $bestK
  $j = NearBeat $m
  if ([Math]::Abs($btMs[$j] - $m) -gt 222) { continue }   # beyond half a beat it is not a label at all
  $hits++
  for ($q = 0; $q -lt 4; $q++) { if ($j + $q -lt $n) { $lab[$j + $q] = $q + 1 } }
}
Write-Host ("labelled {0} of {1} taps (within half a beat)" -f $hits, $tk.Count)

$cur = 0; $tot = 0
for ($i = 0; $i -lt $n; $i++) {
  if ($lab[$i] -eq 0) { continue }
  $tot++
  if ( ($btBar[$i] -eq 0) -eq ($lab[$i] -eq 1) ) { $cur++ }
}
if ($tot -gt 0) {
  Write-Host ''
  Write-Host ("CURRENT MODEL: {0} of {1} = {2:N1} per cent" -f $cur, $tot, (100.0*$cur/$tot))
  Write-Host ("  (a model that always guessed would get 25 per cent; a model that never changed would get about 25 too)")
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('ms,bar,sn,md,label')
for ($i = 0; $i -lt $n; $i++) { [void]$sb.AppendLine(('{0},{1},{2},{3},{4}' -f $btMs[$i], $btBar[$i], $btSn[$i], $btMd[$i], $lab[$i])) }
[System.IO.File]::WriteAllText((Join-Path $here 'beat1_labelled_TEST.csv'), $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host 'wrote beat1_labelled_TEST.csv'