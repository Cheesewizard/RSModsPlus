# RSModsPlus change and compatibility inventory

Review date: 7 September 2026. Branch: `feature/note-by-note`.
Baseline: common ancestor `ac1702c5892ad1c4bee8d779ba01de52cac399b9`
with the locally available `upstream/develop` reference.
Reviewed branch commit: `23f8d08441c2e664f9caddfc3ec4bf9e84623af3`, plus the
uncommitted development changes present during this review.

This is an engineering inventory of the current development mod, not a claim
that every change is released, tested on every device, or safe to ship.
The companion [file inventory](change-file-inventory.tsv) records the tracked
file changes against that baseline. Existing upstream features are inherited;
their presence is not a new RSModsPlus fix.

## Compatibility requirement

Existing working setups must retain their selected drivers, input channels,
sample rates, buffer sizes, output routing and volume settings. Compatibility
work must attach to the existing audio route rather than create another host,
silently select another device, or require users to retune their configuration.
Unchanged INI files alone do not prove unchanged runtime behavior.

Status terms used below:

- **Implemented:** present in source; this does not certify runtime acceptance.
- **Reported working:** user/reporter observation, limited to those setups.
- **Experimental:** deliberately unverified beyond limited development testing.
- **Under investigation:** a regression or compatibility question remains open.
- **Isolated test:** exercises mod logic without Rocksmith or a hardware driver.

## Audio routing and compatibility changes

| Change | Purpose and behavior | Compatibility status / remaining checks |
|---|---|---|
| Attach to RS_ASIO's existing capture path | Chains the supported PortAudio COM-unmarshal hook and taps the existing `IAudioCaptureClient`. Replaces the earlier dependency on observing ASIO driver/buffer creation at exactly the right time. Does not open a second ASIO host. | Reported working on Philip's setup and another user's setup. Intended to improve compatibility across device startup timings; broad device coverage is untested. Current startup failure is under investigation. |
| Deferred hook installation | Retries when RS_ASIO exists but its module or expected patch is not ready yet. Does not make the startup thread wait for the module. | Implemented. A retry does not guarantee attachment if the only relevant unmarshal already happened. This is not a general ability to discover any arbitrary already-running stream. |
| Replacement capture streams | Supports rebinding a single configured route when its earlier stream has gone quiet. A two-second liveness check attempts to avoid stealing an active stream. | Startup liveness and callback/rebind protection corrected and isolated-tested in this review. Endpoint attribution remains under investigation: the timeout is a heuristic, not endpoint identity. Two-player replacement is not resolved by guessing. |
| Input configuration interpretation | Reads RS_ASIO input sections and the endpoint's negotiated format. The current source also infers a route in some incomplete configurations. | Implemented; output-only and incomplete INI behavior require explicit review. An inferred input must not be advertised as preserving every possible existing setup. |
| Shared input processor | RS_ASIO and native cable captures use the same downstream pitch processor. Supports float32 and 16/24/32-bit PCM conversion. | Zero-shift write-back corrected; exact passthrough isolated-tested in all four formats. Real-device and per-player endpoint selection still require acceptance. |
| Real Tone Cable capture tap | Hooks the native PortAudio capture path when RS_ASIO is absent. Supplies the shared shifter and note observation. | Implemented. Native capture and RS_ASIO attachment are distinct compatibility paths. |
| Modern Cable input | Optional replacement WASAPI capture client, with its own stream opening and buffering behavior. | Experimental; `ModernCableInput` defaults off. Skipped when RS_ASIO is present. Older comments/guides describing unconditional modern capture are not evidence of current defaults. |
| Output monitor | Optional diagnostics around the output client. | Experimental; defaults off. Source records previous startup stalls with it enabled. It must not be enabled by default as an audio fix. |
| 1 ms Windows timer request | The experiment present at review entry applied `timeBeginPeriod(1)` to ASIO and ordinary cable setups too. This review restores the request to the experimental Modern Cable opt-in only, after the ASIO early return. | The installed failed run still contains the experiment. It proves the request can succeed while input fails. This is Windows scheduling, not an ASIO hardware clock/sample-rate/buffer change, and is not a proven ASIO fix. |
| Signal and input-health overlay | Shows observed signal, packet flow and stalled input. Removed the displayed latency estimate because it was not a validated end-to-end measurement. | Readiness now requires recent packets, not preparation alone. The sample-level meter still starts after preparation; route liveness now records packets earlier. |

Sources: [capture implementation](../DLL/Audio/AsioHook.cpp),
[cable implementation](../DLL/Audio/CableInput.cpp),
[ASIO guide](asio-drop-pedal.md).

Windows documents timer-resolution requests as affecting wait accuracy and
scheduling, with possible performance/power costs. Scope differs across Windows
versions. A successful request does not guarantee audio deadlines are met.
See [Microsoft's timeBeginPeriod documentation](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod).

## Playing and practice features

| Change | What the mod adds or changes | Compatibility / validation boundary |
|---|---|---|
| Drop Pedal | Input pitch shifting up/down to 24 semitones, physical base tuning, per-player targets and readout. | Implemented. Uniform transposition cannot replace individual string changes for drop/open tuning shapes. |
| Pitch controls and modes | Off, Drop Pedal and Speaker Mode; rebindable pitch/base/mode keys and player-two modifiers. Starts with pitch processing Off. | Implemented. Off must preserve captured samples and the user's route; installed hooks can still run while the audible effect is off. |
| Multiplayer pitch routing | Separate input-side targets for two configured players; selected capture routes feed their processors. | Implemented. Mixed cable/ASIO and endpoint-open ordering are not certified by a single-player test. |
| Speaker Mode | Shifts preview/full-song music to the guitar's physical tuning and adjusts the chart/tuner reference. Guitar input is not intentionally transposed in this mode. | Implemented. Needs song-format, seek, transition, true-tuning and multiplayer acceptance. |
| Song preparation and temporary cache | Background decode/render, prepare the opening first, render ahead, reuse recent prepared content and retire temporary files. | Implemented. Correct song identity, cancellation, unsupported WEMs and cache lifetime remain compatibility surfaces. |
| PSARC/WEM extraction | Managed extraction and decoding for Speaker Mode, including archive entry-path handling and deployment of required tools. | Implemented. Existing/custom content needs representative coverage; decoder support must not be inferred from one successful song. |
| Note by Note playback | Native note/chord holds and release, Riff Repeater integration, section/seek/restart handling and scoring coordination. | Development implementation. Source checks do not prove song timing, visual placement, chord/bend/legato or lifecycle correctness in game. |
| Note by Note presentation | Riff Repeater menu entry, native MISSED-label replacement while active, fretboard/highway presentation and target feedback. | Implemented/development. Public builds and developer builds have different diagnostic presentation. |
| Native/enhanced pitch evidence | Raw captured audio observations, pitch verification, wrong-note checks and repeated picked-attack handling. | Implemented/development. CPU cost and recognition quality need separate measurement from audio route compatibility. |
| ML-assisted detection | Bundled FretNet service reads shared audio and expected-note context; its evidence can confirm or guard Note by Note decisions. | Implemented/development. Uses a matching managed runtime/model; does not establish that ML caused a particular successful acceptance without decision evidence. |
| Detection feedback and colours | Current WIP exposes native, enhanced and ML decision feedback and ties display colours to the active palette. | Uncommitted development changes at this review. Gameplay and visual acceptance are separate from compilation. |
| Pitch-overlay customization | Settings/UI persistence and defaults for pitch display colours, plus tuning controls. | Implemented. Verify saved values and migration from existing settings without resetting user choices. |

Sources: [Drop Pedal](../DLL/Mods/DropPedal/DropPedal.cpp),
[Speaker Mode guide](speaker-mode.md),
[Note by Note scoring](../DLL/Mods/NoteByNoteNativeScoring.cpp),
[managed ML runtime](../RSModsPlus/MachineLearning),
[settings UI](../GUI/UI.cs).

## Integration, reliability and distribution

| Change | Purpose / scope | Status |
|---|---|---|
| Settings startup ordering | Read keybindings/settings before spawning mod threads, avoiding later replacement of maps while other threads read them. | Implemented. Does not by itself synchronize every other shared state. |
| Separate Wavy Notes hook scratch storage | Avoids sharing the same jump-back scratch value with unrelated hooks. | Implemented; separate from ASIO timer behavior. |
| Safer song timer reads | Checks pointer reads and suppresses repeated failure logging until recovery. | Implemented; inherited secondary timer behavior remains. |
| Logging preservation and access | Saves previous mod/audio logs at launch and permits reading the current mod log while running. Debug console quick-edit behavior was adjusted. | Implemented. ASIO's own log is not preserved by the same startup code; collect it before restarting. |
| Game/menu/tuning integration | Adds gameplay state, tuning, keybinding and overlay integration for pitch modes and Note by Note. | Implemented; changes outside `Audio/` can still affect startup or gameplay. |
| Built-in Note by Note controller | Shipping functionality is linked into the host rather than depending on an optional research module. | Implemented. Validate the actual public package, not only the legacy Release layout. |
| Public/developer build separation | Public configuration excludes developer command transport/loader; developer tooling is optional and identity-checked. | Implemented. Public packaging requires `Release Public`. |
| Bundled managed runtime/model | Packages the managed library, embedded model, dependency extraction/verification and game-owned ML service lifetime. | Implemented. Native host, GUI/service and managed library must match. |
| Build output separation | Per-configuration outputs prevent a newer debug artifact from accidentally satisfying a release build/deploy step. | Implemented. Check hashes of the actual deployed files. |
| Developer harnesses | Synthetic guitar source, research bridge, chart authoring, pitch/model/feedback tests and benchmarks. | Development tooling. Test infrastructure is not a user-facing feature or device certification. |
| Documentation and packaging scripts | Installation, pitch guides, public package validation and development workflows. | Present. Documentation can describe intended behavior beyond the evidence collected here. |

The inherited RSMods suite includes its audio crash-prevention patches,
extended range, song lists, loft control, enumeration, GuitarSpeak, MIDI and
other original features. The current branch's `BugPrevention.cpp` has no
functional delta from the selected upstream ancestor; its audio patch names
in a log do not identify a newly introduced RSModsPlus fix. Consult the
file inventory for changed integration points around inherited features.

## Current ASIO failure: evidence and limits

Failed process started at 14:21:24 local time on 7 September 2026.
Installed DLL SHA-256:
`1AD114166D5822A607A81B08AEF74CDAAA48BF76B34F014558134E5E0A7E23CF`.
It matches the local Release output checked during this review.

- Both this failed run and the previous working run logged a successful 1 ms
  timer request. Philip reports several working builds after enabling it.
- The failed run registered an existing RS_ASIO capture client. RS_ASIO logged
  a 48 kHz, two-channel, 32-bit PCM input with 128-frame buffers, then actual
  GetBuffer/ReleaseBuffer activity. Driver mode remained `BufferSizeMode=driver`.
- RS_ASIO subsequently logged input/output xruns and buffer-lock contention,
  then an input Stop. These messages do not establish who caused the failure.
- Its input Stop precedes the mod's processing-enabled marker when the logs
  are aligned using their common input-unmarshal/registration event.
- The game logged `SoundInputMgr::ResetEvents Failed`. The screenshot shows
  `SIGNAL waiting for input`. Neither is proof of a specific race.
- A read-only wait-chain check of the failed process found no reported cycles;
  it does not exclude unsupported waits or an earlier transient race.

**Conclusion:** the timer is not a reliable fix. Late attachment/rebinding is
a plausible compatibility risk, but this run does not prove it caused the
audio stoppage. It also does not prove that stock RS_ASIO fails with the same
configuration. Do not label this branch audio-compatible for release yet.

## Isolated test strategy

[Audio capture harness](../Tests/AudioCapture) compiles the actual capture-hook
implementation against fake capture endpoints and observation boundaries. It
must check startup before preparation, late attachment, duplicate/new streams,
format changes, overlapping callbacks and preparation, two-player isolation,
and exact passthrough when processing is inactive. Forced interleavings should
be reproducible without timing luck or a 1 ms timer request.

The harness cannot reproduce the proprietary game's whole input lifecycle or
a hardware driver's scheduling. Remaining acceptance uses unchanged INI files
and driver settings: stock RS_ASIO without RSModsPlus, the known working mod,
and the corrected mod; repeated launches plus input/output, tuner, pitch and
song-transition checks. A passing isolated suite is necessary evidence for its
covered logic, not a declaration that the reported startup failure is fixed.

## Corrections and results from this review

The original implementation failed three independently run harness scenarios:
startup stream ownership before preparation, exact zero-shift PCM preservation,
and readiness after packets stopped. Source corrections now:

1. Record a capture's liveness before the processing-ready gate.
2. Serialize registration/preparation/control changes and drain in-flight mod
   callbacks before changing formats, scratch buffers or processor references.
   Audio callbacks do not wait for the control thread; while setup is incomplete
   they return the original captured buffer. Independent player callbacks remain
   concurrent. This does not select a fallback device or host.
3. Validate a newcomer's format/implementation before replacing the prior binding.
4. Write the converted packet back only when the processor/source actually changed
   samples. A zero-shift processor can still observe audio without rewriting it.
5. Report input unavailable when packets have stopped for three seconds; preparation
   alone cannot permanently mark a dead stream ready. This does not restart a
   stopped driver or synthesize input.
6. Remove the unconditional ASIO timer experiment from source while retaining the
   already opt-in Modern Cable behavior.

**Results:** 13/13 isolated scenarios pass, including float32 and 16/24/32-bit PCM,
late attachment, concurrent replacement, concurrent players, error/empty packets
and unsupported formats. The two concurrency scenarios also passed 100 repeats
each. See the [harness README](../Tests/AudioCapture/README.md) for exact coverage.
The native Release Public build succeeds; existing inline-assembly/third-party
warnings remain. No gameplay or hardware acceptance is claimed.

The running game's installed DLL was not replaced, and the installed
`RS_ASIO.ini` and `Rocksmith.ini` hashes matched their pre-investigation copies.
The generated validation DLL is a local build artifact, not a deployed fix.

## Maintaining this inventory

An explicit diagnostic build now traces existing input Start/Stop/Reset calls
and buffer activity on ASIO and native WASAPI. It preserves method results and
does not enable recording or modern capture. The diagnostic DLL builds and all
15 isolated scenarios pass; no new in-game trace has been captured yet. See
[audio lifecycle tracing](audio-lifecycle-tracing.md) for limitations and evidence
requirements. This is investigation tooling, not a proven fix for the Stop.

For each compatibility change, record the source revision, affected setups,
default/opt-in behavior, exact test and result, and remaining uncertainty.
Keep user-reported success separate from locally reproduced results. Add
regression findings here before promoting an experiment into a default.
