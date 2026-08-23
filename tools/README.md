# tools/sfx.py

Judge and post-process generated SFX takes. Specs live in `SPEC` at the top of the script and
come from `02-SFX-Generation-Prompts.md`, which derives them from `01-Reference-Analysis.md`.

```
python tools/sfx.py eval C:/Users/mika/Downloads --suggest
python tools/sfx.py make <take.wav> --slot imp_transient --var 1
```

## eval

Drop a batch of generations in a folder and point `eval` at it. Slots are inferred from the
filename — either `<slot>_NN.wav`, or the generator's prompt-prefix name (`sharp_dry_slap_of_a__#2`
→ `imp_transient`), so a mixed folder needs no flags.

It finds the **event inside** the file and judges that, not the raw container:

| level | meaning |
|---|---|
| `FIX` | the post-pass handles it — sample rate, stereo, pre-roll, level, over-length. Never sinks a take |
| `WARN` | audible risk, usually a second contact landing on `imp_sub`'s +55–75 ms arrival |
| `FAIL` | wrong character. Regenerate |

Verdict is `KEEP`, `KEEP*` (keep, but read the warning) or `RETRY`.

Character is measured **after the slot's high-pass**, because that is what ships. This matters:
spectral centroid alone cannot tell a bright contact from a bass-led thud — there are simply more
FFT bins up top, the same trap `01-Reference-Analysis.md` §7 flags for the scrape. Band *tilt* and
`hpfloss` (how much peak level the take gives up to its high-pass) are the honest discriminators.

`--suggest` names any other slot a `RETRY` take would pass as, so a take that missed its prompt's
slot can still be used. `--detail` adds band tables and notes on passing takes; `--csv` dumps the
table.

## Keeping takes

`--archive` copies every `KEEP` take into `takes/<slot>/` and records its metrics in
`takes/ledger.csv`. Keyed on content hash, so re-running over the same download folder never
duplicates a take and byte-identical regenerations are reported rather than filed twice. Built
pack files are skipped — they are outputs, not source material.

`make` then stamps the ledger's `used_as` column with the pack file a take became, so every
shipped asset traces back to the take and the metrics it was chosen on. Rows with an empty
`used_as` are spares: takes that passed but lost the pick.

## make

Trims to the detected attack on a zero crossing, follows the event's own decay (capped to the slot
maximum) so nothing drags room tone behind it, fades 6 ms, applies the slot high-pass as a 2-pole
minimum-phase filter via ffmpeg, resamples to 44.1 kHz with soxr, mono-sums and normalises to the
slot's target peak. Writes `assets/sfx/<slot>_NN.wav`.

Override the cut with `--start MS` / `--len MS` when the detector picks the wrong event.

## split

`split` cuts a multi-contact take into separate one-shots. Generators asked for a "tap" or "scuff"
routinely return four to nine contacts in one file; each is a usable `limb_tap`, so splitting turns
one take into several rather than throwing it away.

Onsets must be **46 ms apart** -- the reference rate floor from `01-Reference-Analysis.md` §2, below
which two contacts stop resolving as separate events. Two contacts inside that window are one onset,
and the louder wins. Pieces keep the generator's prompt prefix in their name so slot inference still
works, and land in `takes/_split` for `eval --archive` to judge and file.

# make_sub.py

Synthesises `imp_sub_01/02`. No text-to-SFX model gives a clean pitched sweep into 30 Hz, so this
layer is built from the measured curves in `01-Reference-Analysis.md` §1 rather than prompted.
Verified against the references: dominant frequency descent, sub-band dominance, 8-14 dB tonality,
and 16-52 ms to −20 dB.

# preview.py

Renders the impact composite — transient at 0 ms, body at +20, sub at +65 — into
`assets/preview/composite_NN.wav`, so the stack can be judged as one sound. This is the check the
whole asset list exists to serve: every layer can pass on its own and still not fuse. Layer gains
are solved against the mean band curve of the four reference hero hits.

# build_pack.py

Rebuilds the 16 files added on 2026-08-22 from their chosen takes, at 48 kHz. Each pick is a KEEP
under `eval`; two `crunch_gran` picks are shelved through ffmpeg first, into `takes/_shelf/`, so
`make` still archives the source and the ledger provenance reads. Point it at the folder holding
the generated takes:

```
python tools/build_pack.py <gen_root>
```

# verify_pack.py

Checks the delivery rules the slot spec does not: mono/48 kHz/16-bit, no lead-in silence, length
inside the slot maximum, level matched across a slot's variants, headroom under the runtime's ±3
semitone shift, and loop seams. Run it on the pack before shipping.

```
python tools/verify_pack.py assets/sfx
```

Two of its thresholds are deliberately not what they first look like, and `03-Asset-Status.md` §6
records why: lead-in is measured against an absolute floor rather than the peak (`settle_rest` is
specified to start softly), and there is no threshold on the −40 dB tail (that would flag
`imp_sub`'s specified decay and the body layers' house decay — the real rule is slot length).

# to48k.py

Resamples already-built pack files to 48 kHz in place and renormalises to the slot's peak target.
For files built before the pack moved to 48 kHz. Resampling rather than rebuilding is deliberate:
several early files were cut or EQ'd with `make` overrides the ledger does not record, so a rebuild
from source silently changes them.

# Adding a slot

Add an entry to `SPEC` and, if it has a prompt, its first 20 prompt characters to `PROMPT_HEADS`.
Requires numpy and ffmpeg on PATH; no scipy.
