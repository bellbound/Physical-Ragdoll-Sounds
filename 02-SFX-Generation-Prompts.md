# 02 — Generation prompts for the 29 assets

One prompt per file, written against the measurements in `04-Reference-Analysis.md` and the asset
list in `00-Design.md`. These are for a text-to-SFX model (ElevenLabs Sound Effects, Stable Audio,
AudioGen, ElevenLabs-style APIs). Nothing here asks the model to imitate Skate 3 audio — the
prompts describe the *shape* the analysis measured.

**Read section 1 before generating anything.** The prompts alone will not produce shippable files;
the post-pass is half the work, and two slots should not be prompted at all.

---

## 1. Ground rules for every generation

### What the model is and isn't for

These models are good at **texture and material** — flesh, wood, grit, cloth, crackle. They are bad
at **precise envelopes, sub-bass, and length under ~300 ms**. So:

- Prompt for the *material and gesture*, then cut the envelope yourself in the editor.
- Never trust the model to give you the bottom octave. Every impact file gets its sub from
  `imp_sub`, which is **synthesised, not prompted** (see §4).
- Most APIs have a minimum duration around 0.5–1 s. Generate long, keep the first 60–400 ms.

### Fixed output format

| | |
|---|---|
| Sample rate | **48 kHz** since 2026-08-22 — `make --rate 48000`. The tools still default to 44100; see `03-Asset-Status.md` §6. Request `pcm_44100`/`pcm_48000` from the API rather than relabelling an mp3 |
| Channels | **Mono.** The references measure 0.95–0.97 correlation — they are point sources |
| Bit depth | 16-bit PCM wav for the game, 24/32-bit float while working |
| Naming | `<slot>_01.wav`, `<slot>_02.wav`, … e.g. `imp_transient_01.wav` |

### The post-pass, applied to every generated file

1. **Mono-sum** and remove DC offset.
2. **Trim the head to the transient.** Cut everything before the attack, land the first sample on
   a zero crossing. Generated audio nearly always has 50–200 ms of silence or room tone in front —
   that silence *is* a timing bug once the plugin schedules layers at +15/+50/+65 ms.
3. **Strip the tail.** The cell's reverb is applied by the game; a baked tail fights it and turns
   overlaps to mud. Fade out to silence inside the slot's stated length.
4. **Kill the reverb the model added.** Prompts below all carry a dry/close-mic instruction, but
   models leak room anyway. If it still sounds roomy, regenerate rather than de-reverb.
5. **High-pass at 120 Hz on every impact layer except `imp_sub`.** Generated low end is muddy and
   uncorrelated with the sub sweep; the sub layer owns the bottom exclusively.
6. **Limit and normalise to −1.5 dBFS peak.** The references are compressed, limited and loud; all
   dynamics come from runtime gain. The analysis's 8.8–13.8 dB crest factor is a property of the
   **assembled composite**, not of one layer — a short transient measures 18–21 dB on its own and
   that is correct. Check crest on the stack, never on a single file.
7. **Check the 20 dB decay time** against the slot spec. The reference hero hits fall 20 dB in
   145–155 ms — that number is the house style, and layers should not fight it.

### Negative prompt (use on every generation that supports one)

```
music, tonal instrument, reverb, room, hall, echo, distant, ambience, birds, wind,
speech, voice, screaming, cartoon, comedic, whoosh sweetener, riser, cinematic braam,
fade in, silence, stereo width
```

### Prompt style that works

Comma-separated concrete descriptors. Material first, action second, mic third, character last.
No narrative, no "epic", no emotion words. Every prompt below ends with the same dry/close tail —
keep it, it does more work than any adjective in front of it.

---

## 2. Impact composite — the core stack

These three fire as one sound, at 0 / +10–30 / +55–75 ms. Generate them to *sit together*: the
transient is the quietest and brightest, the body carries the mass, the sub is the loudest and
arrives late.

### `imp_transient` ×3 — 60–120 ms, offset 0 ms

Bright, fast attack, the contact itself. High-pass at 400 Hz in the post-pass; this layer must not
compete with the body.

| File | Prompt |
|---|---|
| `imp_transient_01` | `sharp dry slap of a heavy leather glove onto packed dirt, single hit, instant attack, no tail, close mic, dry studio foley, no reverb` |
| `imp_transient_02` | `hard knuckle crack against a wooden board, one short percussive tick, brittle top end, close mic, dry studio foley, no reverb` |
| `imp_transient_03` | `one hard dry knock of a leather boot heel on flat stone, single sharp contact, no scrape, no grit, no second hit, close mic, dry studio foley, no reverb` |

*Post:* trim to 60–120 ms, HPF 400 Hz, 20 dB decay inside 90 ms. Author these **6–10 dB quieter**
than the body layer before normalising the set as a group — the analysis has the transient as the
quietest element (−5 to −18 dB).

### `imp_body` ×3 — 150–250 ms, offset +10–30 ms

Low-mid flesh and mass. This is the main body of the sound and where most of the "meat" lives.

| File | Prompt |
|---|---|
| `imp_body_01` | `heavy side of raw meat dropped flat onto a wooden floor, dense wet thud, soft low mid weight, single impact, close mic, dry studio foley, no reverb` |
| `imp_body_02` | `sandbag full of wet sand slammed onto packed earth, dull heavy thump, brief low mid body, single hit, close mic, dry studio foley, no reverb` |
| `imp_body_03` | `weighted leather duffel bag dropped hard onto a stone floor, muffled dense thud with a faint creak of leather, single impact, close mic, dry studio foley, no reverb` |

*Post:* trim to 150–250 ms, HPF 120 Hz, tilt the band 250–800 Hz up ~3 dB, 20 dB decay in
145–155 ms. Keep it **smooth** — the analysis counts only 2–3 low-mid transient peaks in a plain
body hit. If a take has crackle in it, that take is a `crunch_gran` candidate, not a body layer.

---

## 3. `imp_sub` ×2 — do not prompt this

**Synthesise it.** This is the loudest layer, the one the analysis calls the single most important
file in the mod, and no text-to-SFX model will give you a clean pitched sweep into 30 Hz. Ten
minutes in any DAW or a twenty-line Python script beats any prompt.

Recipe, straight off the measured sweeps (§1 of the analysis):

```
sine oscillator
  pitch: 150 Hz → 45 Hz over the first 60 ms, exponential
         45 Hz → 28 Hz over the next 200 ms
  amp:   0 → full in 3 ms, then exponential decay, −20 dB at 40 ms, silent by 300 ms
  drive: saturation for 2nd and 3rd harmonic (keeps it audible on small speakers),
         then a gentle low-pass at 700 Hz -- NOT tight to the fundamental. Filtered
         down to a near-pure sine it has nothing above 300 Hz and cannot sit inside
         a broadband hit, only underneath one
  tail:  a quiet slow-decaying tail under the punch, holding the settled pitch. Without
         it the sweep smears and measures 4-7 dB of tonality against the references' 8-14
  click: 2 ms of the sine's own attack left un-faded — do not add a separate click,
         imp_transient is the click
length:  300 ms (var 01), 380 ms (var 02)
```

| File | Variant |
|---|---|
| `imp_sub_01` | Start 150 Hz. The default boom |
| `imp_sub_02` | Start 110 Hz, 380 ms, slightly slower sweep. Reads bigger; used for heavy hits and as pitch-scale headroom |

**"Loudest layer" is about the composite's band balance, not this file's mix gain.** Solved against
the reference band curve, `imp_sub` sits **8 dB under `imp_transient`** in the stack (transient 0,
body −3, sub −8). It is almost pure sub-band energy while the transient spreads across mid and
high, so it has to be the *quietest* layer for the sub band to come out on top. Mixing it loudest
buries everything else and drives the composite to +29 dB of tilt against the references' +6.5 to
+16.5. `tools/preview.py` renders the stack so this can be checked by ear.

*Post:* no high-pass. Normalise to −1.0 dBFS — author it hot like everything else. Verify the
descent by ear on a sub-capable monitor **and** on a laptop speaker; if it disappears entirely on
the laptop, add more saturation, not more level.

If you must generate it: `deep sub bass drop, descending pitched boom, 808 style, no music, no
beat, single hit, dry` — then expect to fix the pitch curve manually anyway.

---

## 4. Surface skins — 0–15 ms offset, layered on the composite

Short, characterful, and **quiet**. They colour the composite; they do not carry it.

### `surf_wood` ×3 — 120–200 ms, hollow knock

| File | Prompt |
|---|---|
| `surf_wood_01` | `single hollow knock on a thick wooden plank floor, dull woody resonance, short, close mic, dry studio foley, no reverb` |
| `surf_wood_02` | `heavy thud on old creaking wooden boards, hollow box resonance with a faint plank rattle, single hit, close mic, dry studio foley, no reverb` |
| `surf_wood_03` | `dull impact on damp rotten timber lying on soil, soft woody knock, dead and thick with a faint fibrous crackle, single hit, close mic, dry studio foley, no reverb` |

`surf_wood_03` is the **damped** wood, asked for to sit between `surf_wood`'s hollow knock and
`surf_soft`'s dead thump. What makes the other two read as wood is the *cavity* — "plank floor",
"boards", "box resonance" all say there is air behind the surface. Take the cavity away and keep
the species and you land in the middle, which is why the prompt says timber on soil. Add
`hollow box resonance, drum, cavity, plank rattle, creak` to the negative prompt for it, and avoid
`soft` and `muffled` outright: both pull cloth and pillow into the render and lose the woody knock
that is the point of the layer.

### `surf_stone` ×2 — 100–160 ms, hard and short

| File | Prompt |
|---|---|
| `surf_stone_01` | `hard flat slap against cold stone flagstone, tight bright contact, almost no decay, close mic, dry studio foley, no reverb` |
| `surf_stone_02` | `heavy object striking rough granite, sharp stony crack with a light spray of grit, single hit, close mic, dry studio foley, no reverb` |

### `surf_soft` ×2 — 150–250 ms, dull. The default for anything unresolved

| File | Prompt |
|---|---|
| `surf_soft_01` | `dull muffled thump into damp soil, soft dead impact, no ring, close mic, dry studio foley, no reverb` |
| `surf_soft_02` | `heavy weight landing on thick grass and loose earth, soft dull impact with a faint rustle, close mic, dry studio foley, no reverb` |

*Post for all six:* trim to spec, HPF 150 Hz, normalise to −1.5 dBFS then **pull the working level
down 8 dB in the manifest**. If a surface skin is audible as a separate sound rather than as
colour, it is too loud.

---

## 5. Grains and texture

### `limb_tap` ×4 — 40–100 ms, the burst filler

These are what makes a burst read as three to four grains at 46–104 ms spacing. Quiet, dry, and
**heavily pitch-scattered at runtime**, so generate them neutral and let the plugin spread them.

| File | Prompt |
|---|---|
| `limb_tap_01` | `light quick tap of a forearm against a hard floor, tiny dry contact, very short, close mic, dry studio foley, no reverb` |
| `limb_tap_02` | `small dull knock of a knee hitting packed dirt, quiet brief thud, close mic, dry studio foley, no reverb` |
| `limb_tap_03` | `soft scuff tap of a boot heel clipping stone, short gritty tick, close mic, dry studio foley, no reverb` |
| `limb_tap_04` | `quiet slap of loose fabric and flesh brushing a hard surface, tiny brief contact, close mic, dry studio foley, no reverb` |

*Post:* trim aggressively — 40–100 ms means the tail goes. Normalise, then set 12–15 dB below the
composite. These live **under the cliff** described in §4 of the analysis; they are the quiet 9 of
every 10 events, not heroes.

### `crunch_gran` ×2 — 250–400 ms, the bone-break character

**The most commonly mis-briefed file in the set.** It is not a snap. The analysis counts 24
separate transients in the 250–800 Hz band across 420 ms — it is *density*, a granular crunch bed.
Prompt for a sustained crunch gesture, not a break.

| File | Prompt |
|---|---|
| `crunch_gran_01` | `slow crushing of a bundle of dry celery and walnut shells in a fist, dense continuous crackling, many small cracks packed together, close mic, dry studio foley, no reverb` |
| `crunch_gran_02` | `twisting a handful of raw chicken bones and gristle, wet dense crunching, rapid overlapping small snaps, no single loud crack, close mic, dry studio foley, no reverb` |

*Post:* trim to 250–400 ms. **Band-pass 250–800 Hz with a wide bell +4 dB** — that band is where
the granularity has to live. Count the peaks: if you cannot see 15+ distinct transients in the
waveform across the file, it is a snap and it will sound like a stick breaking. Regenerate.

### `gore_wet` ×2 — 200–400 ms, obliterate tier only

| File | Prompt |
|---|---|
| `gore_wet_01` | `wet squelch of raw meat and offal squeezed hard, thick liquid burst, single gesture, close mic, dry studio foley, no reverb` |
| `gore_wet_02` | `heavy wet slap of soaked cloth and raw meat torn apart, sloppy liquid rip, close mic, dry studio foley, no reverb` |

---

## 6. Loops — 1.5–3 s, must be seamless

The engine gives whole-file looping only, so **the loop point is the asset's problem**. Generate
5–10 s, find a stable 2 s window, crossfade the ends by 100–200 ms, and check the seam by playing
it four times through. Any audible pulse at the seam becomes a rhythm in-game.

### `scrape_loop` ×1 — 1.5–3 s

**Low-tilted grinding rumble with grain riding on it. Not a hiss.** The measured band tilt goes
*down*: sub −37, air −56 dB. A bright scrape is the single easiest way to get this wrong.

This prompt is the one that shipped. The **first** version of it said `low grinding rumble, no
high hiss` and over-corrected: the model stripped everything above 250 Hz and the take played as a
rumble with no grit, which no EQ could put back. The reference slide is much flatter than
"low-tilted" suggests — only the air band is really down.

```
heavy limp body dragged across rough stone slabs, gritty broadband grinding friction, coarse
sand and grit crunching under the weight, low rumble with clear midrange scrape, steady
continuous, no hiss, no whistle, close mic, dry studio foley, no reverb
```

Generate 8–10 s so there is a stable window to cut from.

*Post:* trim to a seamless 2 s. **Low-shelf +4 dB at 200 Hz, high-shelf −8 dB at 4 kHz**, then
check the band balance actually tilts downward. Target ~65 grain peaks per second — if it sounds
smooth rather than granular, layer a second take at −6 dB with a slight pitch offset. Sits 15–25 dB
under the impacts.

### `foley_cloth` ×1 — 1.5–3 s, no transients

```
continuous soft rustle of heavy woollen cloak fabric moving, steady gentle friction, no
impacts, no footsteps, close mic, dry studio foley, no reverb
```

*Post:* this is part of the continuous bed, which the analysis puts **30–36 dB under the hero
hit**. Remove any peak that stands more than 6 dB above the bed — a transient here will poke
through the mix at the moment everything else is quiet.

### `air_whoosh` ×1 — 1–2 s, low airy movement

```
low soft air movement of a heavy body falling, dull airy rush, no whistle, no high hiss,
smooth, close mic, dry, no reverb
```

*Post:* low-pass at 2 kHz. This drives the airborne anticipation rise as a parameter, so it must
be **flat and loopable**, not a designed sweep with a built-in climax.

---

## 7. Accents

### `head_impact` ×2 — 300–500 ms

Dull skull thud with a granular edge and a slight ring. The analysis has the head hit as the one
with real body *and* granularity — seven low-mid peaks, more than a plain thud, far fewer than a
spine break.

| File | Prompt |
|---|---|
| `head_impact_01` | `heavy melon wrapped in cloth dropped onto stone, dull dense thud with a short crackle of cracking rind, single impact, close mic, dry studio foley, no reverb` |
| `head_impact_02` | `hard blunt blow to a coconut in a leather sack, deep dull knock with a brief splintering edge and faint hollow ring, single hit, close mic, dry studio foley, no reverb` |

*Post:* keep a *slight* ring — 100–150 ms of a low resonance around 200–300 Hz — but no more.
This is the one file allowed any tonal tail.

### `settle_rest` ×2 — 200–400 ms, closes the event

| File | Prompt |
|---|---|
| `settle_rest_01` | `heavy limp body settling onto the ground, soft final flop of flesh and cloth, quiet, no impact transient, close mic, dry studio foley, no reverb` |
| `settle_rest_02` | `loose fabric and a heavy limb slumping to rest on dirt, soft dull settle with a faint rustle, quiet, close mic, dry studio foley, no reverb` |

*Post:* deliberately soft attack — this is the only impact-family file that should **not** have an
instant transient. Set 15 dB under the composite.

---

## 8. Not generated

`grunt_impact` and `scream_big` stay declared and unfilled. When they are filled, they are the one
slot where a real human recording beats generation outright, and generated screams are the single
worst-sounding thing in this whole list. Record them or licence them.

---

## 8b. Prompts that had to be rewritten

The four slots below did not come out of their §4–§7 prompts. What each rewrite changed is worth
keeping, because the same correction applies to any slot with the same shape.

### `air_whoosh` — say the level, not the movement

Every first-batch take measured 8–15 dB of envelope spread against a 6 dB ceiling: "rush" and
"movement" get read as a gesture with a shape, and the slot wants a bed. Naming the level directly
fixed three of four.

```
low soft air movement, constant unchanging dull airy rush, steady drone of moving air, one
level throughout, no gusts, no swells, no whistle, no hiss, featureless and smooth, close
mic, dry, no reverb
```

### `settle_rest` — borrow `00-Design.md`'s own image

The §7 prompt returned takes at −45 to −50 dBFS with a 6.5 kHz centroid. Swapping the material for
the design's own description of the sound got three of four.

```
heavy limp body settling onto a floor, one soft dull low thump with heavy cloth folding
around it, like dropping a coat with a book in the pocket, no crack, no bright edge, quiet,
close mic, dry studio foley, no reverb
```

### `gore_wet` — "burst" and "rip" are heard as spray

The §5 prompts came back at 5.8–8.4 kHz against a 4000 Hz ceiling. Slowing the gesture and naming
the mass rather than the liquid dropped it to 1.6–3.1 kHz.

```
heavy wet slap of soaked heavy cloth onto raw meat, deep low muffled wet thud with a thick
sucking pull, nothing bright, no spray, no splash, no hiss, close mic, dry studio foley,
no reverb
```

### `head_impact` — ask for the ring by its duration

The §7 coconut prompt ran 166–196 ms against a 300–500 ms slot and had no tail. Giving the ring a
stated length got both shipped files.

```
hard blunt blow to a coconut in a leather sack, deep dull low knock with a brief splintering
edge, then a faint hollow ring fading slowly over half a second, single hit, close mic, dry
studio foley, no reverb
```

### `crunch_gran` — the one prompting could not fix

Four revisions and 30 takes never got the centroid under 4897 Hz against a 4500 Hz ceiling. The
density was always fine. Shipped with a shelf instead — see `03-Asset-Status.md` §4.

**Keep the first 20 characters of a rewritten prompt identical to the original.** That string is
the download filename and the key `sfx.py`'s `PROMPT_HEADS` infers the slot from, so a rewrite that
preserves it needs no new table entry and no `--slot` flag.

---

## 9. Build order

**First taste is 13 files.** Generate in this order — each step is audible on its own:

| Order | Files | Why |
|---|---|---|
| 1 | `imp_sub` ×2 | Synthesise first. Without it none of this feels like the references |
| 2 | `imp_body` ×3 | The mass |
| 3 | `imp_transient` ×3 | The contact |
| 4 | `limb_tap` ×3 | Turns single hits into bursts |
| 5 | `scrape_loop`, `foley_cloth` | The bed and the slide |

Then the remaining 16 fill in, and slot fallback means a missing one is a quieter mod, not a broken
one.

---

## 10. Acceptance checks

`tools/sfx.py` automates all of this — `eval` judges a folder of takes against these specs and
`make` applies the post-pass. See `tools/README.md`. The list below is what it checks and why.

Before a file goes in the pack, it passes all of these. Each maps to a measurement in
`04-Reference-Analysis.md`.

- [ ] Mono, 44.1 kHz, 16-bit, no DC offset
- [ ] First sample is the attack — **zero leading silence**, zero crossing start
- [ ] No audible baked reverb tail
- [ ] Length inside the slot's stated range
- [ ] Peak at −1.5 dBFS (−1.0 for `imp_sub`); crest 8.8–13.8 dB measured on the **assembled
      composite**, not per layer
- [ ] Impact layers 20 dB down within ~150 ms; nothing rings past 600 ms
- [ ] High-passed at 120 Hz unless it is `imp_sub`
- [ ] Loops seamless over four consecutive passes
- [ ] `crunch_gran` shows 15+ distinct transients in 250–800 Hz
- [ ] `scrape_loop` band balance tilts **downward**, sub above air
- [ ] Bright slots tilt **upward** after their high-pass — centroid alone will not catch a
      bass-led take, so check tilt and how much level the file loses to the high-pass
- [ ] The three composite layers, played at 0 / +20 / +65 ms, read as **one** sound and not as three

That last check is the only one that matters if you skip the rest.
