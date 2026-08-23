# 08 — Audio surfaces in Skyrim

What the engine and the ESM records actually offer as a "surface" for sound selection, and
how much of it is real. Read after [04](04-Findings.md) — that doc's *Surfaces and geometry*
table is the capture's view of this, from one room; this is the whole palette.

Everything below was read out of `Skyrim.esm`, `Update.esm`, `Dawnguard.esm`,
`HearthFires.esm`, `Dragonborn.esm` and `SkyrimVR.esm` directly, and cross-checked against
`RE::MATERIAL_ID` in CommonLibSSE. Method in [§9](#9-how-this-was-produced).

## 1. There are two different questions

- **What surfaces can the engine *tell us apart*?** → 90 Havok material IDs.
- **What surfaces does vanilla *make a different sound for*?** → 13, and only on the footstep
  path. On the ragdoll path it is **three**.

That gap is the whole reason this mod exists, and it is wider than the note assumed.

## 2. The material ID is a hash, and we can compute it

`MATERIAL_ID` is not an arbitrary enum. It is:

```
CRC32( poly 0xEDB88320, init 0x00000000, no final XOR ) over the MATT record's MNAM string, lowercased
```

Verified against all 79 `MATT` records: 79/79 match `RE::MATERIAL_ID`. Note the two
non-standard parameters — init is **0**, not `0xFFFFFFFF`, and there is **no final XOR**.
A stock `crc32()` from any library will not reproduce these.

This matters practically: an unknown ID coming out of `bhkShape::GetMaterialID` can be
reverse-matched against a name list rather than discarded, and a mod can mint its own IDs for
new surfaces that hash consistently.

Careful: the hash is over `MNAM`, **not** `EDID`, and the two disagree for 35 of the 79
records. `MaterialBrokenStone`'s MNAM is `Broken Stone` (with a space); `MaterialGlassStairs`
is `StairsGlass`; `MaterialInsect` is just `Insect`. Hashing the EDID scores 47/79 and gives
silently wrong answers for the rest.

## 3. The full material palette — 90 IDs, 79 with records

79 materials have a `MATT` record. Ten more exist in the engine enum with **no record in any
official master**:

```
Amulet   DLC1DeerSkin   DLC1SabreCatPelt   DLC1SwingingBridge   DragonSkeleton
DraugrSkeleton   SteelGreatSword   TrailerSteelSword   Trap        (and None = 0)
```

**`Trap` is on that list, and `Trap` turned up in the capture data** — 7 world episodes at
median 85 units/s, in the hand-driven dataset that has since been deleted. It does not
appear in the scripted run, whose materials are Carpet, Wood, WoodStairs, Stone and Skin,
but that is a property of one room rather than a correction. A shape can carry a material
ID that no ESM record describes. The resolver must not assume `MATERIAL_ID → MATT` always succeeds: those
IDs have no parent, no buoyancy and no impact set to fall back through.

The recorded materials that are plausibly *surfaces*, with the fields that matter:

| MATT EDID | MNAM (hashed) | MATERIAL_ID | parent | buoy | havok impact set (HNAM) |
|---|---|---|---|---|---|
| MaterialStone | Stone | 3741512247 | — | -1.00 | **none** |
| MaterialHeavyStone | Heavy Stone | 1570821952 | MaterialStone | -1.00 | **none** |
| MaterialBrokenStone | Broken Stone | 131151687 | MaterialStone | -1.00 | **none** |
| MaterialStairsStone | StairsStone | 899511101 | MaterialStone | -1.00 | **none** |
| MaterialStairsBrokenStone | StairsBrokenStone | 2892392795 | MaterialStone | -1.00 | **none** |
| MaterialStoneAsStairs | MaterialStoneAsStairs | 1886078335 | MaterialStone | -1.00 | **none** |
| MaterialWoodLight | Light Wood | 365420259 | — | 0.50 | PHYGenericWoodLightImpactSet |
| MaterialWoodMedium | Wood | 500811281 | MaterialWoodLight | 0.25 | PHYGenericWoodMediumImpactSet |
| MaterialWoodHeavy | Heavy Wood | 3070783559 | MaterialWoodLight | -0.25 | **none** |
| MaterialStairsWood | StairsWood | 1461712277 | MaterialWoodLight | 0.50 | **none** |
| MaterialWoodAsStairs | MaterialWoodAsStairs | 1803571212 | MaterialWoodLight | 0.50 | **none** |
| MaterialDirt | Dirt | 3106094762 | — | -0.50 | **none** |
| MaterialGravel | Gravel | 428587608 | MaterialDirt | -1.00 | **none** |
| MaterialSand | Sand | 2168343821 | MaterialDirt | -0.50 | **none** |
| MaterialAsh | MaterialAsh | 534864873 | MaterialDirt | -0.50 | **none** |
| MaterialMud | Mud | 1486385281 | — | -0.50 | **none** |
| MaterialGrass | Grass | 1848600814 | — | 0.00 | **none** |
| MaterialSnow | Snow | 398949039 | — | 0.00 | **none** |
| MaterialStairsSnow | StairsSnow | 1560365355 | MaterialSnow | -1.00 | **none** |
| MaterialIce | Ice | 873356572 | MaterialSnow | 0.25 | **none** |
| MaterialIceForm | MaterialIceForm | 2431524493 | MaterialIce | 1.00 | PHYPotsPansImpactSet |
| MaterialWater | Water | 1024582599 | — | 0.00 | **none** |
| MaterialWaterPuddle | MaterialWaterPuddle | 3764646153 | MaterialWater | 1.00 | **none** |
| MaterialCarpet | MaterialCarpet | 1286705471 | MaterialStone | -0.50 | PHYGenericClothImpactSet |
| MaterialCloth | Cloth | 3839073443 | MaterialCarpet | -0.50 | PHYGenericClothImpactSet |
| MaterialGlass | Glass | 3739830338 | — | 0.00 | **none** |
| MaterialGlassStairs | StairsGlass | 880200008 | MaterialGlass | 0.50 | **none** |
| MaterialHeavyMetal | Heavy Metal | 2229413539 | — | -1.00 | PHYGenericMetalHeavyImpactSet |
| MaterialSolidMetal | Solid Metal | 1288358971 | MaterialHeavyMetal | -1.00 | PHYGenericMetalMediumImpactSet |
| MaterialMetalLight | MaterialMetalLight | 346811165 | MaterialSolidMetal | -0.50 | PHYGenericMetalLightImpactSet |
| MaterialChain | MaterialChain | 3074114406 | MaterialSolidMetal | -1.00 | **none** |
| MaterialChainMetal | MaterialChainMetal | 438912228 | MaterialHeavyMetal | -1.00 | **none** |
| MaterialWeb | Web | 3934839107 | — | 0.00 | **none** |
| MaterialGhost | Ghost | 3312543676 | — | 0.00 | **none** |
| MaterialWard | Ward | 3895166727 | — | 0.00 | **none** |

The bodies, which are what *hits* those surfaces:

| MATT EDID | MNAM | MATERIAL_ID | parent | havok impact set |
|---|---|---|---|---|
| MaterialSkin | Skin | 591247106 | — | **PHYBodyMedium** |
| MaterialSkinSmall | MaterialSkinSmall | 2632367422 | MaterialSkin | PHYBodySmallImpactSet |
| MaterialSkinLarge | MaterialSkinLarge | 2965929619 | MaterialSkin | PHYBodyLargeImpactSet |
| MaterialSkinSkeleton | MaterialSkinSkeleton | 2821299363 | — | PHYBodyBones |
| MaterialSkinMetalSmall | MaterialSkinMetalSmall | 3855001958 | MaterialMetalLight | PHYBodyMetalSmallImpactSet |
| MaterialSkinMetalLarge | MaterialSkinMetalLarge | 3387452107 | MaterialHeavyMetal | PHYBodyMetalLargeImpactSet |
| MaterialBoneActor | BoneActor | 2058949504 | — | PHYBodyMedium |
| MaterialBone | MaterialBone | 3049421844 | — | PHYBonesImpactSet |
| MaterialOrganic | Organic | 2974920155 | — | PHYBodyMedium |
| MaterialOrganicLarge | OrganicLarge | 1322093133 | — | PHYBodyMedium |
| MaterialInsect | Insect | 668408902 | — | PHYBodyMedium |
| MaterialMeat | MaterialMeat | 220124585 | MaterialCloth | PHYMeatImpactSet |
| MaterialArmorHeavy | MaterialArmorHeavy | 3708432437 | MaterialHeavyMetal | PHYMaterialArmorHeavyImpactSet |
| MaterialArmorLight | MaterialArmorLight | 3424720541 | MaterialHeavyMetal | PHYMaterialArmorLightImpactSet |
| MaterialDragon | Dragon | 2518321175 | — | **none** |
| MaterialAlduin | Alduin | 1730220269 | — | **none** |

The remaining 28 are props and weapons rather than surfaces: `MaterialBarrel`,
`MaterialBasket`, `MaterialBottle`, `MaterialBottleSmall`, `MaterialBook`, `MaterialCoin`,
`MaterialPotsPans`, `MaterialCeramicMedium`, `MaterialCarriageWheel`, `MaterialArrow`,
`MaterialBoulderSmall/Medium/Large`, and 14 `MaterialBlade*` / `Blunt*` / `Axe*` / `Bows*` /
`Block*` / `Shield*` weapon materials.

`buoy` is buoyancy, −1.0 = sinks. `flags`: bit 0 = stairs, bit 1 = arrow-sticks.
**7 materials carry the stairs flag** — which is how the engine already knows what
[04](04-Findings.md) measured the hard way — `WoodStairs` at median 181 u/s against `Wood`
at 36 in the same room, and `StoneStairs` at 122 against 56 in the deleted dataset. Take the
first of those lightly: all eleven `WoodStairs` episodes are in one take, and that take is the
hardest push in the set, so surface and input are confounded.

## 4. Every material is a live audio axis — 296 impact data sets

All 79 recorded materials get an impact assigned in at least 57 of the 296 `IPDS` records;
the most-covered (`MaterialWoodLight`) appears in 287. **No material is a dead letter.** So
surface *is* a first-class selection axis in Skyrim's data model — the palette is there.

The runtime lookup is:

```
impacting object's MATT ─HNAM→ IPDS ─PNAM[ other object's MATT ]→ IPCT ─SNAM/NAM1→ SNDR → .wav
```

The **impacting** thing supplies the set; the **surface** is the key into it. So for a ragdoll
landing on stone, `MaterialStone`'s empty HNAM is irrelevant — `MaterialSkin`'s
`PHYBodyMedium` is the set, and `MaterialStone` is the lookup key.

## 5. The ragdoll path resolves 60 materials into 3 sounds

This is the finding that matters. `PHYBodyMedium` — the set a humanoid ragdoll actually uses —
maps 60 materials onto only **three** distinct impacts:

| impact | materials mapped | sound descriptors |
|---|---|---|
| `PHYBodyMediumDirtImpact` | **53** | `PHYBodyMediumDirtL` / `…DirtH`, 3 wav each |
| `PHYBodyMediumWoodImpact` | 6 — WoodLight, WoodMedium, WoodHeavy, StairsWood, WoodAsStairs, Barrel | `PHYBodyMediumWoodL` / `…WoodH` |
| `PHYBodyMediumGrassImpact` | 1 — Grass | `PHYBodyMediumGrassL` / `…GrassH` |

Stone, snow, ice, water, carpet, metal, gravel, sand, mud, glass, all six stairs variants and
every weapon and prop material **all play the dirt sound**. A body landing on a frozen lake
and a body landing on a carpet are bit-identical in vanilla.

The other body sets are worse:

| set | materials mapped | distinct sounds |
|---|---|---|
| `PHYBodyMedium` | 60 | 3 |
| `PHYBodyLargeImpactSet` | 57 | **1** |
| `PHYBodySmallImpactSet` | 57 | 1 |
| `PHYBodyBones` | 75 | 1 |
| `PHYBodyMetalLargeImpactSet` | 57 | 1 |
| `PHYBodyMetalSmallImpactSet` | 57 | 1 |

Of the 295 IPDS that map any material at all, **175 have exactly one distinct sound** — the material column is filled
in but decorative. Only 33 sets discriminate 10 or more ways, and the top of that list is
weapons (`WPNzBluntImpactSet`, 24 distinct across 75 materials), not bodies.

There is one piece of vanilla behaviour we should not ignore: `SNAM` and `NAM1` on the body
impacts are consistently an `…L` / `…H` pair (light / heavy), 3 wav variants each. Vanilla
already models ragdoll impact loudness as a 2-way choice with 3-way random variation — 6
samples per surface class. That is the ceiling our continuous intensity model has to beat. The
range that model spans is **20 → at least 800 u/s** — the highest clean contact recorded so
far — with the true top still unmeasured, because both fall takes were discarded. See
[04](04-Findings.md#impact-magnitude--and-no-measured-ceiling).

## 6. The footstep path is where the real 13-surface palette lives

The only vanilla system that genuinely discriminates surfaces is footsteps.
`DefaultFootstepWalkLImpactset` maps 26 materials onto 13 distinct impacts:

| surface class | materials that map to it |
|---|---|
| Stone | Stone, HeavyStone, BrokenStone, StoneAsStairs, Glass, GlassStairs |
| StoneStairs | StairsStone |
| Wood | WoodLight, WoodMedium, WoodHeavy, WoodAsStairs, Barrel |
| WoodStairs | StairsWood |
| Dirt | Dirt, Mud |
| Gravel | Gravel, Sand |
| Grass | Grass, **HeavyMetal, SolidMetal** |
| Snow | Snow |
| Ice | Ice |
| Ash | Ash |
| Carpet | Carpet |
| Water | Water |
| WaterPuddle | WaterPuddle |

(Metal mapping to the *grass* footstep is not a parse error — it is what the record says.
Vanilla oddity, and a reason not to inherit these mappings uncritically.)

Assets are per-surface and split player / NPC, e.g.
`Data\Sound\FX\FST\Player\Snow\Walk\L\FST_Player_Snow_Walk_01.wav`,
`Data\Sound\FX\FST\NPC\Carpet\FST_NPC_Carpet_Walk_01.wav`. There are also armour-weight
variants (`FSTWalkArmorHeavyLImpactSet`, `…ArmorLight…`, plus Skaal), which is the
armour-as-timbre axis [04](04-Findings.md) predicted we would need — and independent
confirmation that armour is a *sound-selection* axis in vanilla too, never a physics one.

**These 13 classes are the right target palette**, and their wav trees are the obvious
placeholder source while we have no recorded ragdoll audio.

## 7. Terrain resolves through LTEX, not through the shape

The deleted dataset had 57 episodes on "terrain, unresolved"; the scripted run has **no
terrain contact at all**, so the recorder's per-tick land-record sample
([02](02-Data-Dictionary.md)) has still never been exercised. The clean mapping exists: **91 `LTEX` records each carry an `MNAM` pointing at a `MATT`.**

| material | LTEX records |
|---|---|
| MaterialDirt | 26 |
| MaterialGrass | 25 |
| MaterialBrokenStone | 7 |
| MaterialSnow | 6 |
| MaterialMud | 6 |
| MaterialGravel | 6 |
| MaterialAsh | 6 |
| MaterialStone | 5 |
| MaterialSand | 2 |
| MaterialIce | 1 |
| MaterialBone | 1 |

So natural ground collapses to **9 surface classes** (Bone is Soul Cairn, Ice is a single
record). `LCoastBeach01 → MaterialSand`, `LCaveDirt → MaterialDirt`,
`LDLC01SoulCairnBones01 → MaterialBone`. This is the correct terrain resolver: read the
landscape texture at the *contact point* and take its `MNAM`, rather than the actor-position
sampling the recorder does now, which [02](02-Data-Dictionary.md) already flags as wrong
across a texture seam.

## 8. What this means for the design

1. **Target 13 surface classes, not 90 materials.** The 90 IDs collapse to 13 perceptually
   distinct floors, and vanilla's own footstep data is the evidence for which collapse is
   right. A 90-entry sound bank is wasted work.
2. **Build a `MATERIAL_ID → surface class` table of our own.** Do not read `MATT`→`HNAM` at
   runtime: 30 of the 79 materials have no impact set at all, and those that do point at sets
   that mostly cannot tell surfaces apart. Follow the `PNAM` parent chain for unknown IDs, and
   hard-default anything still unresolved — including `Trap` and the other nine record-less
   IDs — to Stone.
3. **Compute the hash ourselves** with the non-standard CRC32 in [§2](#2-the-material-id-is-a-hash-and-we-can-compute-it),
   so unknown IDs can be named in logs instead of printed as bare integers.
4. **Stairs are already flagged** — bit 0 of `FNAM`, on 7 materials. Free confirmation of the
   [04](04-Findings.md) stairs finding, and free routing to a harder sample.
5. **Beat 6 samples per surface.** Vanilla gives a body impact 2 intensity levels × 3 random
   variants. That is the bar, and it is a low one.
6. **Terrain: use LTEX `MNAM`.** Closes the "unresolved terrain" gap in
   [06](06-Gaps-and-Requested-Captures.md) without new captures.

## 9. How this was produced

A standalone ESM record parser (GRUP walk, zlib for compressed records, `XXXX`
long-subrecord handling), run over the six official masters with later masters overriding
earlier by global FormID. Records read: `MATT`, `IPDS`, `IPCT`, `SNDR`, `FSTP`, `FSTS`,
`LTEX`. Counts: 79 MATT, 296 IPDS, 646 IPCT, 174 FSTP, 52 FSTS, 91 LTEX. The hash was
recovered by brute-forcing 6 CRC parameterisations × 6 string variants against
`RE::MATERIAL_ID`; exactly one combination scored 79/79.

No audio was auditioned — this is what the records *say*, and the wav paths are unverified
against the BSAs. The claim "these two materials sound the same" means "they resolve to the
same `IPCT`", which is a stronger statement than a listening test, but says nothing about
whether the sound is any good.
