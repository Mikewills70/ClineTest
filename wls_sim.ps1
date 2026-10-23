# ===================================================================================================
# OFFLINE BEAT-CLOCK SIMULATION - DEVELOPMENT TOOL
# ===================================================================================================
# WHY: four attempts at the drift failed on the board, each because I could not test a change without
# flashing it. This runs the same logic on the laptop in seconds, so a candidate fix is validated before
# it ever reaches the hardware.
#
# WHAT IT MODELS, taken from the measurements:
#   the MUSIC: a beat every BEAT_MS, with an off-beat onset halfway between. That 50/50 mix is the thing
#              the day-one analysis identified as the source of the half-beat confusion.
#   the ONSETS jitter, as real onsets do (ONSET_JITTER_MS).
#   the TEMPO SEARCH: a periodicity search over the recent onsets, a median of three, then a slow average
#              - the structure of the real code.
#   the CLOCK: schedules each beat with its period plus a trim, and corrects from the phase error.
#
# WHAT IT REPORTS: the period the clock settles on against the music's true 449 ms, and the drift over the
# run. That is the number the user sees as IO48 advancing.
# ===================================================================================================
param(
  [double]$BeatMs       = 449.0,   # the music's beat, measured from the user's presses (1797/4)
  [int]$Beats           = 400,
  [int]$OnsetJitterMs   = 12,
  [double]$TrimPct      = 0.0,     # the candidate fix: a constant percentage added to the period
  [int]$Seed            = 7,
  [switch]$HalfGuard,              # model the half-beat guard: a period under 350 ms is doubled
  [switch]$Sticky,                 # and make that decision sticky, as the board now tries to
  [switch]$MidDisc,                # accept an onset as a beat only when the mid band says it is one
  [switch]$NoTrim                  # disable the rate trim (it runs away and makes things worse)
)

$rand = New-Object System.Random $Seed
$gSticky = $false

# ---- the music: onsets on the beat and on the off-beat -------------------------------------------------
# THE DISCRIMINATOR, modelled from the real bands: a KICK (on every beat) has a sharp mid-band click; a
# BASS note (on the off-beat) does not. That is what separates a beat from an off-beat at the instant it
# arrives - the snare cannot, because the snare is only on 2 and 4, which is a BAR cue, not a beat cue.
$onset = New-Object System.Collections.ArrayList
$isBeat = New-Object System.Collections.ArrayList
$midVal = New-Object System.Collections.ArrayList
$t = 1000.0
for ($b = 0; $b -lt $Beats; $b++) {
  $j1 = ($rand.NextDouble() * 2.0 - 1.0) * $OnsetJitterMs
  [void]$onset.Add($t + $j1)                       # the beat: a KICK
  [void]$isBeat.Add($true)
  [void]$midVal.Add(0.55 + $rand.NextDouble() * 0.45)   # a strong mid click
  $j2 = ($rand.NextDouble() * 2.0 - 1.0) * $OnsetJitterMs
  [void]$onset.Add($t + $BeatMs / 2.0 + $j2)       # the off-beat: a BASS note
  [void]$isBeat.Add($false)
  [void]$midVal.Add($rand.NextDouble() * 0.25)          # little or no mid click
  $t += $BeatMs
}

# ---- the clock and the search -------------------------------------------------------------------------
$period   = 0.0        # what the search believes, ms
$pHist    = New-Object double[] 3
$pIdx     = 0
$pN       = 0
$perAvg   = 0.0
$clkNext  = 0.0
$rateTrim = 0.0
$lastOnset = 0.0
$onsetBuf = New-Object System.Collections.ArrayList
$beatsFired = 0
$sumErr = 0.0
$errN = 0

foreach ($oi in 0..($onset.Count - 1)) {
  $o = $onset[$oi]
  $thisMid = $midVal[$oi]
  # IS THIS ONSET A BEAT OR AN OFF-BEAT? The mid-band click says so, at the instant it arrives. This is
  # the fix for the skipped beats: the phase window cannot tell them apart, which is what lost the beat.
  $looksLikeBeat = ($thisMid -ge 0.40)
  if ($lastOnset -gt 0) {
    $iv = $o - $lastOnset
    if ($iv -ge 120 -and $iv -le 1200) {
      [void]$onsetBuf.Add($iv)
      if ($onsetBuf.Count -gt 12) { $onsetBuf.RemoveAt(0) }
      # THE COMB, SIMPLIFIED: the most common spacing among the recent onsets, checked against double it.
      # With beats and off-beats mixed, the raw spacings are ~half the beat and the beat; the search picks
      # whichever explains the recent onsets best, which is what the real one does.
      $cand = @()
      foreach ($x in $onsetBuf) { $cand += $x; if ($x * 2 -ge 120 -and $x * 2 -le 1200) { $cand += ($x * 2) } }
      $bestP = 0.0; $bestScore = -1.0
      foreach ($c in $cand) {
        $sc = 0.0
        foreach ($x in $onsetBuf) {
          $r = [Math]::Abs(($x / $c) - [Math]::Round($x / $c))
          $sc += (1.0 - $r)
        }
        if ($sc -gt $bestScore) { $bestScore = $sc; $bestP = $c }
      }
      if ($bestP -ge 120) {
        $pHist[$pIdx] = $bestP
        $pIdx = ($pIdx + 1) % 3
        if ($pN -lt 3) { $pN++ }
        $mid = 0.0
        if ($pN -lt 3) { $mid = $bestP } else {
          $s3 = @($pHist[0], $pHist[1], $pHist[2]) | Sort-Object
          $mid = $s3[1]
        }
        if ($perAvg -le 0) { $perAvg = $mid } else { $perAvg += ($mid - $perAvg) / 8.0 }
        $period = $perAvg
        if ($HalfGuard) {
          if ($Sticky) {
            if (-not $gSticky -and $period -lt 350) { $gSticky = $true }
            if ($gSticky -and $period -gt 500) { $gSticky = $false }
            if ($gSticky) { while ($period -lt 350) { $period *= 2 } }
          } else {
            while ($period -lt 350) { $period *= 2 }
          }
        }
        $period = $period * (1.0 + $TrimPct)
      }
    }
  }
  $lastOnset = $o

  # THE CLOCK: while beats are due, fire them and correct from the phase error
  while ($period -gt 0 -and $clkNext -gt 0 -and $o -ge $clkNext) {
    $beatsFired++
    $err = $o - $clkNext
    $rej = $period / 3.0
    $accept = $false
    if ($MidDisc) {
      # THE CANDIDATE FIX: accept this onset as a beat ONLY if the mid band says it is one. An off-beat
      # arrival is then ignored outright, instead of being judged by how far it is from the schedule -
      # which is what let the clock skip a real beat and slip.
      $accept = $looksLikeBeat
    } else {
      $accept = ([Math]::Abs($err) -le $rej)
    }
    if ($accept) {
      # THE PHASE PULL, as the real code does: a quarter of the error is taken out of the schedule. Without
      # this the model's beats sat permanently ~100 ms from the onsets, which made it an unfair test of the
      # discriminator - the gap was my model's fault, not the logic's.
      $clkNext += ($period + $rateTrim) + ($err * 0.25)
      if (-not $NoTrim) {
        $rateTrim += $err / 8.0          # THE CANDIDATE FIX, per BEAT
        if ($rateTrim -gt 60) { $rateTrim = 60 }
        if ($rateTrim -lt -60) { $rateTrim = -60 }
      }
      $sumErr += $err
      $errN++
    } else {
      $clkNext += ($period + $rateTrim)   # a spurious onset does not move the timer
    }
  }
  if ($period -gt 0 -and $clkNext -eq 0) { $clkNext = $o + $period }
}

Write-Host ''
Write-Host '=== SIMULATION ===' -ForegroundColor Cyan
Write-Host ("  the music's beat        : {0:N1} ms" -f $BeatMs)
Write-Host ("  beats simulated         : {0}" -f $Beats)
Write-Host ("  candidate trim          : {0:N3} per cent" -f ($TrimPct * 100))
Write-Host ("  the clock settled on    : {0:N1} ms" -f $period)
Write-Host ("  error against the music : {0:N2} per cent" -f (100.0 * ($period - $BeatMs) / $BeatMs))
Write-Host ("  clock beats fired       : {0}" -f $beatsFired)
Write-Host ("  mean phase error        : {0:N1} ms" -f ($(if ($errN -gt 0) { $sumErr / $errN } else { 0 })))
Write-Host ("  rate trim ended at      : {0:N1} ms" -f $rateTrim)
$driftPerBeat = ($period - $BeatMs)
Write-Host ("  DRIFT over the run      : {0:N0} ms  ({1:N2} beats)" -f ($driftPerBeat * $Beats), ($driftPerBeat * $Beats / $BeatMs)) -ForegroundColor Yellow