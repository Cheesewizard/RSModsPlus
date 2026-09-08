# Spec: long-window input pitch detection for Note-by-Note single notes

Status: proposed (2026-08-30). Author handoff after the E/Eb-flicker investigation.
See memory `nbn-single-note-accept-reality` for the diagnosis this builds on.

## 1. Problem being solved

NBN's single-note accept currently uses the game's own onset query
`QueryNativeOnsetNote` (`0x48DC40`). Proven this session:

- It is **±1 tolerant** in both directions (played 41 accepted for expected 42), so wrong
  frets pass.
- It is worst on the **low strings**: at ~82 Hz (open low E) the game's short-window detector
  cannot resolve a semitone, so it **flickers E/Eb** (reported the same open E as 39 then 40 at
  q~94). This is the same root as the in-game tuner E/Eb flicker.

The information to tell E2 (82.4 Hz) from Eb2 (77.8 Hz) **is in the signal** (~4.6 Hz apart).
The game misses it because its analysis window is short (it scores *live*, needs low latency).
**NBN freezes the transport and waits**, so it can afford a much longer window - long enough to
resolve the fundamental precisely. That is the whole idea: run our own long-window monophonic
pitch detector on the guitar input and accept on an exact match, instead of the game's coarse
±1-tolerant query.

Scope: **single notes only.** Chords stay on the existing native chord matcher path (polyphonic
detection is a separate, larger problem).

## 2. What already exists (reuse, don't rebuild)

- `DLL/Audio/AsioHook.hpp` - `SetProcessor(size_t routeIndex, IInputProcessor*)` taps the raw
  ASIO **input** PCM. This is the audio tap; no new hook needed.
- `DLL/Audio/IInputProcessor.hpp` - the processor interface: `Prepare(CaptureFormat)` (off-thread,
  do allocation here), `Process(float* samples, uint32_t frameCount)` (audio thread: **no alloc,
  lock, log, or block**), `GetLatencyFrames()`.
- `DLL/Audio/DelayLinePitchShifter` - **already does lag/autocorrelation pitch detection** and
  exposes it atomically: `bool TryGetDetectedMidi(int& midi)`, `std::atomic<int> detectedMidi`,
  `SetPitchDetectionEnabled`, a `quality`/`ratio` confidence, `MIN_LAG` (currently 12 samples =
  1 kHz ceiling). This is the template for the detector - it just runs a **short** window tuned
  for the shifter and only exists while Drop Pedal's input shifter is installed.
- Sample rate: `output_SampleRate = 48000` (`DLL/Mods/AudioDevices.hpp`); assume 48 kHz input.

## 3. Design

### 3a. New processor: `NbnInputPitchDetector : Audio::IInputProcessor`
- **Passthrough**: `Process` edits nothing (copies input to a ring, leaves `samples` untouched).
  `GetLatencyFrames()` returns 0 (adds no delay; Rocksmith's latency calibration must not shift).
- **Ring buffer**: keep the newest ~2048 input frames (mono; if interleaved, take channel 0 or
  sum). ~2048 @ 48 kHz = ~43 ms; go to ~4096 (~85 ms) for margin down to ~E1. Allocate in
  `Prepare`.
- **Detection**: on demand (called from the game tick, not the audio thread), run **YIN** (or the
  autocorrelation already in DelayLinePitchShifter) over the buffered window:
  - Max lag = `48000 / 70` ≈ 686 samples (covers down to ~D1); min lag = `48000 / 1320` ≈ 36.
  - YIN steps: difference function d(τ), cumulative-mean-normalised d'(τ), absolute threshold
    (~0.1-0.15), first τ below threshold, parabolic interpolation for sub-sample τ → f0 =
    48000 / τ → MIDI = round(69 + 12*log2(f0/440)).
  - Cost: ~686 lags × 2048 window ≈ 1.4M mul-add per estimate = sub-millisecond. Run it once per
    accept attempt, not per audio buffer.
  - Confidence gate: reject when d'(τ*) above threshold or level below a floor (decaying tail).
- Expose `bool TryGetDetectedMidi(int& midi, float& confidence)` reading atomics, same shape as
  the shifter.

### 3b. Install / lifecycle
- Install on `AsioHook` (an unused route index) whenever **NBN is enabled AND no input shifter is
  active** (Speaker Mode / Off - input is the player's physical E-standard guitar). Uninstall when
  NBN disables.
- When **Drop Pedal IS active** (input already retuned), either (i) extend the existing
  `DelayLinePitchShifter` detection to a longer window and read its `TryGetDetectedMidi`, or
  (ii) tap the input BEFORE the shift. Detail to resolve; the Speaker/Off path is the common case
  and should land first.

### 3c. NBN integration (`NoteByNoteNativeScoring.cpp`)
- Replace the picked-note accept (currently `QueryNativeOnsetNote() == expected` + the low-string
  ±1 tolerance) with: at a **fresh attack** (reuse the existing rising-edge onset evidence), read
  `NbnInputPitchDetector::TryGetDetectedMidi`. Accept iff `detectedMidi == expectedMidi` **exactly**
  - no ±1 needed, because the long window resolves the semitone.
- `expectedMidi` stays in the **player's E-standard physical frame** (today's fix:
  `GUITAR_STRING_BASE_MIDI + TUNING_OFFSETS + fret + inputShift`) - the detector reads the same
  physical guitar, so they are in the same frame by construction.
- Keep the native `QueryNativeOnsetNote` path as a **fallback** when the detector has no confident
  read (so nothing regresses if the audio tap is unavailable).

## 4. Open questions to resolve first
1. Exact input `CaptureFormat` (channels, interleave, sample rate) at the `AsioHook` tap under
   Speaker Mode - confirm the input processor actually runs when no shifter is installed, or
   whether installing a passthrough processor is what *enables* the tap.
2. Latency floor: measuring an 82 Hz fundamental needs ~2-3 cycles (~30-40 ms). Confirm that delay
   is acceptable inside the freeze (it should be - NBN waits) and that `GetLatencyFrames()=0` does
   not fight Rocksmith's latency calibration.
3. Thread hand-off: buffer on the audio thread (lock-free ring), compute YIN on the game tick.
   Confirm no allocation/lock in `Process`.
4. Drop Pedal path (input pre/post shift) - defer; ship Speaker/Off first.

## 5. Effort / risk
- **Medium.** The tap, processor interface, and a working autocorrelation detector already exist;
  the new work is a longer-window YIN + a lock-free ring + the NBN wiring. ~1-2 focused sessions.
- **Low risk to the game**: read-only passthrough processor + our own computation; additive with a
  native fallback.
- **Not** in scope: chords (polyphonic), and any ML model - a classical long-window detector is
  expected to resolve the monophonic E/Eb case, which is the reported bug. ML (CREPE-style) is a
  later option only if robustness/polyphony is wanted.
