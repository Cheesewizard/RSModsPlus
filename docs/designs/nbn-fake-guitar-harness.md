# Fake-guitar test harness for Note by Note (tier 1)

Status: landed (2026-08-31). Debug-only research tooling.

## Problem

Testing Note by Note meant plugging in a real guitar and playing the exact note sequence by
hand for every iteration - most of which is just *reaching* a spot in a song to check that
frets and transposition are right. Traces failed silently when a note was not played in
time. The bulk of that work is deterministic and does not need a human.

## Idea

Note by Note freezes the transport and waits for the correct note, so there is no real-time
deadline. That turns testing into a request/response loop: read the expected note over the
research bridge, inject it, wait for the freeze to advance, read the next one. We already own
the ASIO input tap (the same one Drop Pedal uses), and an input processor edits the captured
samples in place - so a processor that *overwrites* them is a synthetic guitar the game's own
detection runs on exactly as it would a real pickup.

## What landed

- `DLL/Mods/FakeGuitar/FakeGuitarInjector.{hpp,cpp}` - a host-side `IInputProcessor` that,
  when armed, replaces the Player 1 cable with a plucked tone (fundamental + light 2nd/3rd
  harmonics, attack/decay/release envelope so the onset detector sees a clean rising edge).
  Up to six pitches at once for chords. Lock-free single-producer/single-consumer ring feeds
  it from the bridge thread; `Process` never allocates, locks, or logs. Passthrough (real
  cable, untouched) whenever it is not armed.
- Lifecycle: installed from `ModManager` right where Drop Pedal installs its input hooks -
  before RS_ASIO unmarshals its capture stream, so arming mid-session never misses the stream.
  It installs as a **source stage** (`AsioHook::SetInputSource`), which runs *before* the
  route's processor, so it coexists with the Drop Pedal shifter instead of competing for the
  one processor slot. The host loop calls `FakeGuitar::Poll()` so the ASIO hook auto-enables
  once the song's capture attaches (a source-only route now brings processing up too). Native
  pitch detection stays live on purpose - it is what the harness tests.

## Coexistence with Drop Pedal (the source stage)

`AsioHook` gained a per-route source stage that runs ahead of the processor:
`Hook_CaptureGetBuffer` does `CopyFirstChannelToFloat -> source->Process (if armed) ->
processor->Process -> CopyFloatToAllChannels`. So:
- **Disarmed** = the real cable flows to the processor untouched (a disarmed source is skipped
  entirely, so a normal session is undisturbed - byte-for-byte the shipped path).
- **Armed** = the synth overwrites the input first, then the **real Drop Pedal shifter
  processes it** - the synth goes through the actual pedal, exactly as a physical cable would,
  and the game detects the genuinely-shifted signal.
- **`P` (or `set_fake_guitar`) is the mid-game swap** between the real guitar and the synth.

This makes the harness work in every config, including the normal `engine=automatic` where the
shifter owns the route - the old "mutually exclusive, takes over the route" limitation is gone.
- Research-bridge commands (`DLL/Research/ResearchBridge.cpp`): `set_fake_guitar` (arm),
  `inject_note`, `inject_chord`, `inject_clear`. `status` gained a `fakeGuitar` block
  (installed / synthEnabled / captureReady / currentMidi / pending). The driver reads the
  expected note from the existing `state.expectedMidi` / `selectedString` / `selectedFret` /
  `selectedRecord`.
- Driver `tools/fake-guitar-harness.ps1`: `status`, `enable`, `disable`, `note`, `chord`,
  `clear`, and `walk`. `walk` marches hands-free - injects each expected note (or chord),
  waits for `selectedRecord`/`epoch`/`expectedMidi` to change, logs the transition. The accept
  signal is the freeze advancing, which is path-independent so notes and chords use the same
  detection.

## Controls and UI

- **Hotkey `P`** (debug only) arms/disarms the synthetic input, handled in
  `NoteByNoteProbe::HandleKeyUp` next to the `N`/`J` research hotkeys. No NBN guard - it is
  independent of the controller - and it falls through to the keybinds when the harness is not
  installed (Drop Pedal owns the route).
- **On-screen indicator**: while armed, the NBN overlay draws an amber `FAKE GUITAR ARMED [P]`
  line (top-left, drawn before the NBN-enabled early-return so it shows even with NBN off). It
  appends the MIDI note currently being injected as live feedback, and `(waiting for song
  input)` until the cable capture is live.
- **Automation**: the same arm state is driven over the bridge with `set_fake_guitar`
  (`enable`/`disable` in the driver), so a script can flip it exactly as the hotkey does. The
  hotkey and the bridge are two doors to one `FakeGuitar::SetSynthEnabled`, and `status`
  reports it as `fakeGuitar.synthEnabled`.

## Testing under Drop Pedal / Speaker Mode

The transpose tests must hold under each input mode, so the harness reads and cycles the pitch
route. Findings that shape this: **engine (cable/asio/automatic) and enable are startup-only**
(ini + restart to change); the **pitch mode** `Off -> Drop Pedal -> Speaker Mode -> Off` IS
runtime-cyclable (the F7 path), except Speaker Mode can't be entered/left during gameplay.
Speaker Mode is output-only (song shift), so the NBN **input shift is non-zero only in Drop
Pedal mode** (= the drop amount); it is 0 in Off and Speaker Mode.

- **Read**: `status.dropPedal` reports `pitchMode`, `configuredEnabled`, `speakerMode`,
  `shiftSemitones`, and `inputShiftSemitones` (the frame NBN's expected notes are computed in).
  The driver's `mode` command prints it; the armed overlay line shows `[DropPedal -1]` etc.
- **Cycle**: `cycle_pitch_mode` (driver `cycle-mode`) steps the route exactly like F7, and
  returns the mode actually reached (it refuses Speaker Mode during gameplay).

## Chords

The current chord target's constituent tones are surfaced so `walk` can inject a matching
strum instead of pausing. They come from the exact `ChordTemplateView` (note+0x30) the native
matcher `0x4E6E90` evaluates: the scoring publishes `researchChordTones[]` where the matcher
tones are already derived each held-eval tick, tagged with the record so a poll never sees a
stale set; `GetResearchState` copies them into the structSize-appended
`NoteByNoteState::expectedChordTones` only when the target really is a chord for that record;
`status` exposes them as `state.expectedChordTones`. `walk` reads them and calls `inject_chord`.

Whether the polyphonic matcher accepts a *synthetic* strum (clean sine-sum, no real
inharmonicity/pick transient) is the one thing a synth tone cannot guarantee - so `walk`
treats a rejected chord as a first-class result: it stops and says the synthetic strum was
rejected, which is the precise signal that tier-2 real WAV clips are needed for chords. If the
matcher does accept it, chords walk end to end. The `(NBN NATIVE HIT)` verbose log
(`hit=YES/no`, `sounding=N/M` per tone) is the per-tone diagnostic when a chord is rejected.

## Why a synth tone is enough here (and where it is not)

A clean tone faithfully exercises the deterministic logic: timeline advancement,
transposition math, expected-MIDI computation, fret mapping, lifecycle/re-arm. That is the
bulk of the manual work. It is *not* guaranteed to model real-pickup acoustics (real harmonic
content, inharmonicity, pick transient). Single-note accept now goes through the native
single-note matcher `0x4E7B30` (the finished native port) and chords through `0x4E6E90`; both
read the raw analysis spectrum. Whether a synthetic sine-sum satisfies those matchers is
exactly what the harness lets us test live rather than assume - single notes first, chords via
the `expectedChordTones` path above.

## Tier 2 (next)

Feed recorded real-guitar WAV clips (from the issue #29 ML harness recordings) through the
same injector instead of a synth tone, so the acoustic edge cases reproduce faithfully. The
injector's queue and lifecycle stay; only the sample source changes.

## Limits (unchanged by this work)

- The game must be in a live song for detection and the timeline to run. Getting into a song
  is still manual (or a separate xinput-automation effort).
- Visual confirmation of the overlay still needs a human. The harness reaches that checkpoint
  reliably and removes the wasted "trace came back empty" round-trips.
- Speaker Mode can only be entered/left from the menu, not during gameplay (a game limitation);
  Drop Pedal engine/enable are startup-only (ini + restart). The pitch *mode* cycles live.
