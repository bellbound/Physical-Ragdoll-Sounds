# Vanilla Skyrim candidates — staging, not pack

35 files extracted from the base game's `sound/fx/phy` and `sound/fx/fst`, kept here because they
measure well against slots in `Slots.md` §3. `manifest.csv` carries the original path, the candidate
slot, the `sfx.py check` verdict against that slot, and whether it is usable **direct** or only as
**split** source — 29 of the 35 are clean whole, the other 6 are multi-contact and need cutting
first. Re-derive it any time with `sfx.py eval <file> --slot <slot>`.

**Nothing here ships.** These are Bethesda's assets; the pack is ours. They are reference and source
material for retakes, not files to assign.

---

## Why this folder is safe

Three separate things would have to change before one of these reached a player:

| Mechanism | Scope | Why `skyrim/` is outside it |
|---|---|---|
| `deploy-pack.ps1:47` | `Get-ChildItem assets\sfx -Filter *.wav` | no `-Recurse`, so a subfolder is not swept into `sounds\` |
| `deploy-pack.ps1:90` | mirrors `assets\sfx\library` only | `skyrim\` is a sibling of `library\`, not inside it |
| `Sfx.cpp:151` | `fs::directory_iterator(m_directory)` | non-recursive, so the game would not index a subfolder even if one were deployed |

Keep it that way. Moving these into `library/` deploys them; that is the line.

---

## What they are

Vanilla `phy` files are **whole events**, not one-shots — Bethesda authored the entire ragdoll
tumble as one sample, where our engine builds the event from timed layers (`Slots.md` §1). Onset
counts, via `sfx.py onsets`:

```
skyrim_phy_body_large_h_01         7 onsets over 2.0 s
skyrim_phy_body_large_h_03        11 onsets
skyrim_phy_body_medium_wood_l_01  10 onsets
```

So playing one whole gives an event inside an event. They are usable only through `sfx.py split`,
then `make` with `--start/--len`. They are also **32 kHz mono** against a 48 kHz pack (§6) —
harmless for the dull slots, since a 16 kHz Nyquist clears the 9 kHz ceiling of the brightest
spec, but it is a resample.

## What each group is for

| Files | Slot | Why it was kept |
|---|---|---|
| `phy_body_large_*` (5) | `imp_body` (**split only**) | centroid 1293–1987, **tilt +26 to +31** — the most bass-led material in the vanilla set, darker than our built 2166–3425. RETRY whole: 17–26 lo-mid transients against the slot's 0–5, which is the multi-contact tumble, not a character fault. Split first |
| `phy_body_dragon_dirt_h_*` (3) | `crunch_gran` | see below |
| `phy_snow_heavy_h_01`, `phy_grass_heavy_h_01/03`, `phy_generic_dirt_medium_h_01` | `crunch_gran` | runners-up, centroid 3322–4458 with 16–19 lo-mid transients |
| `phy_generic_dirt_heavy_*` (6) | `surf_soft` | centroid 1008–3105, tilt +18 to +31; matches built 674–2475, and `surf_soft` is the highest-firing slot at 27× |
| `phy_generic_wood_medium_l_*` (3) | `surf_wood` | centroid 1566–1666 at 245–252 ms — near drop-in, barely needs trimming |
| `fst_wood_land_*`, `fst_wood_jumpdown_*` (5) | `surf_wood` | a body-weight landing on boards, which is literally the slot |
| `phy_body_medium_wood_l_*` (3) | `head_impact` | centroid 1980–2222, tilt +20 to +24; built is 1100–2935. Dull-then-ringing |
| `phy_generic_cloth_*` (5) | `settle_rest` | centroid 2069–2829, tilt +7 to +17; built is 2469–2629 |
| `phy_generic_cloth_h_02` | `head_impact` | centroid 3437 — too bright for `settle_rest`'s 3200 ceiling, so it sits with the accents instead |

## The `crunch_gran` files are the reason this folder exists

`03-Asset-Status.md` §4 records `crunch_gran` as the one slot prompting could not solve: four prompt
revisions, 30 takes, **never below 4897 Hz centroid** against a 4500 ceiling and a reference spine
break at 2705 — which is why the two shipped files are the only ones in the pack that are not purely
prompted, and had to be shelved with a −18 dB high shelf.

`skyrim_phy_body_dragon_dirt_h_01` measures **centroid 2899 Hz, tilt +19.7, 33 lo-mid transients**:
under the ceiling, essentially on the reference, and roughly double the density the slot needs.
`_h_03` is 2985 / +17.5 / 30.

That does not make them shippable. It makes them **proof the slot is reachable**, and a better
calibration target than anything currently in `Example/` — which is a stronger position than §4's
current "prompting cannot move this one."

---

## What was rejected, so it is not re-surveyed

All 1,261 files in both trees were measured against every slot. 1,086 passed at least one, which is
why the numbers alone decide nothing — the specs gate character, not identity (`Slots.md` §4).

- **~900 `fst` files** — walk/run/sprint/sneak/stairs. Boot-and-armour character, wrong event.
- **Every object family** — `coin`, `bottle`, `book`, `potspans`, `ceramic`, `blade*`, `blunt*`,
  `shield*`, `armor`, `generic/metal`, `generic/chain`. They pass slots numerically and would make a
  corpse sound like a dropped saucepan. `generic/chain` at tilt −52 is the clearest case.
- **`phy/meat`** — best-named candidate in the set, measures wrong: centroid 4096–5073, above
  `imp_body`'s 4200 ceiling, tilt only +0.8 to +7.0. A small wet slap, not mass.
- **`phy/bones`** — tilt −19 to −35, a dry bright rattle. `phy/body/bones` is closer (3663–4392,
  tilt +3 to +10) but carries 6–13 lo-mid transients against the 15 floor: right colour, too sparse.
- **`fst/grass/scuff`** — tilt −18 to −21, against `scrape_loop`'s +5 to +19. A hiss, and the exact
  trap `04-Reference-Analysis.md` §7 flags.
- **Every loop slot.** Not one file in 1,261 passes `scrape_loop`, `foley_cloth` or `air_whoosh` —
  they are all short one-shots that fail steadiness and seam. Nothing vanilla helps those three.
- `fst/water/land/fst_waterland_01.wav` and `_02.wav` hold 0.0 ms of audio — vanilla duds.
