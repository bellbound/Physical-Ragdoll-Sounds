#!/usr/bin/env python3
"""
make_rumble.py -- synthesise the scrape_loop_rumble bed.

The mass under a slide: the floor being loaded by a body crossing it, with none of the
grit that rides on top. Built rather than prompted, for the same reason `imp_sub` is -
no text-to-SFX model gives a clean bottom octave, and every one of them answers a
prompt for "low rumble" with a designed sound that has an arc and a climax in it. A bed
must have neither. `02-SFX-Generation-Prompts.md` §1 says it outright: never trust the
model for the bottom octave.

The numbers come off the GTA 4 slide measurements in `temp/scrape-analysis-summary`:
their events are bass-led with the sub band the loudest band and a hard rolloff over
8 kHz, tilt +10 to +21, centroid 4.3-5.7 kHz for the *whole* event. Ours measured
-23 to -36 tilt with the sub 40 dB down, which is a hiss. This layer is the half that
was missing, so it is deliberately further down than the composite target: the grind
supplies the mid and the top, and the two together are what should land on GTA's
figures. Judged alone against `scrape_loop_rumble`'s own row in SfxAnalysis.cpp -
tilt over +20, centroid 20-1200 Hz, envelope 0.5-6 dB.

    python tools/make_rumble.py                       # the four pack variants
    python tools/make_rumble.py --only 02 03
    python tools/make_rumble.py --out takes/_beds --seconds 6

**The loop is seamless by construction, not by editing.** The noise is built in the
frequency domain at exactly the bin frequencies of the output buffer, so the inverse
transform is periodic with the buffer's own period - the last sample joins the first
with no discontinuity at all, and there is nothing to window, fold or crossfade. The
wobble is a sum of sinusoids at integer multiples of 1/length for the same reason, and
tanh saturation of a periodic signal is still periodic. That matters more here than
anywhere else in the bank: a bed has no grit to hide a seam under, and Slots.md's
warning that an audible seam becomes a rhythm in game is at its worst on a layer that
is nothing but low frequency.

No ffmpeg and no resample: 48 kHz is the pack rate and this renders there natively,
so there is no resampler overshoot to chase back down afterwards.
"""

import argparse
import os
import shutil
import wave

import numpy as np

SR = 48000  # the pack rate. Rendered here natively - nothing downsamples this file.

# (name, seconds, corner Hz, alpha, drive, wobble Hz, wobble depth, mode Hz, mode Q, mode dB)
#
# `corner` is where the lowpass knee sits and `alpha` is the tilt under it (1.0 is
# brown-ish, 0 is flat) - together they are how much mass against how much floor
# texture. `drive` is the tanh saturation, which is what puts 2nd and 3rd harmonics on
# a fundamental most speakers cannot reproduce; without it the bed is inaudible on a
# laptop and enormous on a subwoofer, which is the worst pair of failures available.
#
# `mode` is one narrow resonance: a real floor rings, faintly and at one or two
# frequencies decided by what it is made of, and it is the cheapest thing that makes a
# bed sound like a surface rather than like a filter. Kept faint - a resonance loud
# enough to notice is a pitch, and a pitch under a slide is a hum.
#
# **The lengths are deliberately unequal and deliberately not 2 s.** The grinds are
# 1.5-3 s and this rides under them on its own voice, so a bed whose length shared a
# common period with the grind's would put the two seams on top of each other every few
# seconds - which is audible as a pulse in a way either file alone is not. Prime-ish
# lengths, none of them a multiple of a grind's.
VARIANTS = [
    # 01 - the default bed. Middle of the range in every axis; what a body on
    # packed earth or flagstone should sit on.
    ("01", 3.07, 260.0, 1.00, 1.8, 4.5, 0.18, 58.0, 3.0, 2.5),
    # 02 - heavier and darker, with more of the bottom octave and a slower
    # wobble. A big body, or stone that carries further.
    ("02", 3.53, 200.0, 1.25, 2.4, 3.2, 0.22, 47.0, 3.5, 3.0),
    # 03 - hollower. Less tilt, a higher knee and a boxier mode: boards over a
    # void, where the floor answers back rather than absorbing.
    ("03", 3.29, 330.0, 0.85, 1.6, 5.5, 0.15, 118.0, 5.0, 3.5),
    # 04 - light and tight. Least mass of the four, for a limb-led slide or a
    # small body, and the one to reach for if the bed is reading as too much.
    ("04", 2.71, 430.0, 0.70, 1.4, 6.5, 0.12, 92.0, 4.0, 2.0),
]


def bed(seconds, corner, alpha, drive, wob_hz, wob_depth, mode_hz, mode_q, mode_db, seed):
    """One perfectly periodic bed of `seconds`, peak-normalised to 1.0."""
    n = int(round(SR * seconds))
    rng = np.random.default_rng(seed)

    # -- the noise, built as a spectrum so the result is periodic ------------
    #
    # Every bin is a whole number of cycles in the buffer by definition, so a
    # sum of them is too. Random phase and a shaped magnitude is the whole of it.
    freqs = np.fft.rfftfreq(n, 1.0 / SR)
    mag = np.zeros_like(freqs)
    f = freqs[1:]  # bin 0 is DC and stays at zero

    # The tilt, and the knee that ends it. A fourth-order knee rather than a
    # first: the point of this layer is that there is nothing above the shelf,
    # and a gentle rolloff leaves enough 1-3 kHz to read as noise rather than as
    # weight.
    shape = f ** (-alpha) / (1.0 + (f / corner) ** 4) ** 0.5

    # And a rolloff *under* the shelf, which matters as much. Energy below about
    # 25 Hz is inaudible on everything, eats all the headroom, and is what makes
    # a bed bloom on a subwoofer while vanishing on a laptop - so it is spent
    # rather than kept. Second order, from 22 Hz.
    hp = 22.0
    shape *= (f / hp) ** 2 / (1.0 + (f / hp) ** 2)

    # One faint floor mode. A real surface rings; a filter does not.
    if mode_db > 0.0:
        bw = mode_hz / max(0.5, mode_q)
        shape *= 1.0 + (10.0 ** (mode_db / 20.0) - 1.0) * np.exp(-0.5 * ((f - mode_hz) / bw) ** 2)

    mag[1:] = shape
    phase = rng.uniform(0.0, 2.0 * np.pi, mag.size)
    phase[0] = 0.0
    if n % 2 == 0:
        phase[-1] = 0.0  # Nyquist is real in an even-length transform
    y = np.fft.irfft(mag * np.exp(1j * phase), n)
    y /= max(np.abs(y).max(), 1e-9)

    # -- the wobble ---------------------------------------------------------
    #
    # A body crossing a floor is not a constant: the torso rocks over bumps at a
    # few hertz and the load on the surface goes with it. Measured at 3-8 Hz on
    # the references, and it is most of the difference between a bed and a tone.
    #
    # Quantised to whole cycles per buffer so it repeats with the noise. Three
    # partials at non-integer *ratios* to each other, so the envelope does not
    # settle into an obvious pulse inside the loop either.
    t = np.arange(n) / SR
    env = np.ones(n)
    for k, w in ((1.0, 1.0), (1.61, 0.5), (2.71, 0.28)):
        cycles = max(1.0, round(wob_hz * k * seconds))
        env += wob_depth * w * np.sin(2.0 * np.pi * cycles * t / seconds + rng.uniform(0, 6.28))
    y *= env / env.max()

    # -- saturation ---------------------------------------------------------
    #
    # 2nd and 3rd harmonic so the bed survives a speaker that cannot reproduce
    # its fundamental. The same trick and the same reason as `imp_sub`'s drive,
    # and the reason this layer is audible at all on anything but headphones.
    y = np.tanh(y * drive) / np.tanh(drive)

    # DC last, not first. Bin 0 was already zero, but the wobble multiply and the
    # tanh both act on a signal whose mean over a finite buffer is not exactly
    # zero, and a bed is the one layer where a few ten-thousandths of offset is
    # worth removing rather than reporting: it is all bottom octave, so an offset
    # is indistinguishable from signal to everything downstream and rides the
    # whole loop.
    y -= y.mean()

    return y / max(np.abs(y).max(), 1e-9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out",
                    help="where to write (default assets/sfx, the shipped pack, exactly as "
                         "make_sub.py does - and a copy is mirrored into assets/sfx/library "
                         "unless this is given, because the ini assigns library names)")
    ap.add_argument("--only", nargs="+", metavar="NN",
                    help="render only these variants, e.g. 02 03, so regenerating one "
                         "cannot rewrite a shipped file")
    ap.add_argument("--seconds", type=float,
                    help="override every variant's length, for auditioning a longer bed")
    ap.add_argument("--seed", type=int, default=20260826,
                    help="phase seed. Fixed by default so a re-render is byte-identical")
    ap.add_argument("--peak-db", type=float, default=-1.5,
                    help="output peak. -1.5 is the pack rule for everything but imp_sub")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # The pack, not the library, and for the same reason make_sub.py writes there:
    # this is a synthesised asset that ships with the mod, so it has to be in the
    # set an install gets with no ini at all. The library copy below is what
    # RagdollSounds_SFX.ini names and what the testbench's browser lists - the
    # pack files all appear in both, which is why `imp_sub_01.wav` is in each.
    pack = os.path.join(root, "assets", "sfx")
    out = args.out or pack
    os.makedirs(out, exist_ok=True)
    library = None if args.out else os.path.join(pack, "library")
    if library:
        os.makedirs(library, exist_ok=True)

    peak = 10.0 ** (args.peak_db / 20.0)
    for i, (name, secs, corner, alpha, drive, wh, wd, mh, mq, mdb) in enumerate(VARIANTS):
        if args.only and name not in args.only:
            continue
        secs = args.seconds or secs
        y = bed(secs, corner, alpha, drive, wh, wd, mh, mq, mdb, args.seed + i)
        path = os.path.join(out, f"scrape_loop_rumble_{name}.wav")
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes((y * peak * 32767).astype(np.int16).tobytes())

        # The seam, measured rather than asserted: the last sample against the
        # first, which is what whole-file looping actually joins.
        if library:
            shutil.copy2(path, os.path.join(library, os.path.basename(path)))

        seam = 20.0 * np.log10(max(abs(y[-1] - y[0]), 1e-9) / max(np.abs(y).max(), 1e-9))
        print(f"{os.path.basename(path)}  {secs:.2f}s  knee {corner:.0f} Hz  "
              f"alpha {alpha:.2f}  drive {drive:.1f}  wobble {wh:.1f} Hz  "
              f"mode {mh:.0f} Hz  seam {seam:.1f} dB")


if __name__ == "__main__":
    main()
