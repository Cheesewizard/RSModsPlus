# Fretboard tool (internal)

An interactive guitar fretboard that plays each fretted position back, from either the
**real recorded samples of this guitar** or a **physics-based synth**, with a CAGED
chord engine, clean/distortion amp, and chord progressions. Built as a self-contained
HTML page.

This lives under `tools/` (gitignored from the open-source repo) as an internal tool.

## What it is for

Beyond being a playable fretboard, this is the tone source for **automated testing**:
the same recorded/synthesised notes can drive the game so the detector and ML models
can be walked and measured without a human playing. See "Feeding the game" below.

## Files

- `fretboard_template.html` -- the page (UI, Web Audio playback, JS synth port, chord
  engine, amp). Contains a `/*__GUITAR_DATA__*/` marker where the audio payload is
  injected.
- `guitar_data.js` -- the embedded audio payload: base64 WAV samples (open strings +
  multisample frets, auto-tuned to pitch) plus the synth calibration. Committed so the
  page builds without the raw recordings.
- `build.py` -- injects `guitar_data.js` into the template -> `fretboard.html`.
- `prep_fretboard_data.py` -- regenerates `guitar_data.js` from the source recordings
  (needs the recordings, see below). Imports the tone generators from `tools/`.
- `recordings/` -- dry DI source recordings (NOT committed, large). To regenerate:
  - `recordings/Open Strings.wav` -- each open string, played twice.
  - `recordings/multi_real/` -- one WAV per string named `Low E Frets`, `A Fret`,
    `D Fret`, `G Fret`, `B Fret`, `High E fret`, each an ascending run of frets
    3,5,7,9,12,15,17,19,21.

## Build

```
python build.py                 # -> fretboard.html  (uses the committed guitar_data.js)
```

To rebuild the audio from recordings (after replacing/adding takes):

```
python prep_fretboard_data.py   # recordings/ -> guitar_data.js
python build.py                 # -> fretboard.html
```

## Tone generators (in `tools/`, shared with the ML corpus work)

- `waveguide_guitar.py` -- physically-parameterised DI note generator (additive
  stiff-string model) + `fit` to calibrate per-string inharmonicity from a dry
  recording (`wg_calibration.json`). The synth in the page is a JS port of this.
- `sample_guitar.py` -- builds a playable bank from real recordings (open + multisample
  frets) with pitch-shift and tuning correction. The page's sample playback mirrors it.
- `wg_calibration.json` -- fitted per-string constants for this guitar.

## Feeding the game (roadmap)

The detector reads the dry cable input, and the fake-guitar harness
(`tools/fake-guitar-harness.ps1`) already injects a synthetic guitar onto the Player 1
ASIO route. The next step is to source those injected notes from this tool's
samples/synth, so a chart can be walked end to end with a realistic, labelled tone and
the native + ML detection measured with no one playing. Same clean-DI signal the model
is trained on, driven by the same tone bank the fretboard uses.
