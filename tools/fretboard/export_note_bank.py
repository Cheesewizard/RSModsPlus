#!/usr/bin/env python3
"""Export this guitar's tone bank as note_<midi>.wav for the fake-guitar harness.

The Debug host's FakeGuitarInjector can load a folder of note_<midi>.wav files
(bridge command `load_guitar_samples`) and then plays those REAL recordings for every
injected note instead of its internal synth tone. So a chart can be walked end to end
(tools/fake-guitar-harness.ps1 walk) with this guitar's actual DI tone, and native + ML
detection measured with no one playing -- the same clean-DI signal the model trains on.

Latency: the host loads these once into memory and plays them from the audio thread
(GetLatencyFrames()==0, no allocation/generation per note), so injection is as low
latency as the synth path -- there is no per-note render cost.

Each note_<midi>.wav is rendered at that pitch from the nearest real recorded sample
(pitch-corrected, minimal shift). Output: tools/fretboard/note_bank/. Point the host at
that folder:  ./tools/fake-guitar-harness.ps1 load-samples -Folder <abs path>
(or copy it to the game's RSModsResearch\\GuitarSamples, the default)."""
import sys, os, wave, numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import waveguide_guitar as wg
import sample_guitar as sg

REC = os.path.join(HERE, "recordings")
OUT = os.path.join(HERE, "note_bank")
OUT_SR = 22050
LO_MIDI, HI_MIDI = 40, 83          # E2 .. G5 (single-note range reachable within fret 19)


def tune_correct(y, target_midi):
    """Snap a sample to its labelled pitch (fixes the high-e frets recorded ~1.4 st flat)."""
    f0 = wg.estimate_f0(y * np.hanning(len(y)), wg.SR)
    if f0 <= 0:
        return y
    meas = 69 + 12 * np.log2(f0 / 440.0)
    while meas < target_midi - 6: meas += 12
    while meas > target_midi + 6: meas -= 12
    err = target_midi - meas
    if abs(err) > 3.0:
        return y
    r = 2.0 ** (err / 12.0)
    return np.interp(np.arange(0, len(y) - 1, r), np.arange(len(y)), y).astype(np.float32)


def build_bank():
    """{string: [(fret, tuned sample @ wg.SR), ...]} from the recordings."""
    multi = sg.build_multisample_bank(os.path.join(REC, "multi_real"),
        frets=[3, 5, 7, 9, 12, 15, 17, 19, 21], verbose=False)
    opens = sg.build_sample_bank(os.path.join(REC, "Open Strings.wav"))
    bank = {}
    for s in range(6):
        lst = []
        if s in opens:
            lst.append((0, tune_correct(opens[s], wg.OPEN_STRING_MIDI[s])))
        for f, samp in multi.get(s, []):
            lst.append((f, tune_correct(samp, wg.OPEN_STRING_MIDI[s] + f)))
        if lst:
            bank[s] = lst
    return bank


def render_midi(bank, midi):
    """Render one MIDI pitch from the nearest recorded (string, fret) sample -> float32."""
    best = None
    for s in range(6):
        fr = midi - wg.OPEN_STRING_MIDI[s]
        if 0 <= fr <= 19 and s in bank:
            sf, samp = min(bank[s], key=lambda t: abs(t[0] - fr))
            shift = abs(sf - fr)
            if best is None or shift < best[0]:
                best = (shift, fr, sf, samp)
    if best is None:
        return None
    _, fr, sf, samp = best
    ratio = 2.0 ** ((fr - sf) / 12.0)
    return np.interp(np.arange(0, len(samp) - 1, ratio),
                     np.arange(len(samp)), samp).astype(np.float32)


def write_wav(path, y):
    y16 = (np.clip(y, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(OUT_SR)
        w.writeframes(y16.tobytes())


def main():
    os.makedirs(OUT, exist_ok=True)
    bank = build_bank()
    n = 0
    for midi in range(LO_MIDI, HI_MIDI + 1):
        y = render_midi(bank, midi)
        if y is None:
            continue
        write_wav(os.path.join(OUT, f"note_{midi}.wav"), y)
        n += 1
    print(f"wrote {n} note_<midi>.wav ({LO_MIDI}..{HI_MIDI}) to {OUT}")
    print("load into the running Debug host with:")
    print(f'  ./tools/fake-guitar-harness.ps1 load-samples -Folder "{OUT}"')


if __name__ == "__main__":
    main()
