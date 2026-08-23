# 01 — What the Skate 3 references actually do

Measured from the four clips in `Example/`. Nothing here is copied audio — these are *numbers*:
layer timings, band balances, envelope shapes, event rates. They become parameters and asset
briefs, and we generate our own sounds against them.

Method: 44.1 kHz, mono-summed, zero-phase band splits at 20/80/250/800/2500/8000/20000 Hz, 2 ms
RMS envelopes, spectral-flux onset detection. Reverb and pre-impact air noise ignored as
instructed, except where their *level* is the finding.

---

## 1. The finding that changes the design: impacts are three layers arriving at different times

In every hero hit in all four clips, the frequency bands do not arrive together. They arrive in
a fixed order, with a fixed level order, and a fixed decay order — and all three orders are
different from each other.

| Layer | Arrives | Level | Decays to −20 dB in |
|---|---|---|---|
| Transient (2.5 kHz+) | **0 ms** | −5 to −18 dB | 88–212 ms (slowest) |
| Bite (800–2500 Hz) | 0 to +6 ms | −1.6 to −7.5 dB | 52–212 ms |
| Body (250–800 Hz) | **+8 to +34 ms** | 0 to −10 dB | 2–170 ms |
| Weight (80–250 Hz) | **+46 to +100 ms** | −0.5 to −4.5 dB | 10–58 ms |
| **Sub (20–80 Hz)** | **+64 to +74 ms** | **loudest element, 0 dB in 5 of 7 hits** | 16–52 ms (fastest) |

So: the sound starts bright and quiet, and finishes dark, loud and short. The biggest, loudest
part of a Skate 3 impact arrives **two thirds of a frame after the impact itself** and is over
almost immediately.

**This is not a measurement artefact.** A synthetic click and a 20 ms noise burst pushed through
the identical pipeline show at most +10 ms of apparent sub lag. The +65 ms is real content.

It is also not physical — nothing about a body hitting the ground delays its low frequencies by
65 ms. It is a deliberate sweetener, and it is the single biggest reason these clips feel gnarly
rather than clicky. **My design review missed it entirely**, and it is the most important thing
to add.

### The sub layer is a descending pitched boom

Tracking the dominant frequency below 200 Hz through each hero hit:

| | +20 ms | +50 ms | +80 ms | +120 ms | +180 ms |
|---|---|---|---|---|---|
| head crunch | 183 Hz | 65 Hz | 54 Hz | 54 Hz | 22 Hz |
| spine break | 162 Hz | 22 Hz | 65 Hz | 65 Hz | 32 Hz |
| heavy body hit | 108 Hz | 43 Hz | 54 Hz | 54 Hz | 43 Hz |
| loudest tumble hit | 54 Hz | 54 Hz | 32 Hz | 22 Hz | 22 Hz |

All four sweep downward, from roughly 110–185 Hz into the 40–65 Hz range within 50–80 ms, and
into the 20–30 Hz floor by 180 ms. Tonality measures 8–15 dB peak-above-mean in the sub band, so
this is a pitched element, not filtered noise.

That is an 808-style impact boom. It is the standard trick in modern action-game impact design,
and it is exactly what makes a hit read as *mass* rather than as *contact*.

**We can do this in Skyrim.** Bake the sweep into one short file, then scale the whole thing with
the engine's live pitch control per event — a heavier impact starts lower and reads bigger, for
free, from one asset.

---

## 2. Nothing fires faster than about 46 ms apart

Measured gaps between detected onsets, across all four clips:

| Clip | Onsets | Min gap | Median gap |
|---|---|---|---|
| bone break / head impact | 3 | 116 ms | 223 ms |
| foot then spine break | 14 | **46 ms** | 58 ms |
| heavy impact, tumble, slide | 21 | **46 ms** | 93 ms |
| tumbling down hill | 16 | **46 ms** | 70 ms |

The floor is 46 ms in three independent clips. That is not a coincidence — it is about where
human hearing stops resolving two impacts as separate events. Below it, extra onsets do not add
detail, they add mud.

**This is a hard global rate limit, not a per-limb one.** My review proposed a per-limb
refractory; the references cap the *total* onset rate regardless of which body part is involved.

---

## 3. The rhythm is bursts and gaps, and the reduction ratio is about 10:1

The gaps are strongly bimodal. Inside a burst: 46–104 ms. Between bursts: 313–894 ms.

`tumbling-down-hill` in full — gaps in ms:

```
46  58  104 | 726 | 58  52  75 | 453 | 75 | 511 | 64  70  58  46 | 313
└── burst ──┘     └── burst ──┘      └─┘      └──── burst ──────┘
```

**A three-second tumble down a hill is four audible events.** Each is a tight burst of three to
four grains at 46–104 ms spacing, and between them there is 300–730 ms of near-nothing.

Put that against the research: a Skyrim knockdown produces 30–60 collisions, of which the docs
call 15–30 "worth hearing", over a 1.4–2.8 s window. Skate would render the same event as **four
to six audible moments.**

So the job is not to decide which of thirty contacts to play. It is to throw away roughly nine
out of ten of even the ones that pass a sensible loudness bar, and to spend what is left on a
few tight bursts with real silence between them. Suppression is not a safety valve on this
design — it *is* the design.

---

## 4. Hero dominance is a small group of peers, then a cliff

Peak level of every onset, relative to the loudest in its clip:

| Clip | Loudest | 2nd | Median onset | Quietest |
|---|---|---|---|---|
| bone break / head | −1.7 dB | −9.9 | −9.9 | −12.9 |
| foot then spine | −4.6 dB | **±0.0** | −2.0 | −15.4 |
| heavy impact + slide | −4.2 dB | −0.5 | −9.3 | −17.5 |
| tumbling down hill | −4.8 dB | −0.8 | −4.6 | −14.8 |

The top one to three events sit within a decibel of each other, then everything else drops 9–17
dB. My review said "one hero hit that dominates" — the references say **a small group of peers,
then a cliff.** A big fall gets two or three co-equal big moments, not one.

That is a better fit for our data anyway: a faceplant genuinely has a knee, a chest and a head
arriving within a couple of hundred milliseconds, and flattening that to one sound would lose
the thing.

---

## 5. Envelope and level budget

**Decay is remarkably consistent.** The loudest event in every one of the four clips falls 20 dB
in **145–155 ms**, and 40 dB in 320–610 ms. So the perceptual length of an impact is about
150 ms, with a tail that is gone inside 600 ms.

**Crest factor around the peak is 8.8–13.8 dB** and peaks sit at −1.7 to −6.6 dBFS. These assets
are compressed, limited and loud. Dynamics live in the mix, not in the file.

**The usable range is about 35 dB.** Loudest onset to quietest onset is 13–17 dB; the continuous
bed sits **30–36 dB under the hero hit** in all four clips. That is the whole span, and it is
the calibration target for mapping our 0.3–14 m/s onto gain — not the 60 dB a naive log curve
would produce.

**Stereo correlation is 0.95–0.97** on three clips (0.69 on the head impact, which has the most
reverb). These are essentially mono point sources with reverb width. Spatial collapse onto one
point during a hero moment is confirmed as what the references do.

---

## 6. A "bone break" is granular density, not a crack sample

Counting transient peaks inside one event window:

| Event | Peaks in 250–800 Hz | Peaks in 2.5–8 kHz |
|---|---|---|
| spine break | **24** | 18 |
| foot break | 11 | 8 |
| head crunch | 7 | 4 |
| plain heavy body hit | **2** | 10 |
| plain tumble hit | 2–3 | 7–11 |

A plain body impact is smooth in the low-mid — two or three peaks across 350 ms. A bone break is
**ten times denser in that band**: twenty-four separate little transients packed into 420 ms.

So the difference between "thud" and "crunch" is not a snap sample layered on top. It is
*granularity in the 250–800 Hz band*. My review specified `bone_crack` as a 100–200 ms dry snap;
that is the wrong shape and would sound like a stick breaking rather than a body breaking. It
needs to be a granular crunch bed of 250–400 ms.

---

## 7. The slide is a low rumble, not a hiss

From the 2.1 s of stone slide in `heavy-impact-tumble-and-slide`:

- Band levels: sub −37, low −40, low-mid −41, mid −41, high −44, air −56 dB. **Tilted downward,
  not upward.** (A naive spectral centroid reads ~3 kHz and is misleading — there are simply far
  more bins up there.)
- 65 grain peaks per second — one every 15 ms, far denser than any contact stream could drive.
- Envelope standard deviation 4.7 dB against 8.3 dB for the tumble section: much steadier, but
  still clearly granular.
- Level sits 15–25 dB below the impact section.

So a body sliding on stone is a **low, broadband grinding rumble with grain riding on it**, about
20 dB under the impacts. My review described the scrape asset as "broadband friction" and implied
brightness — correct that to a low-tilted grind.

The 65 grains/second confirms this has to be a loop. It also suggests a second, sparse layer of
individual scrape-grain one-shots on top for the moments where a limb catches — which is what
gives a slide its irregularity.

---

## 8. Weak signal: the bed dips before a big hit

In three of the four clips the continuous bed falls 8–15 dB in the 50–100 ms immediately before
a hero impact, then the hit lands. The fourth rises instead.

This may be a deliberate pre-duck — it is a known trick for making an impact land harder — or it
may just be what happens when a body is briefly airborne. Three out of four is not enough to
build on, but it is cheap to try: our airborne detection already knows an impact is imminent, so
ducking the bed for a few frames before the landing is nearly free. **Worth a config toggle,
tuned by ear in the testbench, not a load-bearing part of the design.**

---

## 9. What this changes in the plan

Validated as written:

- A continuous bed under everything, sitting far below the impacts — confirmed at 30–36 dB down
- Layering rather than sample selection — confirmed, these are multi-layer composites throughout
- Spatial collapse onto one point for a hero moment — confirmed by the mono correlation
- Loops for sustained contact — confirmed, the grain rate is ten times what contacts could drive
- Suppression as the main event — confirmed and then some, at roughly 10:1

Changed:

1. **Add a pitched descending sub layer, fired ~65 ms after the transient.** The biggest single
   omission. It is the weight, it is the gnarl, and it is one file.
2. **Layer offsets are structured, not random jitter.** Transient at 0, body at +15, weight at
   +50, sub at +65, each with a few milliseconds of scatter. My review proposed 10–40 ms of pure
   randomness, which would smear the shape rather than build it.
3. **The rate limit is global and about 46 ms**, not per-limb.
4. **Target burst-and-gap rhythm**: three to five grains inside 200–400 ms, then 300 ms or more
   of near-silence. The arbitrator should be choosing *bursts*, not individual sounds.
5. **Budget four to six audible moments per knockdown**, not fifteen to thirty.
6. **Hero moments come in ones to threes**, within a decibel of each other, with a 10 dB cliff
   below them.
7. **Bone break is a 250–400 ms granular low-mid crunch**, not a short snap.
8. **The scrape is low-tilted**, not bright.
9. **Calibrate the whole intensity curve onto about 35 dB.**
10. **Author assets dry, punchy and pre-limited**, peaking near full scale — all dynamics from
    runtime gain.

---

## 10. Revised asset list

Roles now reflect the layered timing model. Offsets are from the impact frame.

### Impact composite — every audible impact is built from these

| Slot | Var | Length | Offset | Character |
|---|---|---|---|---|
| `imp_transient` | 3 | 60–120 ms | 0 ms | Bright, fast attack. The contact itself. Quietest layer. |
| `imp_body` | 3 | 150–250 ms | +10–30 ms | Low-mid flesh and mass. The main body of the sound. |
| **`imp_sub`** | 2 | 250–400 ms | **+55–75 ms** | **Pitched boom sweeping ~150 Hz → 30 Hz. Loudest layer. The single most important file in the mod.** |

### Surface skins — short, layered on the composite

| Slot | Var | Length | Offset |
|---|---|---|---|
| `surf_wood` | 2 | 120–200 ms | 0–15 ms — hollow knock |
| `surf_stone` | 2 | 100–160 ms | 0–15 ms — hard, short |
| `surf_soft` | 2 | 150–250 ms | 0–15 ms — dull; the default for anything unresolved |

### Grains and texture

| Slot | Var | Length | Notes |
|---|---|---|---|
| `limb_tap` | 4 | 40–100 ms | The burst filler. Quiet, dry, heavily pitch-scattered |
| `crunch_gran` | 2 | 250–400 ms | Dense granular crackle in the low-mid. The bone-break character |

### Loops

| Slot | Var | Length | Notes |
|---|---|---|---|
| `scrape_loop` | 1 | 1.5–3 s | Low-tilted grinding rumble with grain. Not a hiss |
| `foley_cloth` | 1 | 1.5–3 s | Cloth rustle, no transients |
| `air_whoosh` | 1 | 1–2 s | Low airy movement |

### Accents

| Slot | Var | Length | Notes |
|---|---|---|---|
| `head_impact` | 2 | 300–500 ms | Dull skull thud with a granular edge and a slight ring |
| `settle_rest` | 2 | 200–400 ms | Soft final flop. Closes the event |

### Voice — optional, highest impact per file

| Slot | Var | Length |
|---|---|---|
| `grunt_impact` | 2 | 300–600 ms |
| `scream_big` | 1 | 800–1500 ms |

**Full set: 30 files. First taste: 13** — `imp_transient` ×3, `imp_body` ×3, **`imp_sub` ×2**,
`limb_tap` ×3, `scrape_loop`, `foley_cloth`. Build the sub layer in the first pass; without it
none of this will feel like the references.
