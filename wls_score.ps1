param(
  [Parameter(Mandatory = $true)][string]$Label,
  [Parameter(Mandatory = $true)][double]$Bpm,
  [int]$Listener = -1,
  [double]$TolFrac = 0.15
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$csv = Join-Path $here ("beat_${Label}_diag.csv")
$ons = Join-Path $here ("beat_${Label}_onsets.txt")
$out = Join-Path $here ("beat_${Label}_score.txt")
$P = 60000.0 / $Bpm
# NOTE: PowerShell variable names are CASE-INSENSITIVE. The accumulator must NOT be called
# $R: the CSV row variable $r is the SAME variable, so once section 1 has run $R.Add()
# fails with "PSCustomObject does not contain a method named Add". Same trap with $P and $p.
$report = New-Object System.Collections.ArrayList
function Rep([string]$s) { [void]$report.Add($s); Write-Host $s }

Rep ('SCORE label={0} BPM={1} period={2:F1} ms   tolerance {3:F0} ms ({4:P0} of a period)' -f $Label, $Bpm, $P, ($TolFrac * $P), $TolFrac)

# ---- 1. the playing window, from /diag -------------------------------------
$rows = Import-Csv $csv
if (($null -eq $rows) -or ($rows.Count -lt 3)) { throw 'diag csv is empty' }
$t = New-Object 'System.Collections.Generic.List[double]'
$lvl = New-Object 'System.Collections.Generic.List[double]'
$up = New-Object 'System.Collections.Generic.List[double]'
foreach ($r in $rows) {
  $t.Add([double]$r.t)
  $v = 0.0; [void][double]::TryParse([string]$r.aBlvl, [ref]$v); $lvl.Add($v)
  $u = 0.0; [void][double]::TryParse([string]$r.uptimeSec, [ref]$u); $up.Add($u)
}
$maxL = ($lvl | Measure-Object -Maximum).Maximum
$gate = [Math]::Max(0.06, 0.10 * $maxL)   # 10% of the music peak: 25% chopped run 1 into an 83 s window
$bs = -1; $be = -1; $s = -1
for ($i = 0; $i -lt $lvl.Count; $i++) {
  if ($lvl[$i] -ge $gate) { if ($s -lt 0) { $s = $i } }
  elseif ($s -ge 0) { if (($i - 1 - $s) -gt ($be - $bs)) { $bs = $s; $be = $i - 1 }; $s = -1 }
}
if (($s -ge 0) -and (($lvl.Count - 1 - $s) -gt ($be - $bs))) { $bs = $s; $be = $lvl.Count - 1 }
if ($bs -lt 0) { throw 'no playing section found: aBlvl never rose above the gate' }
$w0 = $up[$bs] * 1000.0 + 1500.0
$w1 = $up[$be] * 1000.0 - 500.0
Rep ('window  {0:F1}s..{1:F1}s of the sampler   board ms {2:F0}..{3:F0}   aBlvl max {4:F2} gate {5:F2}   polls {6}' -f $t[$bs], $t[$be], $w0, $w1, $maxL, $gate, $rows.Count)

# ---- 2. the onsets --------------------------------------------------------
$all = New-Object System.Collections.ArrayList
foreach ($ln in (Get-Content $ons)) {
  if ($ln -notmatch '^ms=') { continue }
  $h = @{}
  foreach ($kv in ($ln.Trim() -split ' ')) { $kvp = $kv -split '='; if ($kvp.Count -eq 2) { $h[$kvp[0]] = $kvp[1] } }
  $ms = [double]$h['ms']

  # ONE LINE PER ONSET, with a listener mask (bit i = listener i). An older file has
  # listener= on every line, so both forms are accepted.
  $mk = 0
  if ($h.ContainsKey('mask')) { $mk = [Convert]::ToInt32($h['mask'], 16) }
  elseif ($h.ContainsKey('listener')) { $mk = (1 -shl [int]$h['listener']) }
  [void]$all.Add([pscustomobject]@{ ms = $ms; mask = $mk; nov = [double]$h['nov']; thr = [double]$h['thr'] })
}
if ($all.Count -eq 0) { throw 'no onsets at all in the file' }

# THE WINDOW IS THE ONSET SPAN. The detector only fires on sound, so the first and last
# onset bound the music far more accurately than the 10 s /diag level sampling - which
# chopped run 3 to a 55 s window of a 248 s track. The level-gated window is still
# computed and reported for comparison.
$firstMs = ($all | Measure-Object ms -Minimum).Minimum
$lastMs  = ($all | Measure-Object ms -Maximum).Maximum
$all = @($all | Where-Object { $_.ms -ge ($firstMs + 500) -and $_.ms -le ($lastMs - 500) })
Rep ('onset span  board ms {0:F0}..{1:F0}  = {2:F1} s of music, {3} onsets   (level-gated window was {4:F0}..{5:F0})' -f $firstMs, $lastMs, (($lastMs - $firstMs) / 1000.0), $all.Count, $w0, $w1)

# SPLIT AT GAPS OVER 3 s and score the LARGEST block, reporting what was excluded. Run 4
# contained a 190 s interruption (the level trace confirms the sound stopped), which would
# otherwise read as about 380 missed beats.
$blk = New-Object System.Collections.ArrayList
$s = 0
for ($i = 1; $i -le $all.Count; $i++) {
  $gap = 0
  if ($i -lt $all.Count) { $gap = $all[$i].ms - $all[$i - 1].ms }
  if (($i -eq $all.Count) -or ($gap -gt 3000)) { [void]$blk.Add(@($s, ($i - 1))); $s = $i }
}
$bestB = $null; $bestN = -1; $excluded = 0
foreach ($b in $blk) {
  $nn = $b[1] - $b[0] + 1
  if ($nn -gt $bestN) { $bestN = $nn; $bestB = $b } else { $excluded += $nn }
}
if ($blk.Count -gt 1) {
  Rep ('blocks: {0} (separated by gaps over 3 s) - scoring the largest ({1} onsets); {2} onsets in the other block(s) are NOT scored' -f $blk.Count, $bestN, $excluded)
}
$all = @($all[$bestB[0]..$bestB[1]])

Rep ''
Rep 'PER-LISTENER (counted from the onset mask: bit i = listener i took that onset)'
for ($li = 0; $li -le 10; $li++) {
  $cnt = @($all | Where-Object { ($_.mask -band (1 -shl $li)) -ne 0 }).Count
  if ($cnt -gt 0) { Rep ('  listener {0,2}   onsets {1,5}' -f $li, $cnt) }
}

# ---- 3. choose the listener ----------------------------------------------
if ($Listener -lt 0) {
  $bestN = -1
  for ($li = 0; $li -le 10; $li++) {
    $cnt = @($all | Where-Object { ($_.mask -band (1 -shl $li)) -ne 0 }).Count
    if ($cnt -gt $bestN) { $bestN = $cnt; $Listener = $li }
  }
}
$o = @($all | Where-Object { ($_.mask -band (1 -shl $Listener)) -ne 0 } | Sort-Object ms)
Rep ''
Rep ('LISTENER {0}: {1} real onsets scored' -f $Listener, $o.Count)
if ($o.Count -lt 8) { Rep '  TOO FEW ONSETS to score - the detector is barely firing'; $report | Set-Content -Path $out; return }

# ---- 4. intervals --------------------------------------------------------
$iv = New-Object 'System.Collections.Generic.List[double]'
for ($i = 1; $i -lt $o.Count; $i++) { $iv.Add($o[$i].ms - $o[$i - 1].ms) }
$srt = @($iv | Sort-Object)
$med = $srt[[int]($srt.Count / 2)]
$p90 = $srt[[int]($srt.Count * 0.9)]
$mean = ($iv | Measure-Object -Average).Average
Rep ('  measured interval: median {0:F0} ms   mean {1:F0} ms   p90 {2:F0} ms   (track period {3:F0} ms)' -f $med, $mean, $p90, $P)
Rep ('  implied BPM from the median interval: {0:F1}' -f (60000.0 / $med))
$short = @($iv | Where-Object { $_ -lt (0.35 * $P) }).Count
$long = @($iv | Where-Object { $_ -gt (1.6 * $P) }).Count
Rep ('  intervals under 35% of a period (double triggers): {0}    over 160% (a gap): {1}' -f $short, $long)

# ---- 5. grid fit and score ----------------------------------------------
$t0 = $o[0].ms
$res = New-Object 'System.Collections.Generic.List[double]'
foreach ($x in $o) { $k = [Math]::Round(($x.ms - $t0) / $P); $res.Add($x.ms - $t0 - $k * $P) }
$rs = @($res | Sort-Object)
$off = $rs[[int]($rs.Count / 2)]
$base = $t0 + $off
$ks = New-Object 'System.Collections.Generic.List[int]'
foreach ($x in $o) { $ks.Add([int][Math]::Round(($x.ms - $base) / $P)) }
$dup = $o.Count - @($ks | Sort-Object -Unique).Count
$missed = 0
$kprev = $ks[0]
for ($i = 1; $i -lt $ks.Count; $i++) { $d = $ks[$i] - $kprev; if ($d -gt 1) { $missed += ($d - 1) }; $kprev = $ks[$i] }
$err = New-Object 'System.Collections.Generic.List[double]'
for ($i = 0; $i -lt $o.Count; $i++) { $err.Add($o[$i].ms - $base - $ks[$i] * $P) }
$sq = ($err | ForEach-Object { $_ * $_ } | Measure-Object -Sum).Sum
$rms = [Math]::Sqrt($sq / $err.Count)
$tol = $TolFrac * $P
$ontime = @($err | Where-Object { [Math]::Abs($_) -le $tol }).Count
$grid = $ks[$ks.Count - 1] - $ks[0] + 1
Rep ''
Rep ('SCORE   grid {0} beats, {1} onsets' -f $grid, $o.Count)
Rep ('  missed (a grid beat with no onset)  : {0}' -f $missed)
Rep ('  duplicated (2 onsets on 1 grid beat): {0}' -f $dup)
Rep ('  timing: offset {0:F0} ms, RMS {1:F0} ms, on time {2} of {3} ({4:P1})' -f $off, $rms, $ontime, $o.Count, ($ontime / $o.Count))
Rep ('  MISSED + DUPLICATED = {0}    timing RMS = {1:F0} ms' -f ($missed + $dup), $rms)

# ---- 6. detector headroom and the diag deltas ---------------------------
$ratio = @()
foreach ($x in $o) { if ($x.thr -gt 0) { $ratio += ($x.nov / $x.thr) } }
$rSrt = @($ratio | Sort-Object)
if ($rSrt.Count -gt 0) { Rep ('HEADROOM  nov/thr: min {0:F2}  median {1:F2}  max {2:F2}' -f $rSrt[0], $rSrt[[int]($rSrt.Count / 2)], $rSrt[$rSrt.Count - 1]) }
$r0 = $rows[$bs]
$r1 = $rows[$be]
$line = 'DIAG DELTA (window): '
foreach ($k in @('bOns','bActs','bKicks','chSwal','odfHz','odfLo','odfHi','bNov','bThr','p0thr','p0ons','p0swl','aBlvl','aEnv')) {
  if (($null -ne $r0.$k) -and ($null -ne $r1.$k)) {
    $a = 0.0; $b = 0.0
    [void][double]::TryParse([string]$r0.$k, [ref]$a); [void][double]::TryParse([string]$r1.$k, [ref]$b)
    $line += ('{0}={1:F2} ' -f $k, ($b - $a))
  }
}
Rep $line
# ---- 7. RECORD COMPLETENESS - the rings are finite, so prove nothing was lost -----
$allMs = New-Object System.Collections.ArrayList
foreach ($ln in (Get-Content $ons)) { if ($ln -match '^ms=(\d+)') { [void]$allMs.Add([int]$Matches[1]) } }
$distinctAll = @($allMs | Sort-Object -Unique).Count
$head0 = [int]$rows[0].onsHead
$headN = [int]$rows[$rows.Count - 1].onsHead
$expect = $headN - $head0
Rep ''
if ($expect -gt 0) {
  Rep ('RECORD COMPLETENESS  {0} distinct onsets captured of {1} the board recorded ({2:P0})' -f $distinctAll, $expect, ($distinctAll / [double]$expect))
} else {
  Rep ('RECORD COMPLETENESS  distinct onsets captured {0}  (onsHead did not advance)' -f $distinctAll)
}
Rep 'AUTHORITATIVE per-listener onsets and debounce rejections (cumulative counters, never reset):'
$l2 = '  '
foreach ($k in @('p0ons','p1ons','p2ons','p3ons','p4ons','p5ons','p6ons','p7ons','p8ons','p9ons','p0swl','p9swl','chSwal')) {
  if (($null -ne $r0.$k) -and ($null -ne $r1.$k)) { $l2 += ('{0}={1} ' -f $k, ([int]$r1.$k - [int]$r0.$k)) }
}
Rep $l2
Rep '  NOTE: bOns/bActs/bKicks are RESET by the 30 s heartbeat, so a delta of those is NOT a count. The pN* counters are.'
$report | Set-Content -Path $out
Rep ('written to ' + $out)