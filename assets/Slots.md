# Slot reference — what each sound is, and what it has to measure

The brief for every file in `assets/sfx/`. One row per slot: what fires it, what it should sound
like, how long it runs, and the numbers `tools/sfx.py eval` checks it against.

Sources, if a number here ever disagrees with one of them: slot names, families, variant counts and
lengths are `core/src/SlotManifest.cpp`; the measurable targets are the `SPEC` table at the top of
`tools/sfx.py`; the characters and the stack timing are `00-Design.md` §5 and §12. Every threshold
is calibrated against `Example/*.wav` — **a spec must never reject the references themselves.**

---

## 1. The impact stack

The most important thing on this page. **An impact is not a sample, it is a timed stack**, and the
biggest part of it arrives late. These layers fire together as one sound.

| Layer | Offset | Level in the stack | Role |
|---|---|---|---|
| `imp_transient` | **0 ms** | quietest, −5 to −18 dB | The contact itself |
| `surf_*` | +0–15 ms | −6 to −12 dB | What it hit |
| `imp_body` | +10–30 ms | −2 to −8 dB | Low-mid flesh and mass |
| **`imp_sub`** | **+55–75 ms** | **0 dB reference** | The pitched boom. The whole of the gnarl |

It starts bright and quiet and finishes dark, loud and short. That late sub is what makes a hit read
as *mass* rather than *contact*.

**"`imp_sub` is the loudest layer" is about the composite's band balance, not its mix gain.** Solved
against the reference band curve it sits **8 dB under `imp_transient`** — transient 0, body −3,
sub −8 — because it is almost pure sub-band energy while the transient spreads across mid and high.
Mixed genuinely loudest it buries everything and drives the stack to +29 dB tilt against the
references' +6.5 to +16.5. `tools/preview.py` renders the stack so this is checkable by ear.

Loudness across the intensity range comes from **layer balance, not tiers**: a light contact is
mostly transient with almost no sub, a heavy one is sub-dominant. Calibrate the whole range onto
about 35 dB.

---

## 2. What each slot is for, and what it should sound like

`Fires` is the count from the 12 recorded exports, where one was measured — it is a guide to how
much each file matters, not a spec. `—` means it was not counted, not that it never fires.

| Slot | Family | Files | Fires | What it is for | What it should sound like |
|---|---|---|---|---|---|
| `imp_transient` | impact | 3 | — | The contact instant of every impact | Bright, fast attack, no tail. A hard dry slap or knuckle crack. Must not compete with the body — it is the *quietest* layer |
| `imp_body` | impact | 3 | — | The mass of every impact; where the meat lives | Dense low-mid thud, smooth. Raw meat on wood, a wet sandbag, a weighted leather duffel. **If it has crackle in it, it is a `crunch_gran` take, not a body take** |
| `imp_sub` | impact | 2 | — | The boom under every impact. **Synthesised, never prompted** | Pitched sweep ~150 Hz → 30 Hz with a quiet tail holding the settled pitch. Saturated for 2nd/3rd harmonic so it survives a laptop speaker |
| `surf_soft` | surface | 2 | **27×** | The default surface skin — anything unresolved falls back here | The dull component of a body hitting packed earth or a rug. No ring, no snap. Like punching a heavy sofa cushion. Sits *under* the impact layers and must not compete |
| `surf_wood` | surface | 2 | 10× | Wooden floors, boards, planking | A hollow knock with air under it. Knuckles on a plank table, a boot heel on floorboards. Faint pitched resonance that dies almost immediately. Hollow, not boomy |
| `surf_stone` | surface | 2 | 0× | Flagstone, granite, worked stone | Hard and short. A flat tight slap with no resonance and no tail. Slapping concrete with a leather glove — stone does not ring, it stops |
| `limb_tap` | grain | 4 | — | Burst filler; turns a single hit into the 3–4 grains a real tumble has | Tiny dry contacts. A forearm on a floor, a knee in dirt, a boot heel clipping stone. Generate them **neutral** — the plugin pitch-scatters them heavily at runtime |
| `crunch_gran` | grain | 2 | 1× | The bone-break character. Rides on top of an impact | Dense granular crackle in the low-mid — **density, not a snap**. Many small crackles overlapping into one texture, never a single identifiable crack. It must **emerge rather than hit**: no attack of its own |
| `gore_wet` | grain | 2 | 0× | Obliterate tier only (>1400 u/s) | A wet squelch, heavy and viscous rather than splashy — mass moving, not liquid spraying. Sucking quality on the release. No bones, no crack |
| `head_impact` | accent | 2 | 1× | A skull hitting something hard. Rare, unmissable | Dull and heavy first, granular in the middle, a very slight ring at the end — **the ring is what makes it read as a head rather than a hip**. A watermelon on stone, minus the wet. Grim, not comedic |
| `settle_rest` | accent | 2 | **11×** | Closes the event — the body's last small shift as it comes to rest | A heavy limb flopping the final few inches and stopping. One soft dull thump with cloth around it. Like dropping a coat with a book in the pocket. **A full stop, not an event** |
| `scrape_loop` | loop | 1 | — | A body being dragged along a surface | Low-tilted grinding rumble with grit riding on it. **Not a hiss** — and not a pure rumble either: the reference slide is nearly flat, only the air band is really down |
| `foley_cloth` | loop | 1 | — | The continuous bed; clothing moving with the body | Steady gentle fabric friction, no impacts, no footsteps. Part of a bed that sits 30–36 dB under the hero hit |
| `air_whoosh` | loop | 1 | 44 loop ops | Airborne anticipation. Gain and pitch driven live as a parameter | Low airy movement, distinctly low-tilted. A thick blanket swung slowly past a mic. **No transients at all** — the file must be featureless enough that looping is inaudible. Not a designed sweep with a climax |
| `grunt_impact` | voice | **0** | — | Declared and unfilled by design | Record or licence. Fallback resolution skips it silently |
| `scream_big` | voice | **0** | — | Declared and unfilled by design | Record or licence — generated screams are the worst-sounding thing in this list |

`scrape_grain` — sparse one-shots for the moments where a limb catches — has a `SPEC` entry but **no
row in `SlotManifest.cpp`**. `04-Reference-Analysis.md` §7 asks for it and it was dropped from the
29. Three takes in the ledger already pass as one. Adding it back is a design decision.

---

## 3. Measurable targets

What `sfx.py eval` enforces. `As built` is what the shipped files actually measure, for comparison.

| Slot | Length | −20 dB in | Centroid | Tilt | HPF | Lo-mid transients | As built (centroid / −20 dB) |
|---|---|---|---|---|---|---|---|
| `imp_transient` | 60–120 ms | 5–90 ms | 1500–9000 Hz | **≤ −6 dB** (bright) | 400 Hz | 0–6 | 3629–5792 Hz / 8–18 ms |
| `imp_body` | 150–250 ms | 15–200 ms | 800–4200 Hz | **≥ +4 dB** (bass-led) | 120 Hz | 0–5 | 2166–3425 Hz / 16–32 ms |
| `imp_sub` | 250–400 ms | 15–120 ms | 20–400 Hz | — | **none** | — | 156–184 Hz / 48 ms |
| `surf_soft` | 150–250 ms | 15–200 ms | 300–3200 Hz | **≥ +3 dB** | 120 Hz | — | 674–2475 Hz / 16–26 ms |
| `surf_wood` | 120–200 ms | 20–160 ms | 400–3000 Hz | — | 120 Hz | — | 830–1348 Hz / 40–78 ms |
| `surf_stone` | 100–160 ms | 5–90 ms | 1200–9000 Hz | **≤ −2 dB** (bright) | 120 Hz | — | 6751–8041 Hz / 8–12 ms |
| `limb_tap` | 40–100 ms | 3–60 ms | 600–9000 Hz | — | 200 Hz | 0–4 | 904–3459 Hz / 12–44 ms |
| `crunch_gran` | 250–400 ms | 30–400 ms | 500–4500 Hz | — | 120 Hz | **15–99** | 3752–4154 Hz / 34–66 ms |
| `gore_wet` | 200–400 ms | 20–350 ms | 400–4000 Hz | — | 120 Hz | — | 1645–1666 Hz / 28–58 ms |
| `head_impact` | 300–500 ms | 20–300 ms | 400–3500 Hz | **≥ +2 dB** | 120 Hz | 4–14 | 1100–2935 Hz / 34–70 ms |
| `settle_rest` | 200–400 ms | 20–400 ms | 300–3200 Hz | — | 120 Hz | — | 2469–2629 Hz / 28–48 ms |
| `scrape_grain` | 150–500 ms | 40–400 ms | 300–6000 Hz | — | 120 Hz | 6–40 | not built |
| `grunt_impact` | 300–600 ms | — | — | — | — | — | not built |
| `scream_big` | 800–1500 ms | — | — | — | — | — | not built |

Peak is **−1.5 dBFS** everywhere except `imp_sub`, which is **−1.0**. `settle_rest` additionally
requires a **soft attack** (≥10 ms rise) — it is the only impact-family file that must *not* have an
instant transient.

`imp_transient` also has an `hf_loss` floor of **8 dB**: how much peak level a take gives up to its
own 400 Hz high-pass. A take that loses almost nothing to it was bass-led all along.

### Loops

Judged as a sustained texture rather than a one-shot, so there is no attack or decay to measure.

| Slot | Length | Centroid | Tilt | Grains/s | Envelope steadiness | Other | As built |
|---|---|---|---|---|---|---|---|
| `scrape_loop` | 1.5–3 s | — | **+5 to +19 dB** | **8–40** | 2.5–8.0 dB | no high-pass | 3386 Hz, 24 grains/s, **0.02 dB seam** |
| `foley_cloth` | 1.5–3 s | — | — | — | 0.5–5.0 dB | high-pass **120 Hz**; no peak >6 dB over the bed | 5500 Hz, 1.2 dB seam |
| `air_whoosh` | 1–2 s | 50–2000 Hz | — | — | 0.5–6.0 dB | no high-pass | 218 Hz, 0.6 dB seam |

Loops are played **whole-file with no crossfade**, so the seam is the asset's problem: generate
5–10 s, find a stable window, and fold the tail back over the head. Any audible pulse at the seam
becomes a rhythm in game.

---

## 4. How to read tilt, and why centroid is the weak one

**Band tilt** is `(sub + low)/2 − (high + air)/2` in dB. Positive is bass-led, negative is bright.
It is the honest discriminator and the one that catches the most: a wet meat slap is inherently
mid-forward and cannot be a body layer; a gritty scuff is inherently multi-grain and cannot be a
transient.

**Spectral centroid barely works on its own.** The four reference hero hits measure 1950–3306 Hz
centroid while being clearly bass-led, because there are simply more FFT bins up top. The centroid
limits in `SPEC` are loose sanity bounds only — the trap `04-Reference-Analysis.md` §7 flags for the
scrape. Check tilt, and check how much level a file loses to its own high-pass.

Character is always measured **after the slot's high-pass**, because that is what ships.

---

## 5. Format, and the rules the spec does not cover

Every file: **mono / 48 kHz / 16-bit PCM**, `<slot>_NN.wav`. The trailing number only orders the
files — the variant index is position in the sorted set, so gaps are free.

Author everything **dry, punchy and pre-limited**. All dynamics come from runtime gain.

| Rule | Why |
|---|---|
| No lead-in silence | The cue time *is* the attack. Head silence becomes latency and puts the +15/+50/+65 ms layer offsets out |
| No baked reverb tail | The game applies the cell's acoustic space. A baked tail double-counts and turns overlaps to mud — it sounds like a cave inside a cave |
| Level matched within a slot | The engine sets the level; variants differ in **character, not loudness** |
| Survives ±3 semitones | Everything is pitch-shifted at runtime and must not clip or fall apart there |
| Loops seamless | Whole-file looping, no crossfade |

`tools/verify_pack.py` checks all five. `tools/sfx.py eval` checks §3. A file needs to pass both.

---

## 6. Which file a slot plays

Nothing above says *which* file fills a slot, and since 2026-08-23 that is no longer decided by the
filename. `RagdollSounds_SFX.ini` carries one section per slot:

```ini
[imp_body]
Sfx = wet-sack-heavy.wav, leather-duffel-2.wav, flour-sack.wav
Looping = 0
```

The names are files in `sounds/library/`, the order is the variant index a cue carries, and the
engine picks between them with a shuffle bag — which is exactly what `imp_body_01/02/03` was doing
before, now said out loud. **A slot with no line falls back to scanning for `<slot>_NN.wav`**, so an
install with no ini sounds precisely as it did before there was one.

`Looping` is the one slot attribute that is not in the table above, because it is a property of what
got assigned as much as of the slot. A looping slot is played whole and repeated, is never judged for
being too long, and has its seam checked instead of its attack. It defaults to the `loop` family in
§2 and is overridable per slot: a sliding or wind-like sound assigned somewhere else wants the same
treatment.

Which is why **a texture take is worth cutting both ways** — as a seamless 2–3 s loop window, and as
the whole take trimmed but not seam-matched. The long one has no seam to give it away and can fill a
one-shot slot with `Looping = 0`, and the two do not measure the same: the window sweep optimises the
seam, which pins it to one 2 s slice of the material, while the same take averaged whole can land
inside a grain or tilt band the slice misses. The only `scrape_loop` to pass in the 2026-08-23 batch
was a 9.9 s long cut whose own loop window failed. `tools/triage_batch.py` emits both.

Everything in the library carries a `<file>.meta.ini` sidecar with what the importer measured, in
these same units — so a badge in the testbench's library window can be read straight against §3.

Anything unfilled falls through to a procedural stand-in rather than going silent, so a partial pack
is a quieter mod, not a broken one — which is why `grunt_impact` and `scream_big` can sit declared
and empty indefinitely.


---

## 7. Two biases to expect from the generator

Measured across the 102-file batch of 2026-08-23 and the 30 takes before it. Both are
about *direction*, and both survive re-prompting, so budget takes accordingly.
`03-Asset-Status.md` §7 has the numbers.

| Ask for | You get | Consequence |
|---|---|---|
| Anything **hard, dry and bright** — stone, flagstone, a flat tight slap | Something **bass-led**. All four stone prompts measured tilt −0.4 to +17.7 against `surf_stone`'s ≤ −2 | `surf_stone` is not promptable. The takes are usable as `imp_body` / `surf_soft` |
| Anything **dull, dense and low** — bone crunch, wet squelch, offal | Something **bright and sparse**. Centroid 3733–8875 Hz against 4000/4500 ceilings, and 1–16 lo-mid transients against the 15 `crunch_gran` needs | `crunch_gran` and `gore_wet` need shelving *and* enough density to shelve down to. Check density first |

The reason the second one is worth checking before reaching for EQ: shelving above 2.5 kHz
is what rescued the two shipped `crunch_gran` files, but that only works on a take that
already carries the low-mid density. A take that is bright *and* sparse has nothing under
the shelf, and comes out quiet rather than dull.

### The `surf_stone` numbers trap

`surf_stone`'s gate is bright, decays fast and runs 100–160 ms. **A wet meat slap
satisfies all three.** 29 of 38 wet cuts in that batch report "would pass as `surf_stone`"
and not one of them is a flagstone. §2 of this page is the authority — "no resonance and
no tail… stone does not ring, it stops" — and `sfx.py --suggest` is a shortlist to
audition, never a verdict.

### Quiet satellites

Generators bolt a quiet second event onto an otherwise clean take often enough to plan
for: **39 of 100 takes**, 68 events, 21–34 dB under the hero, 60 of them after it. They are
usually a bright debris wash rather than a contact.

Left in, one lands on top of whatever the engine schedules next and reads as a flam — the
same failure the `WARN` in §3 of `03-Asset-Status.md` catches at +55–75 ms, just later and
quieter. `tools/triage_batch.py` cuts them out at a 20 dB threshold and records every one
it dropped, so the decision stays reviewable.
