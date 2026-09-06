# Raw Real Tone Cable input tap

Status: designed, not started. Next task. Retires the MultiPitch workaround (issue #18).

## Problem

The Drop Pedal has two engines that do the same job at different points in the signal chain:

- **ASIO engine** taps `IAudioCaptureClient::GetBuffer` on the capture endpoint RS_ASIO
  created, and shifts each route's samples in place before the game reads them. Detection,
  tuner, and tone all see the same shifted audio. No tone setup, stock tones work, measured
  6-20 ms shift latency, and it never touches the tuning reference.
- **Cable engine** (raw Real Tone Cable, no RS_ASIO) has no tap on the raw input, so it cannot
  shift the samples. It works around that by shifting audibly *inside* a custom tone (retuning
  Rocksmith's own MultiPitch Wwise pedal, param id 6, in cents) and separately transposing the
  arrangement tuning reference (`f * 2^(-s/12)`) so detection still accepts the unshifted input.

Everything painful about the Cable engine follows from that one missing tap: a MultiPitch tone
is mandatory (stock tones do nothing), it is per-profile, it resets per song so the slot key is
pressed each time, uniform tunings only, the target must be set before song load so the tuner
snapshot is correct, MultiPitch deletes `Pedal_BassEmulator` so bass tones must re-add the
octave, and Rocksmith's pitch effect carries more latency and stronger artefacts than the ASIO
shifter.

## Key finding: the cable is already WASAPI

Rocksmith reads the Real Tone Cable through **PortAudio to Windows WASAPI
(`IAudioCaptureClient`), with or without RS_ASIO**. RS_ASIO is not a different pipe. It
impersonates a WASAPI endpoint that the game's own PortAudio code opens. Evidence from the
runtime image (`RocksmithNativeAtlas\LocalGhidra\rs2014-unpacked-runtime.bin`, September 2022
Remastered, base `0x400000`):

- `NSoundInputMgr_Win32::DetectInputDevices_PortAudio` @ `0xC43720` enumerates PortAudio
  devices, filters by WASAPI host API and the cable's USB VID/PID, rejects `Stereo Mix`,
  enforces a minimum input-channel count, and assigns the accepted device to a Slot. It logs
  `WASAPI::OpenStream(input): framesPerUser[..] framesPerHost[..] latency[..] exclusive[..]`
  and carries the full `AUDCLNT_*` error table. No DirectSound / WdmKs / wmme in the shipped
  path (a legacy WaveIn fallback exists but is not preferred).
- PortAudio's WASAPI stream object (`PaWasapiStream`) layout is already asserted against the
  shipping build in `DLL/Audio/AsioHook.cpp:82-85`: input `WAVEFORMATEXTENSIBLE` at
  `stream+0x114`, capture-client parent at `stream+0x1F0`, `IAudioCaptureClient*` at
  `stream+0x1F8`. That layout is PortAudio's own, not RS_ASIO's, so it holds for the native
  cable stream too.

The tap point is therefore the same `IAudioCaptureClient::GetBuffer` (COM vtable slot 3,
`SLOT_CAPTURE_CLIENT_GET_BUFFER = 3`) the mod already overwrites in `Hook_CaptureGetBuffer`. It
is the single shared choke point every input stream passes through, upstream of both pitch
detection and the Wwise tone graph.

This resolves the atlas's one open question on ASIO input routing and issue #18's load-bearing
unknown: an equivalent safe raw-input interception point exists, and it is the mechanism the mod
already owns.

## Approach

Reuse the entire existing capture tap and pitch shifter. The only genuinely new piece is
*reaching* the GetBuffer slot without RS_ASIO present.

Today `InstallUnmarshalHook` (`AsioHook.cpp:492`) reaches the capture client by chaining
RS_ASIO's patch of PortAudio's `UnmarshalStreamComPointers`: it finds the patched call site by
the `UNMARSHAL_CALL_*` byte patterns and validates RS_ASIO's `0x68 ... 0xc3` push/ret patch
before chaining it. Without RS_ASIO there is no such patch, so instead:

1. **Native unmarshal detour.** Locate the game's own `UnmarshalStreamComPointers` call site
   and detour it directly (a normal `call rel32`, not RS_ASIO's push/ret patch). The call sites
   are already located by the byte patterns in `AsioHook.cpp:24-37` and by
   `RocksmithNativeAtlas\Ghidra\TracePortAudioUnmarshalCallSites.java`. Add a native-path
   variant of the install that detours the direct call rather than replacing a push immediate.
2. **Reuse downstream unchanged.** From the detour, `RegisterCaptureStream` reads the
   `PaWasapiStream`, takes `captureClient` at `+0x1F8`, patches its GetBuffer slot 3, reads the
   format with `ReadCaptureFormat`, and runs the `IInputProcessor`. The Drop Pedal's existing
   period-synchronous shifter is that processor, reused as-is.
3. **Route identification.** With RS_ASIO the routes come from `RS_ASIO.ini` channels. Native,
   single-player is the first (only) qualifying capture stream. Multiplayer maps two cables to
   Player 1 / Player 2 (see risks).
4. **Retire the workaround.** Once the native tap is proven, the Cable engine drops the
   MultiPitch tone requirement and the tuning-reference transposition, because detection then
   sees shifted audio exactly as the ASIO engine arranges. Confirm nothing else consumes the
   reference-builder detour before removing it; Speaker Mode shifts the song, not the guitar,
   and is a separate path.

## What this collapses

The Cable engine becomes the ASIO engine with a different attachment. Two subsystems go away
for the drop pedal, not one:

- The MultiPitch tone and its whole setup and reset tail.
- The arrangement-reference transposition (`f * 2^(-s/12)`) and its load-time stamp timing
  fragility, which only exist to compensate for shifting after detection instead of before it.

Latency and artefacts drop to the ASIO shifter's 6-20 ms. Uniform-tuning-only and the
per-song tone reselect stop being constraints.

Secondary win: a universal raw-cable GetBuffer tap is the same tap the FakeGuitar harness and
any Note by Note input processing need, so plain-cable users get those without the ASIO route.

## Detection-side fallback (Plan B)

If the native unmarshal detour proves unstable, the detector's own entry is a proven per-player
raw-sample interception point before pitch and DSP:

- `ProcessAudioBlock` (detector vtable slot 1) @ `0x4DE370`, `__thiscall`, arg is a descriptor
  `{+0x04: float* samples, +0x08: int sampleCount}`.
- It memcpys samples via `0x4DE790` into `state+0x420` (count at `state+0x424`), then fills the
  per-semitone spectrum at `state+0x54C` and the pitch estimate at `0x4DF5F0`.
- Overwrite `desc+0x04` on entry, or the copy target `state+0x420`, to inject before detection.

This is heavier (per-detector rather than one shared point) and does not feed the tone graph, so
it is a detection-only fallback, not the primary plan. Its value is that the approach is not a
single point of failure.

## Risks and open edges

- **Native unmarshal path is high-confidence inference, not traced live.** The `PaWasapiStream`
  offsets are asserted against the shipping build, and the mechanism is PortAudio's, but the
  game cannot be run RS_ASIO-free inside the dump. Prove the native stream exercises the same
  unmarshal with a single-player live capture before building on it.
- **Multiplayer attribution is the real unknown.** Two cables enumerate as two WASAPI devices,
  two slots, two detectors (`P1_NoiseFloor` / `P2_NoiseFloor` confirm two-player awareness), but
  the slot to detector binding (which capture client feeds which detector's `0x4DE370`) was not
  statically traceable. Single-player is clean. Gate cable multiplayer on a live two-cable
  trace and do not commit attribution until it is confirmed.
- **Install ordering.** The native detour must be in place before the game unmarshals its
  capture stream. The existing retry-poll (`AttemptUnmarshalHookInstallation`) already handles
  "not ready yet"; install it in `ModManager` in the same slot as the ASIO hook.
- **Exclusive vs shared mode.** The cable may open WASAPI exclusive. The GetBuffer hook is the
  same COM interface either way, and `ReadCaptureFormat` already covers int16/24/32 and float,
  mono and multichannel, so the format the cable negotiates (typically 48 kHz mono) is handled.
- **Tracing.** Use bridge and probe hooks, not software breakpoints; breakpoints fast-fail the
  game.

## Step sequence

1. Add the native-path unmarshal detour (direct `call` variant) beside the RS_ASIO chaining in
   `InstallUnmarshalHook`, selected when `RS_ASIO.dll` is absent.
2. Confirm single-player: a plain cable attaches, `RegisterCaptureStream` logs the endpoint and
   format, GetBuffer is patched, passthrough is clean.
3. Point the Drop Pedal's existing shifter at the native route and verify a shift lands with no
   MultiPitch tone and no reference transposition.
4. Live two-cable trace to prove slot to detector attribution, then enable cable multiplayer.
5. Remove the MultiPitch tone requirement and the cable reference-builder transposition once
   the above holds, after confirming no other consumer of that hook.

## Evidence

- `DLL/Audio/AsioHook.cpp` - PortAudio/WASAPI stream layout, GetBuffer slot 3 hook,
  `UnmarshalStreamComPointers` chaining and call-site byte patterns.
- `RocksmithNativeAtlas\Generated\detector-pitch-pipeline-decomp-2026-08-30.md` and
  `detector-clock-writer-2026-08-25.txt` - detector frame layout, `0x4DE370` / `0x4DE790` /
  `state+0x54C` / `0x4DF5F0`.
- `RocksmithNativeAtlas\Data\atlas.json` (`asio-input`, `native-note-detector`) and
  `Generated\atlas.md` - detector pointer chain and the open question this resolves.
- `RocksmithNativeAtlas\Ghidra\TracePortAudioUnmarshalCallSites.java` - unmarshal call-site
  locator.
- `RocksmithNativeAtlas\LocalGhidra\rs2014-unpacked-runtime.bin` - `NSoundInputMgr_Win32`
  @ `0xC43720`, detector vtable `0x11A54C0`, PortAudio/WASAPI strings. The on-disk executable
  is packed, so this is from the process dump.
