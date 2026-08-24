#!/usr/bin/env python3
"""
make_sub.py -- synthesise the imp_sub layer.

No text-to-SFX model gives a clean pitched sweep into 30 Hz, so this layer is built rather
than prompted. The pitch curve comes straight off the measured sweeps in
04-Reference-Analysis.md section 1: the sub band of every reference hero hit starts at
110-185 Hz and falls into 40-65 Hz within 50-80 ms, reaching a 20-30 Hz floor by 180 ms,
with 8-15 dB of tonality (a pitched element, not filtered noise).

    python tools/make_sub.py [--out DIR]
"""

import argparse
import os
import subprocess
import wave

import numpy as np

SR = 48000  # render rate; --rate sets what ffmpeg resamples to, alongside the rest of the pack

# (variant, start Hz, mid Hz, floor Hz, length ms, drive)
# 01 is the default boom. 02 starts lower and sweeps slower so it reads bigger; it doubles
# as headroom for the runtime pitch scaling, which stays within +/-3 semitones.
VARIANTS = [
    (1, 150.0, 42.0, 26.0, 300.0, 4.5),
    (2, 110.0, 34.0, 22.0, 380.0, 5.0),
]


def sweep(start, mid, floor, ms, drive):
    n = int(SR * ms / 1000.0)
    t = np.arange(n) / SR
    dur = ms / 1000.0

    # Two exponential legs: a fast collapse, then a slow settle to the floor. Both are
    # exponential in frequency, which is what the octave-spaced measurements describe.
    # The collapse has to finish inside ~25 ms: the amplitude envelope is 20 dB down by
    # 40 ms, so if the pitch is still above 80 Hz while the sound is loud, the energy lands
    # in the low band instead of the sub band and the analysis has the sub band loudest.
    leg = min(0.025, dur * 0.12)
    f = np.empty(n)
    a = t < leg
    f[a] = start * (mid / start) ** (t[a] / leg)
    f[~a] = mid * (floor / mid) ** ((t[~a] - leg) / (dur - leg))

    # Integrate frequency to phase so the sweep is continuous and click-free.
    phase = 2 * np.pi * np.cumsum(f) / SR
    y = np.sin(phase)

    # Amplitude in two stages. The punch decays to -20 dB by 40 ms, matching the 16-52 ms
    # the references take. A quiet slow tail rides under it, which keeps the settled pitch
    # sounding long after the punch is gone -- without it the sweep smears across the band
    # and measures 4-7 dB of tonality against the references' 8-14.
    atk = int(SR * 0.003)
    punch = np.exp(-t * (20 / 8.686) / 0.040)
    tail = 0.10 * np.exp(-t * (20 / 8.686) / 0.260)
    env = punch + tail
    env[:atk] *= np.linspace(0, 1, atk)
    env *= np.cos(np.linspace(0, np.pi / 2, n)) ** 0.5   # settle the tail into silence
    y *= env

    # Soft saturation for 2nd and 3rd harmonic, so the boom survives on small speakers
    # where the fundamental is inaudible, then a gentle low-pass. The corner is deliberately
    # well above the fundamental: filtered down to a near-pure sine the layer has nothing
    # above 300 Hz, and stacking it under the transient and body drives the composite to
    # +29 dB of tilt against the references' +6.5 to +16.5. The harmonics are what let it
    # sit inside a broadband hit instead of underneath one.
    y = np.tanh(y * drive) / np.tanh(drive)
    F = np.fft.rfft(y)
    fr = np.fft.rfftfreq(n, 1.0 / SR) / 700.0
    F /= np.sqrt(1.0 + fr ** 4)
    y = np.fft.irfft(F, n)

    return y / max(np.abs(y).max(), 1e-9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--rate", type=int, default=44100,
                    help="output sample rate (default 44100; 48000 skips the downsample "
                         "and ships the native render)")
    args = ap.parse_args()
    out = args.out or os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "assets", "sfx")
    os.makedirs(out, exist_ok=True)

    for var, start, mid, floor, ms, drive in VARIANTS:
        y = sweep(start, mid, floor, ms, drive)
        final = os.path.join(out, f"imp_sub_{var:02d}.wav")
        tmp = final + ".tmp.wav"
        w = wave.open(tmp, "wb")
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((y * 32767 * 0.9).astype(np.int16).tobytes())
        w.close()
        # -1.0 dBFS: this is the loudest element in the mod. No high-pass.
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", tmp, "-af",
                        f"aresample={args.rate}:resampler=soxr:precision=28",
                        "-c:a", "pcm_s16le", final], check=True)
        # -1.0 dBFS after the resample, which can overshoot the pre-resample peak.
        w = wave.open(final, "rb")
        z = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16) / 32768.0
        w.close()
        gain = -1.0 - 20 * np.log10(max(np.abs(z).max(), 1e-9))
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", final, "-af",
                        f"volume={gain:.2f}dB", "-c:a", "pcm_s16le", tmp], check=True)
        os.replace(tmp, final)
        print(f"{os.path.basename(final)}  {ms:.0f} ms  {start:.0f} -> {floor:.0f} Hz")


if __name__ == "__main__":
    main()
