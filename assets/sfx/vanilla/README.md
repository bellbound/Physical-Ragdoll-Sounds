# Vanilla sound extract — for playback of a take's vanilla track

Empty in the repo. Put an extract of the game's `sound/fx` tree here and the testbench can
play a take's **vanilla track** — the sounds Skyrim itself chose while that take was being
recorded — under the *Use Vanilla Audio* switch.

**Nothing here ships. Nothing here can ship.** These are Bethesda's files, exactly like
`../skyrim/`, and they are outside a release for the same three reasons that folder's README
sets out:

| Mechanism | Scope | Why `vanilla/` is outside it |
|---|---|---|
| `deploy-pack.ps1:65` | `Get-ChildItem assets\sfx -Filter *.wav -File` | no `-Recurse`, so a subfolder is not swept into `sounds\` |
| `deploy-pack.ps1:108` | mirrors `assets\sfx\library` only | `vanilla\` is a sibling of `library\`, not inside it |
| `Sfx.cpp` | `fs::directory_iterator(m_directory)` | non-recursive, so the game would not index it even if it were deployed |

Keep it that way. Moving these into `library/` deploys them; that is the line.

---

## What to put here

Whatever `sound/fx` holds, in any layout. The index is a recursive walk keyed on the
*filename*, so a faithful mirror of the game's tree and one flat folder work equally well:

```
assets/sfx/vanilla/
  phy/body/medium/dirt/l/phy_body_medium_dirt_l_01.wav
  phy/body/medium/dirt/l/phy_body_medium_dirt_l_02.wav
  ...
```

The body impact sets are the only part that matters — `phy/body/**`, `phy/meat/**` and
`phy/material/armor/**` — but there is no harm in extracting more.

Point it somewhere else with `--vanilla-sounds <dir>` if the extract already lives on disk;
the default is just this folder.

## How a descriptor finds its files

A vanilla track row names the descriptor that fired and nothing else — the game half has a
name and never has a path, because `BGSStandardSoundDef::soundFiles` holds a hash of the path
with no way back. So the name is turned into a filename by Bethesda's own convention:

```
PHYBodyMediumDirtL  ->  phy_body_medium_dirt_l  ->  phy_body_medium_dirt_l_{01,02,03}.wav
```

Camel humps become underscores, the `PHY` acronym stays whole, and a trailing `_NN` on a
filename is the variant number. Checked against every original path in `../skyrim/manifest.csv`:
every descriptor name whose files are present resolved exactly, with no false matches.

A descriptor that resolves to nothing is reported once in the log and counted in the switch's
tooltip, so "vanilla was quiet here" and "the extract is missing that sound" never look alike.

## What it cannot reproduce

Which of a descriptor's variants the audio engine actually drew, and what its per-play dB and
frequency variance rolled. Both happen inside `BSAudioManager` after the sound handle is built.
The testbench draws a variant from the seed so a replay is repeatable, and leaves the variance
flat rather than inventing a roll. `<take>_vanilla.csv` says the same thing at the top of every
file it writes.
