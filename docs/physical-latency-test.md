# Physical guitar-to-output latency comparison

This test compares ASIO and cable at the user's existing stable settings.
No backend is assumed to be faster. The current HUD is a reported estimate;
results here are a separate physical measurement, not a replacement constant
to insert into the HUD for every user.

## Required recording arrangement

Record two channels simultaneously on one independent recorder/interface:

1. A reference split from the signal entering the game's input device.
2. The game's physical line/headphone output.

Confirm the available recorder, splitter and connections before wiring anything.
The recorder must not take exclusive ownership of the game's interface. Use
appropriate line/instrument levels; never connect a speaker-level output to an
interface input. Disable direct monitoring in the measured output: otherwise
the test can measure the hardware bypass instead of the game.

First feed the same source to both recording channels to check channel skew
and clipping. Preserve this calibration recording. Both channels must use the
same recording clock, with processing disabled and no independent normalisation.

## Two comparison passes

Keep sample rate, ASIO buffer setting, output device where possible, game tone,
recording setup and game state fixed. Document any output-device difference:
that compares complete setups rather than input devices alone.

Use Pitch Off, a clean tone, and no delay/reverb/modulation. Record at least ten
isolated, short, broadband muted-string attacks, a second apart, with no backing
music. Repeat for the cable. Keep modern/stock cable mode recorded in the test
notes. Repeat the first setup to check that the difference is reproducible.

## Analysis

`tools/measure_audio_delay.py` reads a paired recording offline using NumPy and
SoundFile (already in the ML companion environment). Supply window start times
just before each dry attack using `--events`. It compares the 40 ms reference
window against subsequent output audio, allowing inverted polarity and gain
differences. It rejects weak or ambiguous matches and matches at the search
boundary. Sustained periodic notes are unsuitable for this correlation method.

Inspect accepted matches against the waveforms; a numerical match alone is not
physical acceptance. Reject clipped takes. At least five accepted attacks are
required for a median and 10th–90th percentile range. Apply the independently
measured recorder-channel skew when interpreting results, and retain raw values.
If waveform changes prevent clear matches, report inconclusive rather than
force a result. Pitch-shifted or heavily distorted signals require a different
analysis and are outside this initial comparison.

No physical test has been performed merely by preparing this procedure or
passing synthetic analyser checks.
