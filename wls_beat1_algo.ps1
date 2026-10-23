# ===================================================================================================
# BEAT-1 ALGORITHM TESTS - DEVELOPMENT TOOL. Scores candidate algorithms against the tap labels.
# The labels come from wls_beat1_v2.ps1: the beat nearest each of the user's taps is beat 1.
# Each candidate is scored the same way the current model was: beat by beat, is "this is beat 1" right?
# A guess scores 25 per cent, so anything near that is doing nothing.
# ===================================================================================================
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

$ms = New-Object System.Collections.ArrayList
$bar = New-Object System.Collections.ArrayList
$sn = New-Object System.Collections.ArrayList
$md = New-Object System.Collections.ArrayList
$lab = New-Object System.Collections.ArrayList
foreach ($ln in (Get-Content 'beat1_labelled_TEST.csv')) {
  if ($ln -notmatch '^[0-9]') { continue }
  $f = $ln -split ','
  [void]$ms.Add([int]$f[0]); [void]$bar.Add([int]$f[1])
  [void]$sn.Add([double]$f[2]); [void]$md.Add([double]$f[3]); [void]$lab.Add([int]$f[4])
}
$n = $ms.Count
$nl = 0
foreach ($x in $lab) { if ($x -gt 0) { $nl++ } }
Write-Host ("beats: {0}   labelled: {1}" -f $n, $nl)

# score a per-beat "is this beat 1" prediction against the labels
function Score-Pred($pred) {
  $ok = 0; $tot = 0
  for ($i = 0; $i -lt $n; $i++) {
    if ($lab[$i] -eq 0) { continue }
    $tot++
    if ($pred[$i] -eq ($lab[$i] -eq 1)) { $ok++ }
  }
  if ($tot -eq 0) { return @{ ok = 0; tot = 0; pct = 0.0 } }
  return @{ ok = $ok; tot = $tot; pct = (100.0*$ok/$tot) }
}

# a bar is four beats: walk the labelled beats and group them
$barStart = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt $n; $i++) { if ($lab[$i] -eq 1) { [void]$barStart.Add($i) } }

Write-Host ''
Write-Host '--- the current model (bar column == 0) ---'
$cur = New-Object bool[] $n
for ($i = 0; $i -lt $n; $i++) { $cur[$i] = ($bar[$i] -eq 0) }
$r = Score-Pred $cur
Write-Host ("  {0} of {1} = {2:N1} per cent" -f $r.ok, $r.tot, $r.pct)

# CANDIDATE A: the beat with the LOWEST snare reading in its bar. The snare is on 2 and 4, so beat 1 and
# beat 3 are the quiet ones - which is why this alone cannot separate them, and the score will show it.
Write-Host ''
Write-Host '--- A: lowest snare reading in the bar ---'
$pA = New-Object bool[] $n
foreach ($s0 in $barStart) {
  $best = -1; $bv = 1e9
  for ($q = 0; $q -lt 4; $q++) { $ix = $s0 + $q; if ($ix -ge $n) { break }; if ($sn[$ix] -lt $bv) { $bv = $sn[$ix]; $best = $ix } }
  if ($best -ge 0) { $pA[$best] = $true }
}
$r = Score-Pred $pA
Write-Host ("  {0} of {1} = {2:N1} per cent" -f $r.ok, $r.tot, $r.pct)

# CANDIDATE B: the biggest CHANGE in the mid band within the bar. Chords and bass move on beat 1.
Write-Host ''
Write-Host '--- B: biggest mid-band change in the bar ---'
$pB = New-Object bool[] $n
foreach ($s0 in $barStart) {
  $best = -1; $bv = -1e9
  for ($q = 0; $q -lt 4; $q++) {
    $ix = $s0 + $q
    if ($ix -ge $n) { break }
    $d = $md[$ix] - $md[$ix - 1]
    if ($d -gt $bv) { $bv = $d; $best = $ix }
  }
  if ($best -ge 0) { $pB[$best] = $true }
}
$r = Score-Pred $pB
Write-Host ("  {0} of {1} = {2:N1} per cent" -f $r.ok, $r.tot, $r.pct)

# CANDIDATE D: THE SNARE CONTRAST VOTED OVER A WINDOW (22 Oct).
# The snare sits on beats 2 and 4, so for the CORRECT phase the snare is high on the 2s and 4s and low on
# the 1s and 3s. Judging one bar at a time is noisy (candidate A got 80 per cent that way); averaging the
# contrast over a WINDOW of bars should be steadier, because a single odd bar cannot swing it.
# For each labelled bar the phase is chosen to maximise the contrast across its neighbours.
Write-Host ''
Write-Host '--- D: snare contrast voted over a window of bars ---'
foreach ($win in 1, 2, 4, 8) {
  $pD = New-Object bool[] $n
  foreach ($s0 in $barStart) {
    $bestP = 0; $bestC = -1e18
    for ($p = 0; $p -lt 4; $p++) {
      # the two positions that would be the snares if this phase were correct
      $s1 = ($p + 1) % 4
      $s2 = ($p + 3) % 4
      $hiSum = 0.0; $loSum = 0.0; $cnt = 0
      for ($b = -$win; $b -le $win; $b++) {
        $base = $s0 + 4 * $b
        if ($base -lt 0 -or $base + 3 -ge $n) { continue }
        $hiSum += $sn[$base + $s1] + $sn[$base + $s2]
        $loSum += $sn[$base + $p] + $sn[$base + ($p + 2) % 4]
        $cnt++
      }
      if ($cnt -eq 0) { continue }
      $c = ($hiSum - $loSum) / $cnt
      if ($c -gt $bestC) { $bestC = $c; $bestP = $p }
    }
    # the phase that won says which of the four beats is beat 1
    for ($q = 0; $q -lt 4; $q++) { if ($q -eq $bestP) { $pD[$s0 + $q] = $true } }
  }
  $r = Score-Pred $pD
  Write-Host ("  window +/-{0,2} bars: {1} of {2} = {3:N1} per cent" -f $win, $r.ok, $r.tot, $r.pct)
}

# CANDIDATE E: A (the lowest snare in the bar) CORRECTED BY D's window vote. A decides beat by beat, D
# decides the phase over a neighbourhood; where they disagree D should win, because it has more evidence.
Write-Host ''
Write-Host '--- E: A corrected by the window vote (window 4) ---'
$win = 4
$pE = New-Object bool[] $n
foreach ($s0 in $barStart) {
  $bestP = 0; $bestC = -1e18
  for ($p = 0; $p -lt 4; $p++) {
    $s1 = ($p + 1) % 4
    $s2 = ($p + 3) % 4
    $hiSum = 0.0; $loSum = 0.0; $cnt = 0
    for ($b = -$win; $b -le $win; $b++) {
      $base = $s0 + 4 * $b
      if ($base -lt 0 -or $base + 3 -ge $n) { continue }
      $hiSum += $sn[$base + $s1] + $sn[$base + $s2]
      $loSum += $sn[$base + $p] + $sn[$base + ($p + 2) % 4]
      $cnt++
    }
    if ($cnt -eq 0) { continue }
    $c = ($hiSum - $loSum) / $cnt
    if ($c -gt $bestC) { $bestC = $c; $bestP = $p }
  }
  $pE[$s0 + $bestP] = $true
}
$r = Score-Pred $pE
Write-Host ("  {0} of {1} = {2:N1} per cent" -f $r.ok, $r.tot, $r.pct)
Write-Host ''
Write-Host '--- C: low snare AND a mid-band change (weighted) ---'
foreach ($w in 0.25, 0.5, 1.0, 2.0) {
  $pC = New-Object bool[] $n
  foreach ($s0 in $barStart) {
    $best = -1; $bv = -1e9
    for ($q = 0; $q -lt 4; $q++) {
      $ix = $s0 + $q
      if ($ix -ge $n) { break }
      $mx = 0.0
      for ($q2 = 0; $q2 -lt 4; $q2++) { $iy = $s0 + $q2; if ($iy -lt $n -and $sn[$iy] -gt $mx) { $mx = $sn[$iy] } }
      $snN = 0.0
      if ($mx -gt 0.0) { $snN = $sn[$ix] / $mx }
      $ch = $md[$ix] - $md[$ix - 1]
      $s = (-1.0 * $snN) + ($w * $ch)
      if ($s -gt $bv) { $bv = $s; $best = $ix }
    }
    if ($best -ge 0) { $pC[$best] = $true }
  }
  $r = Score-Pred $pC
  Write-Host ("  weight {0,4}: {1} of {2} = {3:N1} per cent" -f $w, $r.ok, $r.tot, $r.pct)
}