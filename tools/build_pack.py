#!/usr/bin/env python3
"""Build the 16 files added on 2026-08-22 from their chosen takes, at 48 kHz.

Every pick here is a KEEP under `sfx.py eval`. Two of them are shelved before `make`:
crunch_gran is the one slot no prompt would dull -- 30 takes across four prompt revisions
all measured 5-6.7 kHz centroid against a 4500 Hz ceiling, while the reference spine break
sits at 2705. The lomid transients were there all along (16-63 of them); it was the high
and air bands riding on top that moved the centroid. 00-Design.md wants this slot
"concentrated 300 Hz - 2 kHz", so the shelf is the brief rather than a workaround. Shelved
sources are written to takes/_shelf/ under their original name, the way `split` uses
takes/_split/, so `make` archives them and the ledger provenance still reads.

    python tools/build_pack.py <gen_root>
"""
import os, subprocess, sys

RATE = 48000
GEN = sys.argv[1] if len(sys.argv) > 1 else "."
SHELF_DIR = "takes/_shelf"

# slot, var, (gen folder | repo path), filename, shelf dB, tilt_eq dB, start offset past attack
PICKS = [
    # start 3400: `make` picks a loop window on steadiness, which left a 3.9 dB seam on a
    # file the engine repeats whole with no crossfade. Sweeping the start for the seam
    # instead lands 0.02 dB at 24 grains/s, against the reference slide's 17.
    ("scrape_loop", 1, "gen",  "heavy_limp_body_drag_#1-1787431225038.wav", None, 0, 3400),
    ("air_whoosh",  1, "gen2", "low_soft_air_movemen_#3-1787431424424.wav", None, 0, None),
    ("settle_rest", 1, "gen",  "loose_fabric_and_a_h_#1-1787431256733.wav", None, 0, None),
    ("settle_rest", 2, "gen2", "heavy_limp_body_sett_#3-1787431488323.wav", None, 0, None),
    ("surf_soft",   1, None,   "takes/surf_soft/dull_muffled_thump_i_#2-1787424328067.wav", None, 0, None),
    ("surf_soft",   2, "gen",  "heavy_weight_landing_#2-1787431264753.wav", -4, 0, None),
    ("surf_wood",   1, None,   "takes/surf_wood/single_hollow_knock__#3-1787423681358.wav", None, 0, None),
    ("surf_wood",   2, None,   "takes/surf_wood/heavy_thud_on_old_cr_#2-1787423749302.wav", None, 0, None),
    ("surf_stone",  1, "gen",  "heavy_object_strikin_#3-1787431277670.wav", None, 0, None),
    ("surf_stone",  2, "gen",  "heavy_object_strikin_#1-1787431274058.wav", None, 0, None),
    ("head_impact", 1, "gen",  "heavy_melon_wrapped__#1-1787431288865.wav", None, 0, None),
    ("head_impact", 2, "gen2", "hard_blunt_blow_to_a_#2-1787431471675.wav", None, 0, None),
    ("crunch_gran", 1, "gen2", "slow_crushing_of_a_b_#3-1787431435369.wav", -18, 4, None),
    ("crunch_gran", 2, "gen2", "twisting_a_handful_o_#4-1787431448657.wav", -12, 4, 80),
    ("gore_wet",    1, "gen3", "wet_squelch_of_raw_m_#2-1787431576168.wav", None, 0, None),
    ("gore_wet",    2, "gen3", "heavy_wet_slap_of_so_#1-1787431585147.wav", None, 0, None),
]

sys.path.insert(0, "tools")
import sfx

os.makedirs(SHELF_DIR, exist_ok=True)
for slot, var, folder, name, shelf, eq, start in PICKS:
    src = name if folder is None else os.path.join(GEN, folder, name)
    if shelf:
        shelved = os.path.join(SHELF_DIR, os.path.basename(name))
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", src, "-af",
                        f"highshelf=f=2500:g={shelf}:t=q:w=0.7", "-c:a", "pcm_s16le", shelved],
                       check=True)
        src = shelved
    cmd = [sys.executable, "tools/sfx.py", "make", src, "--slot", slot, "--var", str(var),
           "--rate", str(RATE)]
    if eq:
        cmd += ["--tilt-eq", str(eq)]
    if start is not None:
        cmd += ["--start", str(sfx.measure(src, slot)["attack"] + start)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(f"FAIL {slot}_{var:02d}: {(r.stderr or r.stdout).strip()[:200]}")
        continue
    print(f"built {slot}_{var:02d}  <- {os.path.basename(name)}"
          + (f"  [shelf {shelf} dB @2.5k]" if shelf else "")
          + (f"  [bell +{eq}]" if eq else ""))
