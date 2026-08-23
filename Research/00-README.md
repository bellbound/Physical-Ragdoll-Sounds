# Ragdoll impact data — research

What is in here, and the order to read it in.

| Doc | Answers |
|---|---|
| [01-Dataset-Map.md](01-Dataset-Map.md) | Which file is which take, what each one was for, and what it actually contains — which is often not the same |
| [02-Data-Dictionary.md](02-Data-Dictionary.md) | Every column and YAML field, its units, and how far it can be trusted |
| [03-Reduction-and-Cleaning.md](03-Reduction-and-Cleaning.md) | Raw rows → one row per collision. The dedup rules, with the measured multipliers |
| [04-Findings.md](04-Findings.md) | What the data actually says about ragdoll impacts, armour, surfaces and repeatability |
| [05-Capture-Pipeline-Issues.md](05-Capture-Pipeline-Issues.md) | Where `QuickModMenuNG`'s recorder and its scripted run could mislead us — what got fixed, and what this run broke |
| [06-Gaps-and-Requested-Captures.md](06-Gaps-and-Requested-Captures.md) | What is still missing, and what to record next |
| [07-Reliability-Requirements.md](07-Reliability-Requirements.md) | What a runtime sound system must do to work at 24–144 fps, on any actor, in any cell |
| [08-Audio-Surfaces.md](08-Audio-Surfaces.md) | What surfaces Skyrim can tell apart, what it actually makes a different sound for, and the 13 that matter |

## The dataset

`NewRecordings/` — **12 files, hand-picked** out of one 3½-minute session on 2026-08-22 at
15:38, produced by one press of **Debug → Record Requested Data** in Dragonsreach. The run is
thirteen scripted takes; two of them record a second actor as well.

**Four of the run's takes were discarded as bad data and are not in the folder** — `fall-3m`,
`fall-10m`, and the subject's own files for `stack-both` and `stack-onto-standing`. What
remains is nine of the thirteen takes, with the second actor's files for takes 7, 8 and 9.
This folder is a curated set, not a dump: **do not top it up from the recorder's output
directory.**

Per take: a `.csv` of events, a `.yaml` sidecar, an `_sync.csv` OBS timing pair, and for the
nine Proventus takes a `.mp4`. In total 1377 impact rows, 1632 touch, 1203 separate, 2149
listener, 414 limb samples, 65 state changes. Every take reports `dropped: 0` and
`complete: true`.

The fourteen hand-driven takes this research started from — a VR leash yank, uncontrolled
input, two actors, six rooms — have been **deleted**. Every number in these documents comes
from the nine kept takes. Where a claim rests on the old data and this set does not re-measure
it, that is said outright and collected in
[07 §11](07-Reliability-Requirements.md#11-where-these-numbers-come-from-and-what-this-set-cannot-measure).

There is one CSV schema now. `slide_speed` is gone for good.

## The most important thing to know before reading any of it

**`recording.note` says what a take was *for*, not what it contains.** The run names each take
after its intent, and four separate times that intent did not survive contact with the game:

- takes 1–3 were meant to be a backward fall and are a forward faceplant, because the
  subject's package turns them around before the trigger;
- take 4 is named `slide` and is **not a slide at all** — it is an extreme push;
- takes 7, 8 and 9 were meant to record one ragdoll hitting another and all three missed by
  more than a metre;
- takes 5 and 6 were meant to be 3 m and 10 m drops and were bad enough to throw away.

So a take's name is a hypothesis about it. [01](01-Dataset-Map.md) records what each one
actually holds.

## Scripts

`Scripts/` is dependency-light (`numpy`, `pandas`) and never loads more than one take at a
time. Run them from that folder.

```
python _build.py            # writes Scripts/episodes.csv - every other script reads it
python 01_overview.py       # one line per take
python 02_armour.py         # what each actor was wearing, slot by slot
python 03_timing.py         # frame structure of the timestamps
python 04_dedup.py          # how many rows one real collision produces
python 05_episodes.py       # touch -> impacts -> separate, as the trigger unit
python 06_distributions.py  # by limb, site, material, layer, contact normal, scrape ratio
python 07_per_take.py       # per-take fingerprint against the run's own notes
python 08_anomalies.py      # where the numbers stop being trustworthy
python 09_comparisons.py    # armour / repeat-run / frame-rate comparisons
python 10_fps_bias.py       # does frame time bias impact magnitude
python 11_timeline.py       # event rate vs fps; what happens after the ragdoll ends
python 12_nonragdoll.py     # contacts from animated limbs, not ragdolls
python 13_knockdown_shape.py# the time axis of one knockdown
python 14_simultaneity.py   # how many sounds a naive trigger would want at once
python 15_metrics.py        # candidate loudness metrics
python 16_summary_numbers.py# the figures these docs quote
python 17_surfaces.py       # the ESM side: materials, impact sets, footstep surfaces
python 18_video_sync.py     # recover each clip's lead-in (needs ffmpeg)
```

`rlib.py` loads a take (CSV + YAML sidecar); `lib_events.py` reduces raw rows to collision
episodes. Everything else is a report.

## The one-paragraph answer

There is enough here to build a semi-physical impact sound system for the core case, and the
signal that matters most — a per-contact closing speed read out of the solver — is clean and
reproducible run to run. The run's real wins are narrower than its take list suggests: it
settled that **armour does not touch the physics** (three outfits, identical input, all
differences inside the noise), that **ragdoll mass is just the skeleton's table times the
actor's `scale`**, that **Havok's own spread on a byte-identical knockdown is ±13 % on the
median and ±20 % on the peak**, and that a runtime can **fire on the first contact of a
collision and be right 96 % of the time**. It also produced the first working tangential
measurement, though nothing in the set isolates a genuine scrape to validate it against.
What it did **not** deliver: the top of the intensity curve — both fall takes were discarded,
so the highest clean contact here is 794 units/s (11.3 m/s) from an extreme push and the
blow-up guard is still a guess; any natural ground surface; any character-on-character impact;
any get-up blend; any frame-rate contrast; and any acoustic ground truth. The raw rows
over-count real collisions by 2.5×. See [06](06-Gaps-and-Requested-Captures.md).
