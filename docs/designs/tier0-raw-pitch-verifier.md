# Tier 0: raw-audio pitch verifier (Goertzel on the input tap)

Status: built 2026-08-31, shadow mode. First consumer of the #18 tap finding; tier 0 of the
detection roadmap on issue #29.

## Problem

The engine's spectral features are semitone-quantized with an inconsistent +/-1 read bias
(measured live: most notes read expected-1, a perfectly played E4 reads dead-on 64). Two
physically different notes can produce identical feature streams, so adjacent-fret
discrimination is impossible downstream of those features regardless of algorithm. The gap is
input resolution, not detector quality.

## Options considered

- Machine learning on the engine's features: rejected, identical inputs are inseparable by any
  model.
- A parallel WASAPI capture device (the original #29 scope): rejected in favour of the existing
  GetBuffer tap, which delivers the same samples the game receives with no device contention or
  clock drift.
- Goertzel measurement bank on the existing tap: chosen. Deterministic, cent resolution,
  trivially cheap, and the NBN freeze removes the latency constraint.

## Design

- `DLL/Audio/RawPitchVerifier.{hpp,cpp}`: a 32768-sample ring fed from
  `Hook_CaptureGetBuffer` with the POST-processing route samples (exactly what the game's
  detector hears; the detector frame is the frame `expectedMidi` is computed in). Silent
  buffers are fed as zeros to keep the timeline continuous. The observation also runs on the
  previously skipped no-source/no-processor path, so a real cable under Speaker Mode is
  observed too.
- `QueryToneEvidence(frequencyHz, windowSeconds)`: Goertzel powers at the target frequency and
  its +/-1 and +/-2 semitone neighbours over the last 0.05-0.3 s, normalized so a full-scale
  sine reads ~1.0 at any window length, plus window RMS. Lock-free reader; a torn ring tail
  only perturbs an energy measurement.
- Host API: `HostApi::QueryRawToneEvidence` appended structSize-forward-compatibly
  (`ResearchProtocol::RawToneEvidence`). A new probe against an old host fails Initialize
  cleanly, so host and probe deploy together.
- Bridge verb `raw_tone` (`{"command":"raw_tone","midi":52,"window":0.15}` or
  `"frequency":110.0`) for ad-hoc measurements from the driver scripts.
- Probe shadow logging: every single-note accept and throttled reject logs an
  `(NBN TIER0)` line with the five powers and RMS for `expectedMidi`, next to the semitone-bin
  verdict that actually decided.

## Rollout

Shadow first, deliberately: the `(NBN TIER0)` lines accumulate correct-vs-wrong evidence
during normal play and harness sweeps. Enforcement (requiring targetPower to dominate the
neighbours before an accept) is added only after the shadow data shows the raw measure
separating the +/-1-fret cases the semitone features provably cannot. The fake-guitar harness
generates labeled data hands-free; `tools/analyze-tier0.ps1` summarizes the shadow lines.

## Tradeoffs

- Player 1 route only for now; multiplayer waits on the #18 attribution trace.
- The Goertzel window (150 ms default) trades low-string resolution against responsiveness;
  the freeze makes longer windows affordable if the low E needs them.
- A real cable WITHOUT RS_ASIO still needs the #18 native unmarshal detour before the observer
  sees it; with RS_ASIO (the current setup) the tap is already live.
