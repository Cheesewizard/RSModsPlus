#!/usr/bin/env python3
"""Physically-parameterised electric-guitar note generator for the NBN detector.

Purpose
-------
The headline open problem for the string/fret model is *same-pitch-different-
position* disambiguation (Riff 4 in the ML test song, the #66 metric): E4 played
on the open high-e is the same fundamental as E4 fretted at B-string 5, G-string 9,
D-string 14 or A-string 19, and the model has to tell them apart from timbre alone.

Real recordings (GuitarSet) give you that signal tangled up with room, player and
mic variance. This tool generates it *clean and labelled*: every clip differs from
its same-pitch siblings ONLY in the physics that actually depends on fretboard
position, with perfect ground-truth string/fret/onset labels and no recording step.

What actually differs by position (the signal the model can learn)
------------------------------------------------------------------
For a fixed pitch, the two knobs that move with position are both physical and
both deterministic:

1. Inharmonicity. A real string is stiff, so partials stretch sharp:
       f_n = n * f0 * sqrt(1 + B * n^2)
   B is the inharmonicity coefficient. B grows with string thickness (B ~ d^4)
   and shrinks with vibrating length (B ~ 1/L^2). Fretting to fret f shortens the
   string by 2^(-f/12), so
       B(f) = B_open * 2^(f/6).
   Same pitch on a thicker string fretted higher therefore stacks BOTH effects:
   a bigger B_open and a bigger 2^(f/6) multiplier. The partials of the low-string
   realisation are audibly, measurably more stretched than the open thin-string one.

2. Pluck-position comb. The pick sits a fixed distance from the bridge, but the
   vibrating length changes with fret, so the pluck point as a *fraction* of the
   string, p = pick_dist/L, rises with fret. The plucked-string spectrum has notches
   at harmonic numbers n = 1/p, 2/p, ..., so those notches slide down to lower
   partials as you fret higher. Different comb -> different timbre for the same pitch.

Plus fixed per-string character: wound low strings roll their highs off faster than
plain trebles (a spectral tilt), which further separates the realisations.

Why additive synthesis and not a waveguide / Karplus-Strong
-----------------------------------------------------------
A digital waveguide is the right model for *playing* a string in real time, but for
generating a labelled corpus it is the wrong tool: tuning a waveguide's loop/allpass
filters to hit an exact target B is fiddly and only approximate. Additive synthesis
lets us place every partial at exactly f_n(B) with exactly the amplitude/decay the
physics dictates, so the discriminating cue is under direct, exact control and the
labels are exact by construction. The stiff-string partial series IS the steady-state
spectrum a waveguide converges to; we just write it down directly.

This is a DI (direct) signal. The amp/cab nonlinearity is deliberately NOT modelled
here -- that is the one part of the chain where a neural amp model (NAM/GuitarML)
beats analytic modelling, and it would be applied as a post stage if the corpus ever
needs amp colour. For a detection front end (clean-tone test song, CQT input) the DI
spectrum is what you want.

Dependencies: numpy only (WAV I/O via the stdlib `wave` module).

Usage
-----
  # Show the discriminating physics for every position that produces a pitch:
  python waveguide_guitar.py analyze --midi 64

  # Render every realisation of one pitch to WAVs you can listen to / spectrogram:
  python waveguide_guitar.py demo --midi 64 --out ./wg_demo

  # Build a labelled same-pitch corpus across a pitch range for training/eval:
  python waveguide_guitar.py corpus --lo 52 --hi 76 --reps 4 --out ./wg_corpus

Corpus format (per clip): `<id>.wav` (mono, 22050 Hz, float mirrored to int16) plus
`<id>.notes.json` = {"sr":22050,"notes":[{"string":s,"fret":f,"onset":t0,"offset":t1}]}
using the project's string order (0 = low E2 .. 5 = high e4) and fret 0..19. A
`manifest.json` lists every clip. See the adapter note at the bottom of this file for
the ~15 lines that turn a clip into (tiles, tab, dev, mask, onset) via the existing
ml-training front end.
"""

import argparse
import json
import os
import wave

import numpy as np

# ---------------------------------------------------------------------------
# Project conventions (kept in sync with tools/ml-training/src/constants.py)
# ---------------------------------------------------------------------------
SR = 22050                                   # TabCNN/FretNet operating rate
OPEN_STRING_MIDI = [40, 45, 50, 55, 59, 64]  # 0 = low E2 .. 5 = high e4
NUM_STRINGS = 6
MAX_FRET = 19                                # model range is 0..19
STRING_NAMES = ["E2", "A2", "D3", "G3", "B3", "e4"]

MIDI_A4 = 69
FREQ_A4 = 440.0


def midi_to_hz(m):
    return FREQ_A4 * 2.0 ** ((m - MIDI_A4) / 12.0)


# ---------------------------------------------------------------------------
# Per-string physical character
# ---------------------------------------------------------------------------
# B_OPEN: inharmonicity coefficient of each OPEN string. These sit in the range
# measured on real electric strings (~1e-4 to a few e-4); plain strings are more
# inharmonic than wound ones of similar diameter because a wound string's thin
# steel core carries the stiffness while the winding adds only mass. The plain G
# (.017) is famously the most inharmonic string on a light set, which is why it is
# the largest value here. These are the primary tunable knobs -- adjust to match a
# measured spectrum from the real rig if you want tighter domain match.
#                     E2      A2      D3      G3      B3      e4
#                   (.046w) (.036w) (.026w) (.017p) (.013p) (.010p)
B_OPEN = np.array([1.0e-4, 1.1e-4, 1.4e-4, 3.2e-4, 2.2e-4, 1.6e-4])

# BRIGHTNESS: exp spectral-tilt constant. Larger = brighter (more high partials).
# Wound low strings are warmer (smaller) than plain trebles.
BRIGHTNESS = np.array([9.0, 10.0, 12.0, 18.0, 22.0, 26.0])

# Base decay time constant (s) of the fundamental, per string. Low wound strings
# ring longer than thin trebles.
TAU0 = np.array([2.8, 2.6, 2.3, 1.9, 1.6, 1.4])

# Scale length and a nominal pick distance from the bridge, in the same unit (mm).
# Only the ratio matters. ~160 mm from the bridge on a 648 mm (25.5") scale is a
# typical picking spot; p_open = pick_dist / scale_len.
SCALE_LEN_MM = 648.0
PICK_DIST_MM = 160.0

# Frequency-dependent damping: higher partials die faster (air + internal losses,
# ~ f^2). Coefficient chosen so a 4 kHz partial loses ~1 extra 1/e per ~0.4 s.
HF_DAMP = 6.0e-7          # extra decay rate (1/s) per (Hz)^2
N_MAX = 64               # partials to consider before the Nyquist cull


def inharmonicity(string_idx, fret):
    """B(f) = B_open * 2^(f/6): fretting shortens L, and B ~ 1/L^2."""
    return B_OPEN[string_idx] * 2.0 ** (fret / 6.0)


def pluck_fraction(fret):
    """Pick point as a fraction of the *current* vibrating length. The pick is a
    fixed distance from the bridge; fretting shortens L by 2^(-f/12), so the
    fraction grows with fret and the comb notches slide to lower partials."""
    L = SCALE_LEN_MM * 2.0 ** (-fret / 12.0)
    return min(0.49, PICK_DIST_MM / L)


# ---------------------------------------------------------------------------
# Synthesis
# ---------------------------------------------------------------------------
def stiff_partials(f0, B):
    """Return (n, f_n) for partials below 0.45*SR under stiff-string stretching."""
    n = np.arange(1, N_MAX + 1, dtype=np.float64)
    fn = n * f0 * np.sqrt(1.0 + B * n * n)
    keep = fn < 0.45 * SR
    return n[keep], fn[keep]


def partial_amplitudes(n, string_idx, p):
    """Initial amplitude of each partial: ideal plucked-string 1/n^2 rolloff,
    times the pluck-position comb |sin(n*pi*p)|, times a per-string spectral tilt."""
    comb = np.abs(np.sin(np.pi * n * p))
    rolloff = 1.0 / (n * n)
    tilt = np.exp(-n / BRIGHTNESS[string_idx])
    a = comb * rolloff * tilt
    return a


def _common_modulation(t, rng):
    """The pitch life the whole string shares. Returns (mphase, amp_trem) where
    mphase is the accumulated phase of a unit-frequency oscillator whose
    instantaneous frequency is the multiplicative cents deviation, so partial i's
    instantaneous phase is simply fn_i * mphase. Two ingredients:

      * onset glide -- a hard pluck raises tension, so the note starts a few cents
        sharp and settles over ~150 ms (exponential decay to 0).
      * slow wander -- a sum of a few low-freq LFOs (~0.5..7 Hz) giving a couple of
        cents of continuous, non-repeating pitch drift. This is the dominant
        'alive' cue; a pure decaying-sinusoid stack sounds dead without it.

    amp_trem is a gentle (~few %) tremolo correlated with the slow wander (bridge/
    body coupling amplitude-modulates as the string pitch drifts)."""
    glide_cents = rng.uniform(5.0, 11.0) * np.exp(-t / rng.uniform(0.06, 0.12))

    wander = np.zeros_like(t)
    trem = np.zeros_like(t)
    for _ in range(4):
        f = rng.uniform(0.4, 7.0)
        a = rng.uniform(0.4, 1.0) / f            # 1/f: slower drifts dominate
        ph = rng.uniform(0.0, 2.0 * np.pi)
        wander += a * np.sin(2.0 * np.pi * f * t + ph)
        trem += a * np.sin(2.0 * np.pi * f * t + ph + rng.uniform(0, 1))
    wander *= 2.6 / (np.std(wander) or 1.0)      # ~2.6 cents RMS drift
    trem *= 0.04 / (np.std(trem) or 1.0)         # ~4% RMS tremolo

    cents = glide_cents + wander
    mod = 2.0 ** (cents / 1200.0)                # multiplicative freq factor ~1.0
    mphase = (2.0 * np.pi / SR) * np.cumsum(mod)
    return mphase, (1.0 + trem)


def render_note_additive(string_idx, fret, dur=1.6, velocity=1.0, seed=None):
    """Synthesise one plucked note as a float32 mono waveform at SR.

    Realism model (why it does not sound like an organ):
      * common pitch wander + onset glide shared by every partial (_common_modulation)
      * per-partial polarisation beating with an INDEPENDENT beat rate (~1..4 Hz) and
        independent phase per partial -- the two orthogonal string polarisations of
        each mode beat at their own rate, so the shimmer is incoherent across
        partials (a single synchronised detune sounds like chorus, not a string)
      * per-partial frequency-dependent decay (highs die first) with a slightly
        longer-lived twin so energy sloshes into an aftersound
      * a pick-attack noise burst and a faint enveloped air bed above the noise floor
    """
    rng = np.random.default_rng(seed)
    f0 = midi_to_hz(OPEN_STRING_MIDI[string_idx] + fret)
    B = inharmonicity(string_idx, fret)
    p = pluck_fraction(fret)

    n, fn = stiff_partials(f0, B)
    amp = partial_amplitudes(n, string_idx, p)

    t = np.arange(int(dur * SR)) / SR
    mphase, amp_trem = _common_modulation(t, rng)

    decay = 1.0 / TAU0[string_idx] + HF_DAMP * fn * fn
    phase = rng.uniform(0.0, 2.0 * np.pi, size=fn.shape)
    phase2 = rng.uniform(0.0, 2.0 * np.pi, size=fn.shape)
    beat_hz = rng.uniform(1.0, 4.0, size=fn.shape)     # per-partial beat rate
    twin_gain = rng.uniform(0.5, 0.85, size=fn.shape)

    out = np.zeros_like(t)
    for i in range(fn.shape[0]):
        env = amp[i] * np.exp(-decay[i] * t)
        # main polarisation rides the shared pitch modulation
        out += env * np.sin(fn[i] * mphase + phase[i])
        # second polarisation: offset by beat_hz, own phase, slightly longer decay
        env2 = twin_gain[i] * amp[i] * np.exp(-0.85 * decay[i] * t)
        out += env2 * np.sin(fn[i] * mphase + 2.0 * np.pi * beat_hz[i] * t + phase2[i])
    out *= amp_trem

    # Pick-attack transient: short shaped noise burst gives the onset head an edge.
    atk_len = int(0.006 * SR)
    burst = rng.standard_normal(atk_len) * np.exp(-np.arange(atk_len) / (0.0015 * SR))
    out[:atk_len] += 0.12 * burst
    # Faint enveloped air bed so the tail is not a mathematically dead sinusoid stack.
    air = rng.standard_normal(t.shape) * np.exp(-t / (0.5 * TAU0[string_idx]))
    out += 0.004 * air

    peak = float(np.max(np.abs(out))) or 1.0
    out = (out / peak) * (0.9 * velocity)
    return out.astype(np.float32)


# ---------------------------------------------------------------------------
# Karplus-Strong / waveguide engine (the one that actually sounds plucked)
# ---------------------------------------------------------------------------
# The additive engine places ideal partials with a smooth 1/n^2 envelope and fakes
# the excitation; that is why it reads as synthetic. A digital waveguide instead
# excites a tuned feedback delay line with a real noise burst, so the attack
# clangor, the non-exponential decay and the beating all emerge from the physics.
# We add two things a bare Karplus-Strong lacks and a guitar needs: a first-order
# stiffness allpass in the loop (dispersion -> inharmonicity, scaled by the SAME
# thickness*fret trend the additive B uses, so the position cue survives) and a
# post pickup/body colour filter (the jagged, resonant spectral envelope whose
# absence is the other synthetic tell).

# Loop damping coefficient per string: larger = darker, highs decay faster.
# Wound low strings are darker than plain trebles.
LOOP_DAMP = np.array([0.42, 0.40, 0.36, 0.30, 0.26, 0.22])
LOOP_LOSS = 0.9992          # per-sample loop gain (<1); overall sustain length


def _lowpass_noise(n, rng, cutoff_hz=7.0):
    """Band-limited random drift in [-1, 1]-ish: white noise through a one-pole,
    then normalised. Irregular, non-repeating -- unlike summed sinusoids, which is
    why the additive warble felt 'too perfect'."""
    a = np.exp(-2.0 * np.pi * cutoff_hz / SR)
    w = rng.standard_normal(n)
    out = np.empty(n)
    z = 0.0
    for i in range(n):
        z = (1 - a) * w[i] + a * z
        out[i] = z
    s = np.std(out) or 1.0
    return out / s


def _pickup_body_color(y, rng):
    """Impose a resonant, jagged spectral envelope: a pickup resonant peak with a
    steep top-end rolloff, plus a few low body/instrument resonances. Applied in the
    frequency domain (linear, offline). This is what stops the spectrum being a
    smooth curve, the single biggest 'not a real instrument' cue."""
    n = len(y)
    Y = np.fft.rfft(y)
    f = np.fft.rfftfreq(n, 1.0 / SR)
    H = np.ones_like(f)

    if CALIB_PICKUP_PEAK:                           # fitted from a real DI capture
        fp, Qp = CALIB_PICKUP_PEAK * rng.uniform(0.97, 1.03), 1.4
    else:
        fp, Qp = rng.uniform(2600, 3800), 1.4       # pickup resonant peak (guessed)
    xp = f / fp
    peak = 1.0 / np.sqrt((1 - xp ** 2) ** 2 + (xp / Qp) ** 2)
    H *= 1.0 + 1.8 * peak / peak.max()
    H *= 1.0 / np.sqrt(1.0 + (f / 5200.0) ** 4)    # pickup high rolloff

    for _ in range(3):                             # body / instrument resonances
        fb, Qb, g = rng.uniform(90, 260), rng.uniform(7, 14), rng.uniform(0.25, 0.7)
        xb = f / fb
        H += g / np.sqrt((1 - xb ** 2) ** 2 + (xb / Qb) ** 2) / Qb
    H *= 1.0 / np.sqrt(1.0 + (30.0 / np.maximum(f, 1.0)) ** 4)  # sub rumble cut

    return np.fft.irfft(Y * H, n)


def render_note_ks(string_idx, fret, dur=1.6, velocity=1.0, seed=None):
    """Extended Karplus-Strong pluck. Sequential feedback loop (not vectorisable),
    so it is ~10-50x slower than the additive engine but sounds like a real string."""
    rng = np.random.default_rng(seed)
    f0 = midi_to_hz(OPEN_STRING_MIDI[string_idx] + fret)
    N = int(dur * SR)
    D = SR / f0                                    # base loop delay (fractional)

    # stiffness -> allpass coefficient. Same thickness*fret trend as additive B, so
    # the low-string/high-fret realisation is more inharmonic here too.
    stiff = inharmonicity(string_idx, fret) / 1e-4
    c_ap = -min(0.30, 0.03 * stiff)                # negative -> partials stretch sharp

    # excitation: one-wavelength noise burst, comb-filtered by pluck position.
    exc_len = int(np.ceil(D))
    exc = np.zeros(N)
    burst = rng.standard_normal(exc_len)
    pd = max(1, int(round(pluck_fraction(fret) * exc_len)))
    burst[pd:] -= burst[:-pd]                       # pluck-position comb
    burst *= np.hanning(exc_len) * 0.5 + 0.5        # soften the very edges
    exc[:exc_len] = burst

    # delay modulation: onset glide (starts sharp, settles) + irregular slow drift.
    t = np.arange(N) / SR
    glide = -rng.uniform(0.004, 0.009) * np.exp(-t / rng.uniform(0.06, 0.12))
    drift = 0.0016 * _lowpass_noise(N, rng, cutoff_hz=6.0)   # ~2.7 cents RMS, irregular
    dmod = D * (1.0 + glide + drift)

    CB = 1
    while CB < D + 8:
        CB <<= 1
    cb = np.zeros(CB)
    out = np.zeros(N)
    a = LOOP_DAMP[string_idx]
    lp_prev = 0.0
    x_prev = 0.0
    y_prev = 0.0
    w = 0
    for i in range(N):
        d = dmod[i]
        rp = w - d
        i0 = int(np.floor(rp))
        frac = rp - i0
        s = cb[i0 & (CB - 1)] * (1.0 - frac) + cb[(i0 + 1) & (CB - 1)] * frac
        lp = (1.0 - a) * s + a * lp_prev            # loop damping one-pole
        lp_prev = lp
        ap = c_ap * lp + x_prev - c_ap * y_prev     # stiffness allpass (dispersion)
        x_prev = lp
        y_prev = ap
        val = LOOP_LOSS * ap + exc[i]               # loss + pluck excitation
        cb[w & (CB - 1)] = val
        out[i] = val
        w += 1

    out = _pickup_body_color(out, rng)
    peak = float(np.max(np.abs(out))) or 1.0
    out = (out / peak) * (0.9 * velocity)
    return out.astype(np.float32)


DEFAULT_ENGINE = "ks"


def render_note(string_idx, fret, dur=1.6, velocity=1.0, seed=None,
                engine=DEFAULT_ENGINE):
    """Dispatch to the chosen engine.
      'ks'       -- Karplus-Strong waveguide: sounds like a real pluck, inharmonicity
                    is a physically-scaled trend (not an exact B). Use for listening
                    and for realistic-timbre training data.
      'additive' -- exact stiff-string partials with exact B and comb: less realistic
                    but the discriminating physics is numerically exact. Use when the
                    label/cue must be precise (e.g. the analyze table's numbers)."""
    if engine == "additive":
        return render_note_additive(string_idx, fret, dur, velocity, seed)
    return render_note_ks(string_idx, fret, dur, velocity, seed)


def render_sequence(events, gap=0.15, tail=0.4, engine=DEFAULT_ENGINE):
    """events: list of (string_idx, fret, dur). Returns (waveform, notes) where
    notes is [{string,fret,onset,offset}] with real onset/offset times in seconds."""
    chunks, notes, cursor = [], [], 0.0
    for (s, f, dur) in events:
        w = render_note(s, f, dur=dur, engine=engine)
        onset = cursor
        chunks.append(w)
        offset = onset + len(w) / SR
        notes.append({"string": s, "fret": f,
                      "onset": round(onset, 4), "offset": round(offset, 4)})
        cursor = offset + gap
        chunks.append(np.zeros(int(gap * SR), dtype=np.float32))
    chunks.append(np.zeros(int(tail * SR), dtype=np.float32))
    return np.concatenate(chunks), notes


# ---------------------------------------------------------------------------
# Position enumeration
# ---------------------------------------------------------------------------
def positions_for_midi(m):
    """Every (string_idx, fret) in range 0..MAX_FRET that sounds MIDI note m,
    low string first."""
    out = []
    for s in range(NUM_STRINGS):
        fret = m - OPEN_STRING_MIDI[s]
        if 0 <= fret <= MAX_FRET:
            out.append((s, fret))
    return out


# ---------------------------------------------------------------------------
# WAV I/O (stdlib, no soundfile dependency)
# ---------------------------------------------------------------------------
def write_wav(path, y):
    y16 = np.clip(y, -1.0, 1.0)
    y16 = (y16 * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(y16.tobytes())


def read_wav(path):
    """-> (float32 mono in [-1,1], sr). Handles 16/32-bit PCM via stdlib wave."""
    with wave.open(path, "rb") as w:
        sr = w.getframerate()
        nch = w.getnchannels()
        sw = w.getsampwidth()
        raw = w.readframes(w.getnframes())
    dt = {1: np.int8, 2: "<i2", 4: "<i4"}.get(sw)
    if dt is None:
        raise ValueError(f"unsupported sample width {sw} in {path}")
    y = np.frombuffer(raw, dtype=dt).astype(np.float64)
    y /= float(2 ** (8 * sw - 1))
    if nch > 1:
        y = y.reshape(-1, nch).mean(axis=1)
    return y.astype(np.float32), sr


# ---------------------------------------------------------------------------
# Calibration: fit per-string constants from a dry recorded note
# ---------------------------------------------------------------------------
# Applied globals, overwritten by apply_calibration(). None = use the guessed
# default pickup peak (a random range) instead of a fitted one.
CALIB_PICKUP_PEAK = None


def _sustain_window(y, sr):
    """Return the steady-state chunk: skip the first 60 ms (attack) and take up to
    ~0.8 s where the note is loudest, so f0/partial estimates are stable."""
    a = int(0.06 * sr)
    seg = y[a:a + int(0.8 * sr)]
    if seg.size < sr // 8:
        seg = y[a:] if y.size > a else y
    return seg * np.hanning(len(seg))


def estimate_f0(y, sr, fmin=70.0, fmax=1300.0):
    """FFT-based autocorrelation f0 with parabolic refinement."""
    y = y - y.mean()
    n = len(y)
    Y = np.fft.rfft(y, 2 * n)
    ac = np.fft.irfft(Y * np.conj(Y))[:n]
    lo, hi = int(sr / fmax), min(n - 2, int(sr / fmin))
    if hi <= lo + 1:
        return 0.0
    k = lo + int(np.argmax(ac[lo:hi]))
    a, b, c = ac[k - 1], ac[k], ac[k + 1]
    denom = a - 2 * b + c
    shift = 0.5 * (a - c) / denom if denom != 0 else 0.0
    return sr / (k + shift)


def segment_notes(y, sr, thr_frac=0.12, min_dur=0.4, bridge=0.15):
    """Split a recording into individual notes by an RMS gate. Returns a list of
    (start, end) sample indices for each sounded region longer than `min_dur`,
    bridging silences shorter than `bridge` so one note is not cut into pieces.
    Handles an all-in-one take (every open string played once or twice into a
    single file), which is the natural way to record the calibration."""
    win = max(1, int(0.03 * sr))
    env = np.sqrt(np.convolve(y.astype(np.float64) ** 2,
                              np.ones(win) / win, mode="same"))
    above = env > thr_frac * (env.max() or 1.0)
    bridge_n = int(bridge * sr)
    min_n = int(min_dur * sr)
    notes, n, i = [], len(y), 0
    while i < n:
        if not above[i]:
            i += 1
            continue
        j = i + 1
        gap = 0
        while j < n and (above[j] or gap < bridge_n):
            gap = 0 if above[j] else gap + 1
            j += 1
        end = j - gap
        if end - i >= min_n:
            notes.append((i, end))
        i = j
    return notes


def estimate_note_params(y, sr):
    """Fit (f0, B, brightness, pickup_peak_hz) from one dry plucked note.

      B          -- inharmonicity: fit (f_n/(n f0))^2 - 1 = B n^2 over located partials.
      brightness -- spectral tilt: fit a_n * n^2 ~ exp(-n / brightness).
      pickup_peak-- frequency of the broad spectral-envelope maximum above 800 Hz.
    """
    seg = _sustain_window(y, sr)
    f0 = estimate_f0(seg, sr)
    if f0 <= 0:
        return None

    win = seg * np.hanning(len(seg))  # (already hanned once; extra taper is harmless)
    mag = np.abs(np.fft.rfft(win))
    freqs = np.fft.rfftfreq(len(win), 1.0 / sr)
    df = freqs[1] - freqs[0]

    ns, fn, an = [], [], []
    for k in range(1, 25):
        target = k * f0
        if target > 0.45 * sr:
            break
        lo = int((target - 0.03 * target) / df)
        hi = int((target + 0.03 * target) / df) + 1
        if hi >= len(mag) or hi <= lo:
            break
        j = lo + int(np.argmax(mag[lo:hi]))
        if mag[j] <= 0 or j <= 0 or j >= len(mag) - 1:
            continue
        # parabolic freq refine
        a, b, c = mag[j - 1], mag[j], mag[j + 1]
        d = a - 2 * b + c
        shift = 0.5 * (a - c) / d if d != 0 else 0.0
        ns.append(k)
        fn.append((j + shift) * df)
        an.append(b)
    ns = np.array(ns, float)
    fn = np.array(fn, float)
    an = np.array(an, float)

    # inharmonicity least-squares through the origin: y = B * x, x = n^2
    B = 1e-4
    if len(ns) >= 4:
        x = ns ** 2
        yv = (fn / (ns * f0)) ** 2 - 1.0
        good = yv > -0.001
        if np.any(good):
            B = float(np.clip(np.sum(x[good] * yv[good]) / np.sum(x[good] ** 2),
                              1e-6, 5e-3))

    # brightness from the tilt of a_n * n^2 in log space
    brightness = 15.0
    if len(ns) >= 4:
        z = np.log(np.maximum(an * ns ** 2, 1e-9))
        A = np.vstack([ns, np.ones_like(ns)]).T
        slope = np.linalg.lstsq(A, z, rcond=None)[0][0]
        if slope < -1e-4:
            brightness = float(np.clip(-1.0 / slope, 3.0, 40.0))

    # pickup peak: smooth the envelope, find the broad max above 800 Hz
    band = freqs > 800.0
    if np.any(band):
        sm = np.convolve(mag, np.hanning(64) / np.sum(np.hanning(64)), mode="same")
        pk = 800.0 + freqs[band][int(np.argmax(sm[band]))] - 800.0
        pickup_peak = float(freqs[band][int(np.argmax(sm[band]))])
    else:
        pickup_peak = 3000.0

    return {"f0": float(f0), "B": B, "brightness": brightness,
            "pickup_peak": pickup_peak}


def nearest_open_string(f0):
    """Map a measured open-string f0 to the string index whose open pitch is closest."""
    open_hz = np.array([midi_to_hz(m) for m in OPEN_STRING_MIDI])
    cents = 1200.0 * np.abs(np.log2(f0 / open_hz))
    return int(np.argmin(cents)), float(np.min(cents))


def apply_calibration(path):
    """Load a fit JSON and overwrite the per-string constants + pickup peak."""
    global B_OPEN, BRIGHTNESS, CALIB_PICKUP_PEAK
    cal = json.load(open(path))
    if "B_OPEN" in cal:
        B_OPEN = np.array(cal["B_OPEN"], dtype=float)
    if "BRIGHTNESS" in cal:
        BRIGHTNESS = np.array(cal["BRIGHTNESS"], dtype=float)
    if cal.get("PICKUP_PEAK_HZ"):
        CALIB_PICKUP_PEAK = float(cal["PICKUP_PEAK_HZ"])
    print(f"calibration applied from {path}")


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
def cmd_analyze(args):
    m = args.midi
    pos = positions_for_midi(m)
    f0 = midi_to_hz(m)
    print(f"MIDI {m}  f0={f0:.2f} Hz  ({len(pos)} playable positions 0..{MAX_FRET})")
    print(f"{'string':>6} {'fret':>4} {'B':>10} {'2nd/3rd/5th partial stretch (cents sharp)':>44} {'1st comb notch @ n':>18}")
    for (s, fret) in pos:
        B = inharmonicity(s, fret)
        p = pluck_fraction(fret)
        # cents that partials 2,3,5 are stretched sharp of the ideal harmonic
        cents = []
        for nn in (2, 3, 5):
            ideal = nn * f0
            actual = nn * f0 * np.sqrt(1.0 + B * nn * nn)
            cents.append(1200.0 * np.log2(actual / ideal))
        notch = 1.0 / p
        print(f"{STRING_NAMES[s]:>6} {fret:>4} {B:>10.2e} "
              f"{cents[0]:>12.1f}{cents[1]:>10.1f}{cents[2]:>10.1f}      "
              f"{notch:>18.1f}")
    print("\nHigher B and a lower comb notch = the low-string/high-fret realisation."
          "\nThat spread is exactly the cue the string/fret model learns to read.")


def cmd_demo(args):
    os.makedirs(args.out, exist_ok=True)
    pos = positions_for_midi(args.midi)
    manifest = []
    for (s, fret) in pos:
        y, notes = render_sequence([(s, fret, args.dur)], engine=args.engine)
        cid = f"m{args.midi}_{STRING_NAMES[s]}_f{fret:02d}"
        write_wav(os.path.join(args.out, cid + ".wav"), y)
        with open(os.path.join(args.out, cid + ".notes.json"), "w") as fh:
            json.dump({"sr": SR, "notes": notes}, fh, indent=2)
        manifest.append(cid)
        print(f"  wrote {cid}.wav  (B={inharmonicity(s, fret):.2e})")
    with open(os.path.join(args.out, "manifest.json"), "w") as fh:
        json.dump({"midi": args.midi, "clips": manifest}, fh, indent=2)
    print(f"\n{len(manifest)} same-pitch realisations in {args.out}")


def cmd_fit(args):
    """Fit per-string constants from dry open-string recordings.

    Play each open string (ideally twice); point --dir at the folder of WAVs, or pass
    a single --wav. Each take is mapped to its nearest open string by pitch, fits are
    averaged per string, and the pickup peak is averaged across all takes. Strings
    with no take keep their default; the merged constants are written to --out."""
    import glob as _glob
    if args.wav:
        paths = [args.wav]
    else:
        paths = sorted(_glob.glob(os.path.join(args.dir, "*.wav")))
    if not paths:
        print("no WAVs found")
        return

    per_string = {s: {"B": [], "brightness": []} for s in range(NUM_STRINGS)}
    peaks = []
    for p in paths:
        y, sr = read_wav(p)
        segs = segment_notes(y, sr)
        if not segs:                       # treat the whole file as one note
            segs = [(0, len(y))]
        base = os.path.basename(p)
        print(f"{base}: {len(segs)} note(s)")
        for (a, b) in segs:
            est = estimate_note_params(y[a:b], sr)
            if est is None:
                continue
            s, cents = nearest_open_string(est["f0"])
            tag = f"{STRING_NAMES[s]}({cents:+.0f}c)"
            if cents > 60:
                print(f"  {a/sr:5.1f}s f0={est['f0']:7.1f}Hz -> {tag} off-pitch, skip")
                continue
            per_string[s]["B"].append(est["B"])
            per_string[s]["brightness"].append(est["brightness"])
            peaks.append(est["pickup_peak"])
            print(f"  {a/sr:5.1f}s f0={est['f0']:7.1f}Hz {tag} "
                  f"B={est['B']:.2e} bright={est['brightness']:.1f} "
                  f"peak={est['pickup_peak']:.0f}Hz")

    B_out = list(map(float, B_OPEN))
    bright_out = list(map(float, BRIGHTNESS))
    for s in range(NUM_STRINGS):
        if per_string[s]["B"]:
            B_out[s] = float(np.mean(per_string[s]["B"]))
            bright_out[s] = float(np.mean(per_string[s]["brightness"]))
    cal = {"B_OPEN": B_out, "BRIGHTNESS": bright_out,
           "PICKUP_PEAK_HZ": float(np.median(peaks)) if peaks else None,
           "source_takes": len(peaks)}
    with open(args.out, "w") as fh:
        json.dump(cal, fh, indent=2)
    covered = [STRING_NAMES[s] for s in range(NUM_STRINGS) if per_string[s]["B"]]
    print(f"\nfitted {covered or 'no'} strings from {len(peaks)} takes -> {args.out}")
    missing = [STRING_NAMES[s] for s in range(NUM_STRINGS) if not per_string[s]["B"]]
    if missing:
        print(f"  (kept defaults for uncovered strings: {missing})")


def cmd_corpus(args):
    os.makedirs(args.out, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    manifest = []
    for m in range(args.lo, args.hi + 1):
        for (s, fret) in positions_for_midi(m):
            for r in range(args.reps):
                dur = float(rng.uniform(1.2, 1.9))
                y, notes = render_sequence([(s, fret, dur)], engine=args.engine)
                cid = f"m{m}_{STRING_NAMES[s]}_f{fret:02d}_r{r}"
                write_wav(os.path.join(args.out, cid + ".wav"), y)
                with open(os.path.join(args.out, cid + ".notes.json"), "w") as fh:
                    json.dump({"sr": SR, "notes": notes}, fh)
                manifest.append({"id": cid, "midi": m, "string": s, "fret": fret})
    with open(os.path.join(args.out, "manifest.json"), "w") as fh:
        json.dump({"count": len(manifest), "clips": manifest}, fh, indent=2)
    print(f"wrote {len(manifest)} labelled clips to {args.out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("analyze", help="print the position-dependent physics for a pitch")
    a.add_argument("--midi", type=int, default=64)
    a.set_defaults(func=cmd_analyze)

    d = sub.add_parser("demo", help="render every position of one pitch to WAVs")
    d.add_argument("--midi", type=int, default=64)
    d.add_argument("--dur", type=float, default=1.6)
    d.add_argument("--out", default="./wg_demo")
    d.add_argument("--engine", choices=["ks", "additive"], default=DEFAULT_ENGINE)
    d.add_argument("--calib", help="fit JSON from `fit` to calibrate to your rig")
    d.set_defaults(func=cmd_demo)

    c = sub.add_parser("corpus", help="build a labelled same-pitch corpus over a range")
    c.add_argument("--lo", type=int, default=52, help="lowest MIDI note")
    c.add_argument("--hi", type=int, default=76, help="highest MIDI note")
    c.add_argument("--reps", type=int, default=4, help="clips per (string,fret)")
    c.add_argument("--seed", type=int, default=0)
    c.add_argument("--out", default="./wg_corpus")
    c.add_argument("--engine", choices=["ks", "additive"], default="additive",
                   help="additive = exact labels/cue (default for corpora); "
                        "ks = realistic timbre")
    c.add_argument("--calib", help="fit JSON from `fit` to calibrate to your rig")
    c.set_defaults(func=cmd_corpus)

    f = sub.add_parser("fit", help="fit per-string constants from dry open-string WAVs")
    g = f.add_mutually_exclusive_group(required=True)
    g.add_argument("--dir", help="folder of dry open-string WAVs (each string, x2 ok)")
    g.add_argument("--wav", help="a single dry open-string WAV")
    f.add_argument("--out", default="./wg_calibration.json")
    f.set_defaults(func=cmd_fit)

    args = ap.parse_args()
    if getattr(args, "calib", None):
        apply_calibration(args.calib)
    args.func(args)


if __name__ == "__main__":
    main()


# ---------------------------------------------------------------------------
# Training adapter (drop into tools/ml-training/src, imports the existing front end)
# ---------------------------------------------------------------------------
#   import json, soundfile as sf
#   from features import compute_cqt, cqt_frame_times, tile_context
#   from guitarset import frame_labels          # reused verbatim
#   from fretnet_data import rasterize_onsets   # reused verbatim
#
#   def featurize_synth_clip(wav_path, notes_json):
#       y, sr = sf.read(wav_path)
#       meta = json.load(open(notes_json))
#       cqt = compute_cqt(y, sr)
#       tiles = tile_context(cqt)
#       n = cqt.shape[0]
#       # notes -> the list-of-6-lists (onset, offset, fret) shape guitarset uses
#       per_string = [[] for _ in range(NUM_STRINGS)]
#       for nd in meta["notes"]:
#           per_string[nd["string"]].append((nd["onset"], nd["offset"], nd["fret"]))
#       tab = frame_labels(per_string, n)
#       onset = rasterize_onsets(per_string, n, cqt_frame_times(n))
#       # dev/mask are all-zero (no bends in the synth set) -> shape-compatible target
#       import numpy as np
#       dev = np.zeros((n, NUM_STRINGS), np.float32)
#       mask = (tab > 0).astype(np.float32)
#       return tiles, tab, dev, mask, onset
