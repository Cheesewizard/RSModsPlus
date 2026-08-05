# Drop Pedal Multiplayer Technical Findings

## Scope

This document records the technical findings behind multiplayer pitch
processing for the Drop Pedal, for both engines: player identity,
configuration, audio processing, readiness, controls and engine constraints.
User setup instructions are provided in the
[ASIO Drop Pedal guide](asio-drop-pedal.md) and the
[Cable Drop Pedal guide](cable-drop-pedal.md).

## Supported configuration

RS_ASIO input sections map directly to Rocksmith player slots:

| Rocksmith player | RS_ASIO section | Drop Pedal processor |
|---|---|---|
| Player 1 | `[Asio.Input.0]` | ASIO route 0 shifter |
| Player 2 | `[Asio.Input.1]` | ASIO route 1 shifter |

Both inputs must use the same ASIO driver. Each input may select a different
channel from that driver. Configurations that assign the two inputs to
different drivers are rejected because one hook instance cannot safely manage
buffers owned by two independent driver modules.

Input readiness is based on the configured route, negotiated buffer and sample
format. Signal amplitude is not part of readiness; a connected interface input
remains valid when no instrument is plugged into it.

Supported sample formats are:

- `ASIOSTFloat32LSB`
- `ASIOSTInt32LSB`
- `ASIOSTInt24LSB`
- `ASIOSTInt16LSB`

Buffer sizes from 1 to 4096 frames are accepted. The upper bound is a defensive
limit for fixed callback storage, not a recommended operating size.

## Player state

Each player owns independent pitch state:

- target semitone offset;
- physical base tuning used by the overlay;
- persistent pitch-shifter instance;
- ASIO route readiness.

The enabled state is global. Disabling the Drop Pedal sets both processors to
unity. Enabling it reapplies each player's target independently.

Pitch state belongs to the player slot rather than the selected arrangement.
Changing a player between Lead, Rhythm, Emulated Bass and Physical Bass does
not recreate or reassign the processor.

| Instrument configuration | Target calculation |
|---|---|
| Guitar on Lead or Rhythm | Arrangement semitone offset |
| Guitar on Emulated Bass | Arrangement semitone offset; Rocksmith supplies the octave |
| Bass on Physical Bass | Arrangement semitone offset |
| Guitar on Physical Bass | Arrangement semitone offset plus an explicit octave-down shift |

Arrangements within the same song may use different tunings. The two player
targets therefore remain independent and are not derived from one global song
tuning.

## Initialization and ownership

1. Read `[Asio.Input.0]` and `[Asio.Input.1]` from `RS_ASIO.ini`.
2. Validate the shared driver and each configured channel.
3. Install the ASIO class-factory and buffer-creation hooks.
4. Capture the driver's non-interleaved input buffers and negotiated formats.
5. Associate each configured channel with its player route.
6. Prepare one persistent processor per configured route on the game thread.
7. Enable ASIO processing after every configured route is ready.

In automatic engine mode, Cable retains ownership until the complete configured
ASIO route set is ready. Ownership is transferred as one operation, preventing
a state where Player 1 is shifted by ASIO while Player 2 remains unshifted.

Recreating ASIO buffers clears route readiness and disables processing until
the new layout has been captured and both processors have been prepared again.

## Audio callback

ASIO input buffers are non-interleaved, so each player route is processed as a
separate mono stream. For every callback:

1. Resolve the buffer associated with the route's configured ASIO channel.
2. Convert the native ASIO sample format to floating point.
3. Process the block with that route's pitch shifter and target.
4. Convert the processed samples back to the original ASIO format.
5. Invoke the original RS_ASIO callback.

The processing occurs before RS_ASIO copies the samples into Rocksmith. The
game's tuner, note detector and tone chain therefore receive the same shifted
signal.

At a unity target, the processor bypasses pitch detection and splicing while
continuing to update its input history. This keeps the route ready for a later
target change without paying the full shifted-path cost.

## Controls and overlay

| Action | Player 1 | Player 2 |
|---|---|---|
| Pitch down or up | `,` / `.` | `Control+,` / `Control+.` |
| Base tuning down or up | `F9` / `F10` | `Control+F9` / `Control+F10` |
| Enable or disable both players | `F7` | `F7` |

The single-player overlay contains one tuning row. Multiplayer adds a second
row directly below it. Row order matches Rocksmith player order:

```text
Drop: E -> Eb (-1)
Drop: E
```

A shifted row is green. Each row shows its own player's configured state, like
a physical pedal; under Cable ownership a row whose loaded tone has no pitch
shifter reads `Drop: No pedal in tone` instead of a target that is not being
applied.

## Performance characteristics

Processing cost scales with the number of non-unity targets. Unity routes use
the lightweight history-update path; shifted routes perform pitch detection
and period-synchronous splicing.

Reference timings for two 96-frame routes in an x86 Release build are:

| Player 1 target | Player 2 target | Callback processing time | 2 ms callback budget |
|---|---|---:|---:|
| `0` | `0` | 0.0003 ms | 0.02% |
| `-2` | `0` | 0.0235 ms | 1.18% |
| `-2` | `-2` | 0.0460 ms | 2.30% |
| `-12` | `-12` | 0.0632 ms | 3.16% |

These values describe processor cost only. Driver overhead, Rocksmith workload
and system scheduling are external to the Drop Pedal callback.

## Engine constraints

### Cable

The Cable engine supports independent per-player targets, built from two live
identification mechanisms proven by trace:

- **Detection references.** The tuning-reference builder carries the detection
  object in `ESI` and stamps it at `+0x135C`. The detour publishes each call's
  register snapshot; the game loop identifies Player 1's object as the one the
  `ptr_trueTuning` chain resolves into, and any other captured object as
  Player 2's. Until a capture matches Player 1 the previous last-wins
  behaviour is kept, so an unknown game version cannot regress single player.
  Once identified, Player 2's stamps take Player 2's cent adjustment in-detour
  and are kept in step through guarded live writes.
- **MultiPitch attribution.** The pitch shifter's effect `Init` (vtable slot 6,
  identified from a live code dump) pairs each param object with the engine
  context that owns the effect. The context's `+0x18` mixer-pipeline node has a
  session-stable ID at `+0xC` (Sept 2022 exe: Player 1 `0x5e22c1ab`, Player 2
  `0x5e22c1a8`; single-player sessions only ever show Player 1's node). Objects
  are tagged per delivery, because the engine reuses param objects across both
  players' tone loads; a post-`Init` push re-applies owner-correct pitch to
  deliveries that landed before the pairing.
- **Node IDs are per game version.** The IDs are versioned constants
  (`versionedPlayer*PipelineNodeId`). A version whose IDs are not filled in
  logs `unrecognized mixer node ID 0x...` once per ID it encounters — the
  logged values are the constants for that version — and resolves every object
  to Player 1 with Player 2 controls rejected
  (`DropPedalHooks::IsCableAttributionActive`), so audio and detection can
  never disagree.

## Failure handling

| Condition | Behaviour |
|---|---|
| Configured channel is absent from the driver buffers | ASIO processing remains disabled |
| Configured inputs use different ASIO drivers | Configuration is rejected |
| A route negotiates an unsupported sample format | ASIO processing remains disabled |
| A configured route is not ready | Automatic mode retains Cable ownership |
| Player 2 is not configured under ASIO ownership | Player 2 controls are rejected; the overlay keeps showing Player 2's own configured state |
| An input buffer contains silence | Route remains ready and is processed normally |

## Cable multiplayer validation

Validated live on Remastered September 2022 with two interface inputs:
independent per-player targets and note detection across two songs with
different arrangement tunings (Fortunate Son: D standard lead with E standard
rhythm), including tone switches and target changes mid-song. The functional
matrix, each cell in both the same-tuning and different-tuning variants:

| Scenario | What to verify |
|---|---|
| Both players Lead/Rhythm guitar | Each hears their own target; detection tracks each player |
| Player 2 on Emulated Bass | Octave supplied by Rocksmith composes with the shift; A220 song-identity checks unaffected |
| Player 2 on Physical Bass | No double octave; correct target |
| Tone switching mid-song (each player) | Baseline follows the newly loaded tone; no shift applied twice |
| Pre-song tuner (both players) | Each tuner expects that player's shifted pitch |
| Pause and resume | No stale pitch pushed on resume |
| Difficulty change | Builder re-stamp handled; targets persist |
| Song transition and quick requeue | Identification resets and rebuilds; no writes into freed detection objects |
| Mid-song enable/disable toggle | Both players restore to authored and reapply cleanly |

## Cable callback overhead measurement

The Cable engine adds work only in `SpySetParam` (tone loads, occasional) and
the audio-thread push (hotkey-driven, occasional); the per-callback cost is the
pitch-push scan of at most 16 slots. Measure with `QueryPerformanceCounter`
around `PushPitchToLiveShiftersOnAudioThread` in a Release build:

- steady state (no pushes): confirm the pending-flag check is the only cost;
- worst case: repeated hotkey pushes with 8+ live objects in multiplayer;
- long-session stability: a multi-hour session with periodic toggles.
