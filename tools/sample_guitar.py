#!/usr/bin/env python3
"""Sample-based guitar corpus generator: build the same-pitch training set from a
REAL dry recording instead of synthesising from scratch, so every clip sounds like
the actual guitar and still carries exact string/fret/onset labels.

Why this beats the physical model for a corpus that must sound real
------------------------------------------------------------------
The waveguide/additive model reduces a string to a few scalars; it can carry the
inharmonicity cue exactly but never sounds like the instrument. A sampler keeps the
real timbre. And it still serves the #66 same-pitch-different-position goal for free:
each string's OPEN note is recorded once, and a fretted note on that string is that
open note pitch-shifted up by `fret` semitones. Because the low-E sample really is
thick and the high-e sample really is thin, the same pitch rendered on different
strings keeps each string's genuine harmonic character -- which is exactly the cue
the model must learn, now sampled from the real guitar rather than modelled.

Pitch shift = pure resampling (read the open-string sample at rate 2^(fret/12)). This
keeps the sound natural (no phase-vocoder smearing); the clip just gets shorter as
the fret rises, which is fine because the labels are time-based, not fixed-length.
The one artifact is that the pickup resonance shifts up with the note (a real pickup
resonance is fixed); for a detection corpus that is a minor, documented domain seam,
and far smaller than the "sounds synthetic" seam of the physical model.

The output format (wav + <id>.notes.json + manifest.json, string order 0=low E2..
5=high e4, fret 0..19) is identical to waveguide_guitar.py, so the SAME FretNet
adapter (tools/ml-training/src/synth_corpus.py) consumes it with no changes.

Usage
-----
  # Inspect what strings the recording yielded and how clean each is:
  python sample_guitar.py bank --rec dry_opens.wav

  # Every position of one pitch, from the real strings -> WAVs you can listen to:
  python sample_guitar.py demo --rec dry_opens.wav --midi 64 --out ./samp_demo

  # Labelled same-pitch corpus over a pitch range:
  python sample_guitar.py corpus --rec dry_opens.wav --lo 52 --hi 76 --out ./samp_corpus

Depends only on numpy + waveguide_guitar.py (for the shared WAV I/O, note
segmentation and pitch helpers).
"""

import argparse
import json
import os

import numpy as np

import waveguide_guitar as wg
from waveguide_guitar import (SR, OPEN_STRING_MIDI, NUM_STRINGS, MAX_FRET,
                              STRING_NAMES, midi_to_hz)


def _resample_to(y, sr_from, sr_to):
    if sr_from == sr_to:
        return y
    t = np.arange(len(y)) / sr_from
    return np.interp(np.arange(0, t[-1], 1.0 / sr_to), t, y)


def _trim_to_onset(y, sr, pre=0.01):
    """Drop leading silence, keeping `pre` seconds of lead-in before the attack."""
    win = max(1, int(0.005 * sr))
    env = np.sqrt(np.convolve(y ** 2, np.ones(win) / win, mode="same"))
    thr = 0.08 * (env.max() or 1.0)
    onset = int(np.argmax(env > thr))
    start = max(0, onset - int(pre * sr))
    return y[start:]


def build_sample_bank(recording_path, verbose=False):
    """Segment a dry recording, map each note to its open string, and keep the best
    (longest, most in-tune) OPEN-string take per string as the source sample.
    Returns {string_idx: float32 sample @ SR}."""
    y, sr = wg.read_wav(recording_path)
    segs = wg.segment_notes(y, sr)
    candidates = {s: [] for s in range(NUM_STRINGS)}
    for (a, b) in segs:
        seg = y[a:b]
        f0 = wg.estimate_f0(seg * np.hanning(len(seg)), sr)
        if f0 <= 0:
            continue
        s, cents = wg.nearest_open_string(f0)
        if cents > 60:                       # not a clean open string
            continue
        candidates[s].append((cents, (b - a), seg))

    bank = {}
    for s in range(NUM_STRINGS):
        if not candidates[s]:
            continue
        # prefer in-tune, then longer
        cents, length, seg = min(candidates[s], key=lambda c: (c[0], -c[1]))
        samp = _resample_to(seg.astype(np.float64), sr, SR)
        samp = _trim_to_onset(samp, SR)
        peak = np.max(np.abs(samp)) or 1.0
        bank[s] = (samp / peak * 0.9).astype(np.float32)
        if verbose:
            print(f"  {STRING_NAMES[s]}: {len(candidates[s])} take(s), "
                  f"chose {cents:.0f}c off, {len(bank[s])/SR:.2f}s")
    return bank


import re as _re


def _string_from_filename(name):
    """Map a filename to a string index. Accepts E2/A2/D3/G3/B3/E4, lowE/highE, and
    loose forms like 'Low E Frets', 'A Fret', 'High E fret'."""
    toks = set(_re.findall(r"[a-z0-9]+", name.lower()))
    if "e2" in toks or "lowe" in toks or {"low", "e"} <= toks:
        return 0
    if "e4" in toks or "highe" in toks or {"high", "e"} <= toks:
        return 5
    if "a2" in toks or "a" in toks:
        return 1
    if "d3" in toks or "d" in toks:
        return 2
    if "g3" in toks or "g" in toks:
        return 3
    if "b3" in toks or "b" in toks:
        return 4
    if "e" in toks:                       # bare E -> high e
        return 5
    return None


def build_multisample_bank(rec_dir, frets=None, verbose=False):
    """Multisample bank from one dry WAV per string, each an ascending run of fretted
    notes. The string comes from the filename. If `frets` (the known ascending fret
    list you played) is given AND a file's note count matches it, the fret of each
    note is assigned BY POSITION -- robust to the f0 octave-errors that plague thin
    high-fret notes. Otherwise the fret is derived from each note's pitch.

    Returns {string_idx: [(fret, float32 sample @ SR), ...] sorted by fret}."""
    import glob as _glob
    bank = {s: [] for s in range(NUM_STRINGS)}
    for p in sorted(_glob.glob(os.path.join(rec_dir, "*.wav"))):
        s = _string_from_filename(os.path.basename(p))
        if s is None:
            print(f"  {os.path.basename(p)}: no string token in name, skipped")
            continue
        y, sr = wg.read_wav(p)
        open_hz = midi_to_hz(OPEN_STRING_MIDI[s])
        segs = wg.segment_notes(y, sr)
        by_position = frets is not None and len(segs) == len(frets)
        for i, (a, b) in enumerate(segs):
            seg = y[a:b]
            if by_position:
                fret = frets[i]
            else:
                f0 = wg.estimate_f0(seg * np.hanning(len(seg)), sr)
                if f0 <= 0:
                    continue
                fret = int(round(12.0 * np.log2(f0 / open_hz)))
            if fret < 0 or fret > MAX_FRET:
                continue
            samp = _trim_to_onset(_resample_to(seg.astype(np.float64), sr, SR), SR)
            peak = np.max(np.abs(samp)) or 1.0
            bank[s].append((fret, (samp / peak * 0.9).astype(np.float32)))
        if verbose:
            mode = "by-position" if by_position else "by-pitch"
            print(f"  {STRING_NAMES[s]}: {len(segs)} notes ({mode}) "
                  f"-> frets {[f for f, _ in sorted(bank[s])]}")
    for s in range(NUM_STRINGS):
        bank[s].sort(key=lambda fr: fr[0])
    return {s: v for s, v in bank.items() if v}


def render_note_multi(bank, string_idx, fret, rng=None):
    """Render from the nearest recorded fret on that string, shifting the remainder."""
    if string_idx not in bank:
        raise KeyError(f"no samples for string {STRING_NAMES[string_idx]}")
    src_fret, src = min(bank[string_idx], key=lambda fr: abs(fr[0] - fret))
    ratio = 2.0 ** ((fret - src_fret) / 12.0)
    idx = np.arange(0, len(src) - 1, ratio)
    y = np.interp(idx, np.arange(len(src)), src)
    if rng is not None:
        y = y * rng.uniform(0.9, 1.0)
    return y.astype(np.float32)


def render_note(bank, string_idx, fret, rng=None):
    """Open-string sample pitch-shifted up by `fret` semitones via resampling."""
    if string_idx not in bank:
        raise KeyError(f"no sample for string {STRING_NAMES[string_idx]}")
    src = bank[string_idx]
    ratio = 2.0 ** (fret / 12.0)             # raise pitch: read faster
    idx = np.arange(0, len(src) - 1, ratio)
    y = np.interp(idx, np.arange(len(src)), src)
    if rng is not None:                       # tiny variation so reps differ
        y = y * rng.uniform(0.9, 1.0)
        trim = rng.integers(0, int(0.01 * SR) + 1)
        y = y[trim:]
    return y.astype(np.float32)


def _is_multi(bank):
    return bool(bank) and isinstance(next(iter(bank.values())), list)


def render_sequence(bank, events, gap=0.15, tail=0.4, rng=None):
    render = render_note_multi if _is_multi(bank) else render_note
    chunks, notes, cursor = [], [], 0.0
    for (s, f, _dur) in events:
        w = render(bank, s, f, rng=rng)
        onset = cursor
        chunks.append(w)
        offset = onset + len(w) / SR
        notes.append({"string": s, "fret": f,
                      "onset": round(onset, 4), "offset": round(offset, 4)})
        cursor = offset + gap
        chunks.append(np.zeros(int(gap * SR), dtype=np.float32))
    chunks.append(np.zeros(int(tail * SR), dtype=np.float32))
    return np.concatenate(chunks), notes


def positions_for_midi(m, bank):
    """(string, fret) in range that sounds m AND has a sampled string."""
    out = []
    for s in range(NUM_STRINGS):
        fret = m - OPEN_STRING_MIDI[s]
        if 0 <= fret <= MAX_FRET and s in bank:
            out.append((s, fret))
    return out


def _load_bank(args):
    """Single-sample bank from --rec, or multisample bank from --multi."""
    if getattr(args, "multi", None):
        frets = None
        if getattr(args, "frets", None):
            frets = [int(x) for x in args.frets.split(",")]
        return build_multisample_bank(args.multi, frets=frets, verbose=True)
    return build_sample_bank(args.rec, verbose=True)


def cmd_bank(args):
    bank = build_sample_bank(args.rec, verbose=True)
    have = [STRING_NAMES[s] for s in sorted(bank)]
    missing = [STRING_NAMES[s] for s in range(NUM_STRINGS) if s not in bank]
    print(f"\nsampled strings: {have}")
    if missing:
        print(f"MISSING (record these open): {missing}")


def cmd_demo(args):
    bank = _load_bank(args)
    os.makedirs(args.out, exist_ok=True)
    pos = positions_for_midi(args.midi, bank)
    if not pos:
        print(f"no sampled positions for MIDI {args.midi}")
        return
    manifest = []
    for (s, fret) in pos:
        y, notes = render_sequence(bank, [(s, fret, None)])
        cid = f"m{args.midi}_{STRING_NAMES[s]}_f{fret:02d}"
        wg.write_wav(os.path.join(args.out, cid + ".wav"), y)
        with open(os.path.join(args.out, cid + ".notes.json"), "w") as fh:
            json.dump({"sr": SR, "notes": notes}, fh, indent=2)
        manifest.append(cid)
        print(f"  wrote {cid}.wav")
    with open(os.path.join(args.out, "manifest.json"), "w") as fh:
        json.dump({"midi": args.midi, "clips": manifest}, fh, indent=2)
    print(f"\n{len(manifest)} same-pitch realisations (real timbre) in {args.out}")


def cmd_corpus(args):
    bank = _load_bank(args)
    os.makedirs(args.out, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    manifest = []
    for m in range(args.lo, args.hi + 1):
        for (s, fret) in positions_for_midi(m, bank):
            for r in range(args.reps):
                y, notes = render_sequence(bank, [(s, fret, None)], rng=rng)
                cid = f"m{m}_{STRING_NAMES[s]}_f{fret:02d}_r{r}"
                wg.write_wav(os.path.join(args.out, cid + ".wav"), y)
                with open(os.path.join(args.out, cid + ".notes.json"), "w") as fh:
                    json.dump({"sr": SR, "notes": notes}, fh)
                manifest.append({"id": cid, "midi": m, "string": s, "fret": fret})
    with open(os.path.join(args.out, "manifest.json"), "w") as fh:
        json.dump({"count": len(manifest), "clips": manifest}, fh, indent=2)
    print(f"wrote {len(manifest)} labelled clips (real timbre) to {args.out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("bank", help="show which strings a recording yields")
    b.add_argument("--rec", required=True)
    b.set_defaults(func=cmd_bank)

    d = sub.add_parser("demo", help="every position of one pitch, real timbre")
    dg = d.add_mutually_exclusive_group(required=True)
    dg.add_argument("--rec", help="single recording (open strings) -> shift per fret")
    dg.add_argument("--multi", help="folder of one-file-per-string fret runs (better)")
    d.add_argument("--frets", help="known ascending frets, e.g. 3,5,7,9,12,15,17,19,21"
                   " (label by position, robust to octave errors)")
    d.add_argument("--midi", type=int, default=64)
    d.add_argument("--out", default="./samp_demo")
    d.set_defaults(func=cmd_demo)

    c = sub.add_parser("corpus", help="labelled same-pitch corpus from the recording")
    cg = c.add_mutually_exclusive_group(required=True)
    cg.add_argument("--rec", help="single recording (open strings) -> shift per fret")
    cg.add_argument("--multi", help="folder of one-file-per-string fret runs (better)")
    c.add_argument("--frets", help="known ascending frets, e.g. 3,5,7,9,12,15,17,19,21"
                   " (label by position, robust to octave errors)")
    c.add_argument("--lo", type=int, default=52)
    c.add_argument("--hi", type=int, default=76)
    c.add_argument("--reps", type=int, default=3)
    c.add_argument("--seed", type=int, default=0)
    c.add_argument("--out", default="./samp_corpus")
    c.set_defaults(func=cmd_corpus)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
