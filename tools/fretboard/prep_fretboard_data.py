"""Build the embedded audio payload for the fretboard artifact: real recorded
samples (open strings + multisample frets) as base64 WAV, plus the synth calibration.
Output: guitar_data.js with window.GUITAR_DATA = {...}."""
import sys, os, io, wave, json, base64
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))   # the tone generators live in tools/
import numpy as np
import waveguide_guitar as wg
import sample_guitar as sg

# Source recordings (dry DI, not committed - large). Drop them in tools/fretboard/recordings/:
#   Open Strings.wav      -- each open string, played twice (also used to fit calibration)
#   multi_real/           -- one WAV per string named Low E Frets / A Fret / D Fret /
#                            G Fret / B Fret / High E fret, ascending frets 3,5,7,9,12,15,17,19,21
REC = os.path.join(_HERE, "recordings")
OUT_SR = 22050
CLIP_S = 3.2

def tune_correct(y, target_midi):
    """Snap a sample to its labelled pitch: measure f0, fold to the octave nearest the
    target, and resample away the residual tuning error. Fixes strings recorded out of
    tune (the high-e frets came in ~1.4 semitones flat). In-tune samples are unchanged."""
    f0 = wg.estimate_f0(y * np.hanning(len(y)), wg.SR)
    if f0 <= 0:
        return y
    meas = 69 + 12 * np.log2(f0 / 440.0)
    while meas < target_midi - 6: meas += 12       # fold octave errors toward target
    while meas > target_midi + 6: meas -= 12
    err = target_midi - meas                        # semitones to shift up(+)/down(-)
    if abs(err) > 3.0:                              # too far to trust -> leave alone
        return y
    ratio = 2.0 ** (err / 12.0)
    idx = np.arange(0, len(y) - 1, ratio)
    return np.interp(idx, np.arange(len(y)), y).astype(np.float32)


def extend_ring(y, target_s=2.6, sr=wg.SR):
    """Extend a short note's sustain with a crossfaded granular tail (overlap-add of a
    windowed grain from the sustain region, with continued exponential decay), so the
    high-e samples (recorded only 0.5-1.1s) ring out instead of dying abruptly."""
    y = np.asarray(y, dtype=np.float64)
    if len(y) / sr >= target_s - 0.1:
        return y
    g = int(0.22 * sr); hop = g // 2
    src = int(len(y) * 0.5)
    if src + g >= len(y):
        return y
    grain = y[src:src + g] * np.hanning(g)
    need = int(target_s * sr)
    out = np.zeros(need + g)
    out[:src + hop] = y[:src + hop]
    pos = src; amp = 1.0
    while pos + g < need and amp > 0.02:
        out[pos:pos + g] += grain * amp
        pos += hop; amp *= 0.82                      # per-hop decay (~natural ring-out)
    return out[:need]


def enc(y_22k):
    # resample 22050 -> OUT_SR, trim, int16 WAV, base64
    t = np.arange(len(y_22k)) / wg.SR
    y = np.interp(np.arange(0, min(t[-1], CLIP_S), 1.0 / OUT_SR), t, y_22k)
    y = (np.clip(y, -1, 1) * 32767).astype("<i2")
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(OUT_SR)
        w.writeframes(y.tobytes())
    return base64.b64encode(buf.getvalue()).decode("ascii")

# multisample fretted bank (frets 3..19) + open strings (fret 0)
multi = sg.build_multisample_bank(os.path.join(REC, "multi_real"),
    frets=[3, 5, 7, 9, 12, 15, 17, 19, 21], verbose=False)
opens = sg.build_sample_bank(os.path.join(REC, "Open Strings.wav"))

calib = json.load(open(os.path.join(_HERE, "..", "wg_calibration.json")))

data = {"sampleRate": OUT_SR, "openMidi": wg.OPEN_STRING_MIDI,
        "stringNames": wg.STRING_NAMES,
        "calib": {"B_OPEN": calib["B_OPEN"], "BRIGHTNESS": calib["BRIGHTNESS"]},
        "strings": {}}

total = 0
for s in range(6):
    # original recorded samples, pitch-corrected only (no ring extension / looping)
    samples = []
    if s in opens:                       # fret 0
        samples.append({"fret": 0, "b64": enc(tune_correct(opens[s], wg.OPEN_STRING_MIDI[s]))})
    for fret, samp in multi.get(s, []):
        tgt = wg.OPEN_STRING_MIDI[s] + fret
        samples.append({"fret": fret, "b64": enc(tune_correct(samp, tgt))})
    data["strings"][s] = samples
    total += sum(len(x["b64"]) for x in samples)
    print(f"{wg.STRING_NAMES[s]}: frets {[x['fret'] for x in samples]}")

js = "window.GUITAR_DATA=" + json.dumps(data, separators=(",", ":")) + ";"
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "guitar_data.js")
open(out, "w").write(js)
print(f"\nwrote {out}  ({len(js)/1e6:.2f} MB, base64 audio {total/1e6:.2f} MB)")
