# AI LEARNINGS — mistakes I have actually made in this project, and the rule that prevents each

Written 23 Oct 2026, at the user's request: *"add any learnings you have made about yourself into a
document that will save you making the same mistakes over and over again."*

**Read this before starting work on this project.** Every entry is a real incident from this codebase,
not a general principle. Each one cost time or nearly cost correctness.

---

## 1. Replacement text that does not end in a newline JOINS TWO LINES

**Happened twice in one session, and the second one shipped to the board.**

- **B3:** a cut consumed the newline after `int lockedDownbeat = -1;`, and my replacement had no trailing
  newline — so the new comment and `int beatToggle = 0;` ended up on ONE line. That made the declaration
  part of a `//` comment, i.e. **it silently deleted a declaration.**
- **B5:** the same defect joined `server.on("/onsets", HTTP_GET, handleOnsets);` onto a comment line. The
  route was commented out, so **`handleOnsets` became unreferenced and the linker garbage-collected it** —
  the endpoint 404'd and the build looked 1,360 bytes *smaller* than it should have been.

**RULE:** every replacement string ends with a newline. After every cut, **read the boundary lines back**
and confirm the line above and below are intact and separate. Do not trust a script's "cut succeeded"
message — it reports only that a match was found, never that the result is sane.

## 2. Never report a size delta without checking nothing else was removed

Because of #1, I reported B5 as reclaiming **3,280 bytes**. The real figure was **1,920** — the rest was my
own bug making the build smaller by deleting an endpoint.

**RULE:** before quoting a byte figure, confirm (a) what was removed was genuinely dead, and (b) nothing
else left the build. A *smaller* build after a cut is a warning sign, not a win.

## 3. Check the trivial baseline before believing a score

A beat-1 harness reported **80.4%** and I nearly presented it as success. But 84 of the 112 labelled beats
were *not* beat 1, so a predictor answering "false" to everything already scores **75%**. The real per-bar
score was **60.7%** against a 25% chance baseline — the headline was almost entirely true negatives.

**RULE:** before believing any accuracy figure, compute what a trivial predictor would score, and state
the chance baseline next to the result.

## 4. Know which endpoint publishes what before declaring something missing

I read `snNov` from `/diag` and reported it missing. It was not missing — it lives in `/trav`, a text
response, not the JSON document. I nearly filed a false fault against my own work.

**RULE:** confirm the publishing endpoint before reporting a field as gone. `/diag` = JSON object;
`/trav` = a short text document.

## 5. Prove the code changed BEFORE investigating behaviour

The user reported the beat drifting and I assumed my clean-up had broken it. **The guard's hash proved the
beat code was byte-identical to the trusted baseline.** The real difference was environmental (the display
load), and I would have spent hours editing working code.

**RULE:** when behaviour changes, diff the code against the last known-good tag FIRST. If the code is
identical, stop looking at the code.

## 6. Find every call site, and check whether the result is even USED, before "refactoring"

Phase C was planned as a risky architectural **move** of direction-change behaviour from the audio path
into the pattern code. In fact `checkAudioBeat()` has **one** call site and **its return value is
deliberately discarded** — and the reversal already lived in the pattern code. The planned move was really
a **deletion of dead code**. Had I "moved" it as planned, I would have created a second reversal path and
broken the dot.

**RULE:** before refactoring, count call sites and check whether the return value is used. Dead code can
look exactly like live architecture. Prefer deleting to moving.

## 7. After every flash, check `displayActive` and re-select the pattern

`listeningStep()` — the whole beat engine — is called at the bottom of the `} else if (displayActive) {`
branch. A flash reboots the board, `displayActive` starts `false` by design, so **the strips, the 240V and
IO48 all go dark at once**. Three dead outputs from ONE gate. I flashed, did not re-select, and the user
found three dark displays and no beat indicator — which looked exactly like a serious regression.

**RULE:** after every flash, read `displayActive` from `/diag`, re-select the pattern, and confirm `p0clk`
is advancing BEFORE reporting anything. If `displayActive` is 0, no other reading means anything.

## 8. State hypotheses as hypotheses, and propose the measurement

I speculated at length about the half-beat cause. Rule 5 of this project exists precisely because guessing
wastes the user's time: *create the variables, run a test, read the results.*

**RULE:** label speculation as speculation. Offer the measurement that would settle it, and say what result
would confirm or refute it.

## 9. Treat the user's observation as DATA, not as a claim to argue with

Repeatedly in this project the user's instinct was right and mine was wrong: "the heaviest beat is beat 1"
was worth 18 points over the running model; "it was BASS BEAT that selected the wrong beat" was correct;
"the strips are fine, it's the beat" was correct. When I argued from theory instead, I lost.

**RULE:** an observation from the person watching the hardware outranks my model of it. Write it down
verbatim and test it.

## 10. PowerShell: do not use short function names

`function H($s)` collided with PowerShell's built-in `Get-History` alias and the script died with a
confusing `Cannot bind parameter 'Id'` error.

**RULE:** use multi-character, project-prefixed helper names in PowerShell scripts (`Nl`, `CutRx`, `Rep`).

## 11. Copy exact text; do not reconstruct it from memory

Two regex patterns failed because I typed the text from memory of a read:

- `if (mdNow > g_mdNormPeak) g_mdNormPeak += ...` — the real line has `= mdNow; else` in the middle.
- `beats\.` — the real text has a comma (`beats, dbMid on the midpoints`).

**RULE:** anchor patterns on short distinctive fragments rather than long transcribed lines, and prefer
`.*?` between two solid anchors. Let the pattern fail loudly rather than match the wrong thing.

## 12. Verify your own work adversarially, not sympathetically

The `/onsets` breakage was caught ONLY because I ran the post-flash endpoint check I had promised. Every
other check passed. The bug was invisible in the source, invisible in the build, and invisible in the
hash — it needed an actual request to the board.

**RULE:** after a change, exercise the things that were supposed to KEEP working, not just the casualty.

## 13. The build exceeds the 30-second command timeout

**RULE:** run `wls_build.ps1` detached (`Start-Process`), then poll `wls_build.log` for `error:` /
`Sketch uses` / `build finished`. Do not block on it.

## 14. Say what is proven and what is only suspected, every time

The most useful sentences I produced in this session were the ones separating the two: "the code is
byte-identical — proven" next to "the display load is the likely cause — not proven". The user could act
on the first and reason about the second. Blurring them would have sent us both down the wrong path.

**RULE:** mark every claim PROVEN or HYPOTHESIS, and say what would settle the hypotheses.

---

## The technique that worked, and is worth reusing

**An external hash plus a build that refuses to proceed.** `wls_beatguard.ps1` hashes the protected region;
`wls_build.ps1` refuses to build if it changed; `baseline/bassbeat_history.txt` is append-only and in git.

It caught a tampered tuning constant, ruled out the code during a false alarm, caught my own accidental
in-region edit, and stood as proof while the user reported a drift. **A guard that lives outside the code
it protects, and that blocks the build rather than merely warning, is the strongest protection available
inside a single repository.**

**And its honest limit, worth remembering:** it is a tripwire, not a lock. An agent that can edit the sketch
can edit the guard. What it cannot do quietly is rewrite git history — which is why the hash is also
recorded in an append-only file and in a tag.

---

## The one-line version, if you read nothing else

1. End every replacement with a newline and read the boundary back.
2. Compute the trivial baseline before trusting a number.
3. Diff against the last good tag before blaming the code.
4. Check `displayActive` after every flash.
5. Test what was supposed to keep working.
6. Mark claims PROVEN or HYPOTHESIS.
7. The user's observation is data.

