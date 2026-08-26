# 03 — Asset status: what is built, how it was graded, what is left

Live status of the sound pack. Companion to `02-SFX-Generation-Prompts.md` (the prompts) and
`tools/README.md` (the tooling). Numbers here come from `tools/sfx.py`, and every threshold is
calibrated against `Example/*.wav` — a spec must never reject the references themselves.

Last updated: 2026-08-23.

---

## 1. Where things live

| Path | What |
|---|---|
| `assets/sfx/` | **The pack.** Shippable 48 kHz / mono / 16-bit files, `<slot>_NN.wav` |
| `assets/sfx/library/` | **The library.** Every sfx that exists, named whatever it is, each with a `<file>.meta.ini` |
| `assets/preview/` | Rendered impact composites, for judging the stack as one sound |
| `takes/<slot>/` | Every generated take that passed, kept as source material |
| `takes/_split/` | Staging for multi-contact takes cut into one-shots |
| `takes/_shelf/` | Staging for takes shelved before `make` (only `crunch_gran` needed it) |
| `takes/ledger.csv` | Metrics and provenance for every take, including which pack file it became |
| `testbench/sounds/` | Copy of `assets/sfx/` that the testbench loads (see §5) |

---

## 2. Built: 29 files

Every slot `core/src/SlotManifest.cpp` declares is filled. All 29 pass `sfx.py eval`, and all
29 pass `tools/verify_pack.py`, which checks the delivery rules the spec does not: format,
lead-in, level match within a slot, headroom under runtime pitch shift, and loop seams.

| Slot | Files | Source | Key metrics |
|---|---|---|---|
| `imp_transient` | 3/3 | slap, knuckle crack, dowel crack | centroid 3629 / 4546 / 5792 Hz, -20 dB in 8-18 ms |
| `imp_body` | 3/3 | wet flour sack, wet sandbag, leather duffel | centroid 2166 / 2951 / 3425 Hz |
| `imp_body_limb` | **0/3** | nothing recorded — falls back to `imp_body`, so every limb impact plays the torso's wav | wanted: drier, tighter, higher, shorter than `imp_body` |
| `imp_sub` | 2/2 | **synthesised**, `tools/make_sub.py` | -20 dB in 48 ms, centroid 156 / 184 Hz |
| `limb_tap` | 4/4 | boot scuff x3 + forearm tap, all **split** from multi-contact takes | centroid 904 -> 3459 Hz |
| `scrape_loop` | 1/1 | limp body on rough stone slabs | 24 grains/s, tilt in band, **0.02 dB seam** |
| `air_whoosh` | 1/1 | low air movement, generated as a loop | centroid 218 Hz, 0.6 dB seam |
| `settle_rest` | 2/2 | slumping fabric + limb, body settling | -20 dB in 28 / 48 ms, soft attack |
| `surf_soft` | 2/2 | damp soil thump, weight into grass and earth | centroid 674 / 2475 Hz |
| `surf_wood` | 3/2 | hollow plank knock, old creaking boards, damped timber on soil | centroid 830 / 1348 / 2098 Hz, -20 dB in 40 / 78 / 48 ms |
| `surf_stone` | 2/2 | strike on rough granite | 94 / 118 ms, -20 dB in 8 / 12 ms |
| `head_impact` | 2/2 | melon in cloth, coconut in a leather sack | 226 / 496 ms, centroid 1100 / 2935 Hz |
| `crunch_gran` | 2/2 | gravel and gristle, **shelved** before `make` | 18 / 16 lomid transients, centroid 3752 / 4154 Hz |
| `gore_wet` | 2/2 | slow squeeze, soaked cloth on meat | centroid 1645 / 1666 Hz |
| `grunt_impact` / `scream_big` | 0/0 | **declared and unfilled by design** | record or licence -- see §4 |

### The composite

`assets/preview/composite_01..03.wav` stack transient / body / sub at 0 / +20 / +65 ms. Against the
four reference hero hits:

| | ours | references |
|---|---|---|
| crest | 6.1–10.1 dB | 6.1–11.1 dB |
| −20 dB in | 36–54 ms | 24–88 ms |
| tilt | +13.8 to +18.1 dB | +6.5 to +16.5 dB |

Band curve lands within **1.9 dB RMS** of the reference mean. Re-rendered at 48 kHz the stack is unmoved: tilt +13.8 to +18.1 against +13.2 to +18.2 before, -20 dB in 40-50 ms against 36-54 ms. Our −40 dB time is shorter
(180–196 ms vs 247–581 ms), which is expected: we author dry and the game's cell reverb supplies
that tail.

---

## 3. How takes were graded

`python tools/sfx.py eval <folder> --archive --suggest`. It finds the **event inside** a file and
judges that, not the raw container.

| Level | Meaning |
|---|---|
| `FIX` | The post-pass handles it — sample rate, stereo, pre-roll, level, over-length. Never sinks a take |
| `WARN` | Audible risk. Usually a second contact landing on `imp_sub`'s +55–75 ms arrival |
| `FAIL` | Wrong character. Regenerate |

Character is measured **after the slot's high-pass**, because that is what ships.

### What actually decides it

**Band tilt**, `(sub+low)/2 − (high+air)/2`. This is the honest discriminator and the one that
caught the most. Spectral centroid barely works: the reference hero hits measure 1950–3306 Hz
centroid while being clearly bass-led, because there are simply more FFT bins up top — the trap
`04-Reference-Analysis.md` §7 flags for the scrape. Centroid limits in `SPEC` are loose sanity
bounds only.

Then: usable event length, decay to −20 dB, transient count in 250–800 Hz (what separates a thud
from a crunch), stereo correlation, and for loops the grain rate, envelope steadiness and seam
match.

### Failure modes seen, in order of frequency

1. **Stereo correlation under 0.6** — 5 takes. Uncorrelated channels comb-filter when summed. The
   fix in `make` is to take the left channel instead; below ~0.45 the take is dead.
2. **Wrong tilt for the slot** — a wet meat slap is inherently mid-forward and cannot be a body
   layer; a "gritty scuff" is inherently multi-grain and cannot be a transient.
3. **Multiple contacts in one take** — not a failure. `sfx.py split` cuts them into separate
   one-shots at onsets ≥46 ms apart (the reference rate floor, §2 of the analysis). Five scuff
   takes yielded 26 pieces, 19 of which passed alone. All four `limb_tap` files came from this.
4. **Pre-roll** — 28 to 476 ms of room tone in front. Always fixable, but it *is* a timing bug once
   the plugin schedules layers at +15/+50/+65 ms.
5. **The model designs an event when the slot wants a texture** — every `air_whoosh` take in the
   first batch measured 8–15 dB of envelope spread against a 6 dB ceiling, because "rush" and
   "movement" get read as a gesture with a shape. The fix is to say the level explicitly:
   *one constant level throughout, no gusts, no swells*. Three of four passed on the retry.
6. **The model will not make a dull crunch.** `crunch_gran` took four prompt revisions and 30
   takes — duller materials, "no bright edge", "nothing above two kilohertz" — and every single
   take still landed at 5–6.7 kHz centroid against a 4500 Hz ceiling, while the reference spine
   break sits at 2705. Prompting cannot move this one; see §4.

### Three specs of mine that were wrong

Recorded because the same class of error will recur:

- **`imp_body` centroid 250–1600 Hz** rejected all 8 takes and would have rejected the Skate 3
  references. Recalibrated to tilt.
- **`scrape_loop` grains 35–95/s** came from the analysis's prose count of 65/s, measured with a
  different threshold than `count_peaks`. The reference slide measures 17/s here.
- **"`imp_sub` is the loudest layer"** — that is a statement about the composite's *band balance*,
  not layer gain. Mixed loudest it buries everything; solved against the reference band curve it
  sits **8 dB under `imp_transient`** (transient 0, body −3, sub −8).

---

## 4. Left to do

### Nothing is blocking

`scrape_loop_01` — the one file §4 used to be about — is rebuilt and passes. The old take was a
low rumble with no grit, and no amount of EQ put the mid content back. The fix was the prompt the
previous revision of this document proposed: describing the friction as *gritty broadband* with a
*clear midrange scrape* rather than as a low rumble. Two of three retakes passed.

The shipped cut starts at **3400 ms** into the take. `make` picks a loop window on steadiness,
which left a 3.9 dB seam on a file the engine repeats whole with no crossfade; sweeping the start
for the seam instead lands **0.02 dB**, at 24 grains/s against the reference slide's 17.

### `crunch_gran` is the one file that is not purely prompted

Four prompt revisions and 30 takes never produced a centroid under 4897 Hz, against a 4500 Hz
ceiling and a reference at 2705. The density was never the problem — the best takes carry 16–63
transients in 250–800 Hz, well past the 15 the slot needs. It is the high and air bands riding on
top that move the centroid, and no wording removed them.

So the two shipped files are **shelved before `make`**: `highshelf=f=2500:g=-18` and `g=-12`, then
the +4 dB bell at 250–800 Hz that §5 of `02-SFX-Generation-Prompts.md` already specifies for this
slot. `00-Design.md` wants `crunch_gran` "concentrated 300 Hz – 2 kHz", so shelving above 2.5 kHz
is the brief rather than a workaround — this is a cut of content that should not be there, not a
boost of content that is not there, which is what failed on the old scrape.

Shelved sources land in `takes/_shelf/` under their original filename, the way `split` uses
`takes/_split/`, so `make` archives them and the ledger provenance still reads.

### Still unfilled, by design

`grunt_impact` and `scream_big`. Record or licence them — generated screams are the worst-sounding
thing in this list, and this is the one slot where a real human recording beats generation
outright.

### Spares worth using

`takes/ledger.csv` rows with an empty `used_as` are takes that passed but lost the pick. Three
`short_gritty_scuff` takes pass as a `scrape_grain` — a slot `04-Reference-Analysis.md` §7 asks for
("sparse scrape-grain one-shots for the moments where a limb catches") that got dropped from the 29
in `00-Design.md`, and the only `SPEC` key with no entry in `core/src/SlotManifest.cpp`. Adding it
back is a design decision, not a tooling one.

---

## 4b. The library, and which file a slot actually plays

Since 2026-08-23 a slot's files are not decided by their filenames. `RagdollSounds_SFX.ini` says
which library files each slot plays, the testbench's SFX panel writes it, and the game reads it at
load. A slot the ini does not name falls back to the old `<slot>_NN.wav` scan, so **everything in §2
above is still true** — the 29 files still fill the 29 slots, they just do it through an assignment
that was seeded from their names rather than through the names themselves.

What that changes for this document: the "Files" column is now what the ini assigns, not what is on
disk. `tools/verify_pack.py` and `sfx.py eval` still measure `assets/sfx/`, which is the pack; the
library is a superset that also holds everything imported for auditioning and not yet chosen.

The importer measures each file the way `sfx.py measure()` does — duration, lead-in, decay to −20 dB,
centroid, band tilt, lo-mid transient count, and for anything that looks like a texture the seam,
the envelope steadiness and the grain rate. Two deliberate differences, both because a library file
has no slot yet:

- **Nothing is high-passed first.** `sfx.py` judges character after the slot's own high-pass because
  that is what ships; a library file is a candidate for every slot, so its tilt is its own and reads
  a little more bass-led than the same file's shipped tilt.
- **The FFT zero-pads to a power of two.** Band energies are still exact — Parseval does not care
  about trailing zeros — and the centroid is interpolated, which matters not at all for a number §4
  calls a loose sanity bound.

Deploy with `deploy-pack.ps1`, which now mirrors `assets/sfx/library/` into
`sounds/library/` alongside the pack. The ini is not copied: the testbench writes it straight into
`deployment_files/`.

### What an import fixes, and what it only tells you (2026-08-24)

The delivery rules in §6 and `Slots.md` §5 split cleanly in two, and since 2026-08-24 the importer
treats the halves differently. **The mechanical ones are repaired on the way in and nothing is
said about them** — there is no judgement in normalising a peak, so a badge for it is a badge that
only ever means "press the button that was going to be pressed anyway":

| Repaired silently | What it does |
|---|---|
| peak | normalised to −1.5 dBFS, for the ±3 semitone headroom rule. Files already inside −2.1…−0.9 are left alone, which is what keeps `imp_sub`'s −1.0 intact when the pack is adopted |
| DC | subtracted, measured *after* the trim rather than over the whole file |
| head silence | trimmed to 1 ms in front of the attack — §3.4's pre-roll, "always fixable" |
| trailing silence | trimmed to a 20 ms guard. Digital silence only; the room tail is a judgement and stays a warning |
| hard ending | `make`'s own 6 ms cos² fade, when the last samples are still over 3% of the peak |
| stereo | under 0.6 correlation the **left channel is kept** rather than the two summed — §3.1's fix, applied automatically |

All of it is idempotent, which is what makes `Repair` safe to press on a whole library: run over
the 29 shipped files plus `foley_cloth_01`, **all 30 come back byte-identical**. That is the
regression test for this half — anything that changes a file `verify_pack.py` passes is a bug in
the repair, not in the file.

What is left is what a badge is for, in two severities. Advisory ones name their own fix
(`sfx.py split`, `sfx.py make`, the `repair` button). The `dead` ones are the technical rule-outs
§7 ruled four cuts out on, and they say so: nothing recovers them.

| Badge | Means | Answer |
|---|---|---|
| `clipped` | the waveform is squared off — counted as flat tops at the file's own maximum, so it survives the normalise. **Not** a peak reading any more | re-source; gain does not put a wave back |
| `noise floor` | hiss inside 30 dB of the hero contact, `sfx.py`'s own gate | re-source or regenerate |
| `duplicate` | the same samples already in the library under another name, by content hash | delete one |
| `uncorrelated` | under 0.45 — two recordings sharing a file | re-source |
| `satellite` | a contact 20+ dB under the hero, §7's flam | `sfx.py split` |
| `contacts` | several contacts ≥46 ms apart. Suppressed over 15 lo-mid transients, where they are grains and not contacts | `sfx.py split` |
| `band-limited` | nothing above 16 kHz whatever the container claims — upsampled or lossy | re-source for a transient layer; fine for a body |
| `tail`, `seam`, `very long`, `lead-in`… | as before | as before |

---

## 5. Getting the pack into the testbench

Already done — `testbench/sounds/` holds a copy and the bank loads it. To refresh after a rebuild:

```
cp assets/sfx/*.wav testbench/sounds/
```

`testbench/sounds` is the default location: `main.cpp` resolves `--sounds` to
`<configs>/../sounds` when the flag is absent. To point somewhere else instead:

```
RagdollSoundsTestbench.exe --sounds <dir> --recordings Research/NewRecordings --configs testbench/configs
```

`SoundBank::Load` parses `<slotname>_<NN>.wav`, which is exactly what `sfx.py make` writes, and the
slot names in `core/src/SlotManifest.cpp` match the `SPEC` keys in `tools/sfx.py`. The trailing
number only orders the files — the variant index is position in the sorted set, so gaps are free.

Confirm a load from the log:

```
bank: imp_transient: 3/3 files
bank: imp_body: 3/3 files
bank: imp_sub: 2/2 files
bank: limb_tap: 4/4 files
bank: scrape_loop: 1/1 files
bank: foley_cloth: 1/1 files
bank: surf_wood: 2/2 files
```

Nothing should now report `nothing to play` except `grunt_impact` and `scream_big`. Anything
unfilled is silent rather than synthesised, so a partial pack is a thinner
testbench, not a broken one.

---

## 6. Delivery format

The pack is **mono / 48 kHz / 16-bit PCM**, changed from 44.1 kHz on 2026-08-22. The testbench
resamples and downmixes; the game is fussier, so the pack matches what the game wants.

- `sfx.py make --rate 48000` and `make_sub.py --rate 48000` build at the new rate. Both still
  default to 44100, which is what `02-SFX-Generation-Prompts.md` §1 documents — pass the flag.
  `make_sub.py` renders natively at 48 kHz anyway, so `--rate 48000` skips a downsample.
- The 11 files built before the change were **resampled**, not rebuilt. Re-rendering them from
  their takes is not faithful: several were cut or EQ'd with `make` overrides the ledger does not
  record, so a rebuild silently changes the file. `tools/to48k.py` resamples with soxr and
  renormalises afterwards, since soxr can overshoot the pre-resample peak by a few tenths of a dB.

`tools/verify_pack.py` checks the delivery rules that the slot spec does not:

| Rule | Why |
|---|---|
| mono / 48 kHz / 16-bit | the game is fussier than the testbench |
| no lead-in silence | the cue time is the attack; head silence becomes latency and puts the +15/+50/+65 ms layer offsets out |
| length inside the slot maximum | a baked room shows up as a file longer than its slot allows |
| level matched within a slot | the engine sets the level; variants differ in character, not loudness |
| headroom at ±3 semitones | everything is pitch-shifted at runtime and must not clip there |
| loop seam under 6 dB | loops are whole-file with no crossfade, so the seam is the asset's problem |

Two of its thresholds were wrong on the first run and are worth not re-deriving: a **lead-in**
measured against the peak flags `settle_rest`, which is specified to start softly — measure
leading silence against an absolute floor instead. And a **tail** threshold on the −40 dB time
flags `imp_sub`'s specified decay tail and the body layers' 145–155 ms house decay, all correct;
the real rule is the slot length, which `make` already enforces by truncating.


---

## 7. The 2026-08-23 Firefly batch

100 unique takes (102 files; two byte-identical duplicates). Triaged with
`tools/triage_batch.py`, which splits multi-contact takes, drops quiet satellites and
handles textures both as loops and as long one-shots. Output: **183 cuts** in
`takes/_split/`, listed in `takes/batch-manifest.csv`, with a per-cut verdict in
`takes/batch-triage.csv`.

**`takes/_import/` holds the 179 of those that are not technically broken**, each
prefixed with its best slot so the folder sorts itself — `imp_body__…`, `limb_tap__…`,
`unsorted__…` — with `_manifest.csv` alongside. That prefix is also what `sfx.py`
infers a slot from, so `eval` and `make` read the renamed files correctly. Nothing has
been promoted into `assets/sfx/`; these are candidates to audition.

### 63 cuts KEEP on the slot they were prompted for

| Slot | Cuts | Source |
|---|---|---|
| `limb_tap` | 33 | forearm tap on a hard floor, boot heel on stone, knee into packed dirt, series-of-taps, hand on packed earth |
| `imp_body` | 9 | wet sandbag on packed earth, sandbag of wet sand, weighted leather duffel, raw meat onto wood, `bone_crack_impact-composite` |
| `imp_transient` | 10 | knuckle crack on a wooden board, sharp dry slap of a leather glove, **fish slap** |
| `surf_wood` | 6 | hollow plank knock (5), creaking boards (1) |
| `surf_soft` | 2 | dull muffled thump into damp soil |
| `settle_rest` | 2 | loose fabric and a heavy limb slumping to rest |
| `scrape_loop` | 1 | heavy body violently sliding — **the long variant, not a loop window** |

`limb_tap` is the batch's real win: 33 cuts against a slot that currently ships 4, and
most of them came out of splitting — the tap prompts return four to nine contacts each, and
`series_of_light_limb_taps` yields nine keepers from two takes on its own.
The knuckle cracks are the best `imp_transient` material here (centroid 1772–2372 Hz,
tilt −15 to −25), notably darker than the shipped 3629–5792 Hz.

### Textures are worth cutting both ways

Every sliding / dragging / rustling take now yields two cuts: `-loop`, the 2–2.5 s window
with the smallest seam, and `-long`, the whole take trimmed and faded but **not**
seam-matched. `Looping` is a per-slot attribute of the assignment (`Slots.md` §6), so a
long drag is a perfectly good one-shot in a slot set `Looping = 0`, and a one-shot has no
seam to give it away.

That change is what found the only passing scrape in the batch.
`heavy_body_violently_sliding_and_scraping…variation2-long` — **9.9 s, tilt +18.6, centroid
3135 Hz — KEEPs as `scrape_loop`**, while all eleven 2 s loop windows failed. The window
sweep optimises the seam, and in doing so lands on a 2 s slice whose grain rate (53–73/s)
misses the 8–40 band; across the whole take the same material averages into it.

The long variants also reach slots the loop windows never could — the stone glides pass as
`surf_wood`, `surf_soft`, `scrape_grain`, `crunch_gran`, `gore_wet` and `settle_rest` when
taken whole, and `naked_body_sliding…dense_continuous-loop` passes as **`foley_cloth`**,
which is a better answer for that slot than any of the four cloak-rustle takes.


### `fish slap` measures as a contact, not a body

Four takes, prompted as the classic wet-slap reference. They measure **4923–7044 Hz, tilt
−26 to −31** — bright and fast, which is `imp_transient`, not `imp_body` and not `gore_wet`.
Three KEEP there. The `PROMPT_HEADS` entry says so explicitly rather than recording the
intent, because in this one case the prompt's family and the take's family disagree and the
measurement is the honest answer.

They are the brightest `imp_transient` candidates in the batch by a distance — the shipped
three sit at 3629–5792 Hz — so they read as a harder, thinner contact. Worth an A/B against
the knuckle cracks at the other end (1772–2372 Hz) before picking.

### A satellite is measured against the loudest contact now

`fish_slap_variation3(1)` exposed a hole: its file peak sits where no onset was detected, so
every contact in it read as 20+ dB down and the take was dropped whole. Satellite level is
now relative to the loudest **contact** rather than the file peak, which is what the concept
meant all along. Nothing else in the batch moved, and one take that was being discarded
silently now cuts.

### Two systematic failures, both about direction

**Stone prompts come back bass-led.** All four `surf_stone` prompts measured tilt **−0.4 to
+17.7 dB** against the slot's **≤ −2** ceiling. Not one is a stone take; three are good
*body* takes and are labelled as such. `surf_stone` gets nothing here, which is not
blocking — it is already 2/2 at 6751–8041 Hz.

**Wet and crunch prompts come back bright.** §3.6 again, and now `gore_wet` too. 38
wet/crunch cuts measure centroid **3733–8875 Hz** against ceilings of 4000 and 4500.

What is new, and worse: **the density has gone too.** §4 records that for the earlier batch
"the density was never the problem — the best takes carry 16–63 transients in 250–800 Hz".
These carry **1–16**, against the 15 `crunch_gran` needs. That matters for the fix: shelving
the top off, which rescued the two shipped `crunch_gran` files, only works when there is
something underneath. Here there is not. Re-prompt rather than re-EQ.

### Two gates a take can pass while being wrong

**29 of the 38 wet/crunch cuts report "would pass as `surf_stone`".** The slot's gate is
bright, fast-decaying and 100–160 ms, and a wet meat slap satisfies all three. `Slots.md` §2
asks for "a flat tight slap with no resonance and no tail… stone does not ring, it stops".

**Five stone-glide loop windows report "would pass as `air_whoosh`".** That slot wants
"**no transients at all** — featureless enough that looping is inaudible". A gritty glide is
nothing but transients.

`takes/_import/` refuses both pairings when it labels a file, and calls those cuts
`unsorted` instead. They still travel — the label is the only thing withheld.

### Quiet satellites are the norm, not an exception

**39 of 100 takes carry at least one contact 21–34 dB under the hero**, cleanly separated —
68 satellites in all, **60 after the hero and 8 before**. Mostly bright debris-settling
washes: the one on `dull_muffled_thump…variation2` sits at +262 ms, −29 dB, centroid 8594 Hz.

Left in, each lands on top of whatever the engine schedules next and reads as a flam — the
same class of problem §3's `WARN` catches at +55–75 ms, just later and quieter.
`triage_batch.py` drops them at a 20 dB threshold and records every one in the manifest's
`note` column, so nothing is discarded silently.

**37 of 100 takes split into more than one usable one-shot.**

### Ruled out for technical reasons — 4 cuts

Everything else travels, because a character failure is a judgement to make by ear.

| Cut | Why |
|---|---|
| `hard_flat_impact_of_human_body…variation4` (2 cuts) | stereo correlation **+0.68** — the channels comb-filter when summed |
| `hitmarker_sound…variation2` | stereo correlation **+0.61**, and the other variation has no contact above the floor at all |
| `twisting…gristle_variation2` | noise floor only **29 dB** down, under the 30 dB gate |

Plus two byte-identical duplicates dropped before cutting: `limb_tap2.wav` is a copy of the
damp-soil take rather than a limb tap, and `heavy_body_dragged…variation3(1)` copies
`variation3`.

### Still not solved

`crunch_gran` and `gore_wet` have no viable candidate in this batch, for the density reason
above. `foley_cloth`'s four cloak-rustle takes all fail on transients over the bed (8.0–16.1
dB against a ceiling of 6) — §3.5 once more, the model writing a gesture where the slot wants
a bed — and the best answer for that slot is the body-slide loop mentioned above.

### Tooling

`tools/triage_batch.py` is new and re-runnable: point it at the download folder and it
re-derives all of the above. `tools/sfx.py` gained twenty-three `PROMPT_HEADS` entries so this
batch's prompts infer a slot with no flags. Two of them, the stone heads, deliberately name
the *prompt's* slot rather than the take's — `make --slot` overrides it anyway, and recording
the prompt is what makes the failure legible.
