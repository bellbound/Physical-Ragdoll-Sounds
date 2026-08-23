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
| `imp_sub` | 2/2 | **synthesised**, `tools/make_sub.py` | -20 dB in 48 ms, centroid 156 / 184 Hz |
| `limb_tap` | 4/4 | boot scuff x3 + forearm tap, all **split** from multi-contact takes | centroid 904 -> 3459 Hz |
| `foley_cloth` | 1/1 | woollen cloak rustle | seamless 3 s, 1.2 dB seam |
| `scrape_loop` | 1/1 | limp body on rough stone slabs | 24 grains/s, tilt in band, **0.02 dB seam** |
| `air_whoosh` | 1/1 | low air movement, generated as a loop | centroid 218 Hz, 0.6 dB seam |
| `settle_rest` | 2/2 | slumping fabric + limb, body settling | -20 dB in 28 / 48 ms, soft attack |
| `surf_soft` | 2/2 | damp soil thump, weight into grass and earth | centroid 674 / 2475 Hz |
| `surf_wood` | 2/2 | hollow plank knock, old creaking boards | centroid 830 / 1348 Hz |
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
`01-Reference-Analysis.md` §7 flags for the scrape. Centroid limits in `SPEC` are loose sanity
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
`short_gritty_scuff` takes pass as a `scrape_grain` — a slot `01-Reference-Analysis.md` §7 asks for
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

Nothing should now report `procedural` except `grunt_impact` and `scream_big`. Anything unfilled
falls through to the procedural stand-in rather than going silent, so a partial pack is a quieter
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
