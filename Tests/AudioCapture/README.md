# Isolated audio capture regression tests

Run from the repository root on Windows with Visual Studio C++ build tools:

```powershell
cmd /c Tests\AudioCapture\run.cmd
```

This builds a **32-bit** console harness and runs each scenario in a fresh
process. Output goes to `build/Tests/AudioCapture`. Each scenario has a 15-second
watchdog; a timeout stops only that test process and fails the run. No game
launch, installed DLL replacement, driver opening, timer-resolution request or
audio configuration edit is performed.

To repeat the concurrency scenarios without rebuilding:

```powershell
& .\Tests\AudioCapture\run-tests.ps1 -Scenario @('concurrent-rebind', 'concurrent-players') -Repeat 100
```

## What is exercised

The test translation unit includes the actual `AsioHook.cpp` and
`DelayLinePitchShifter.cpp`. It invokes their registration, preparation,
GetBuffer callback and readiness paths. This is not a second implementation
of the capture algorithm.

The boundary substitutes are a writable fake COM vtable, a fake capture
GetBuffer implementation, observer counters, and unused MIDI link stubs.
The production virtual-table patch helper receives only the fake object.
The memory-patch operation is replaced with a copy into that object's test
vtable. Backend method results and packets are supplied by the fixture.

| Scenario | Required behavior |
|---|---|
| `startup-liveness` | A stream delivering packets before processor preparation cannot be stolen by a newcomer. Disabled samples remain untouched and the backend is called once. |
| `neutral-passthrough` | The real zero-shift processor leaves the original 32-bit PCM packet, including separate channels, byte-for-byte unchanged. Observation still happens. |
| `neutral-int16` | Same requirement for 16-bit PCM. |
| `neutral-int24` | Same requirement for packed 24-bit PCM. |
| `neutral-float32` | Same requirement for float32. |
| `stopped-readiness` | An expired packet timestamp clears reported readiness; returning packets restore it without opening another host. |
| `concurrent-rebind` | Pause an actual callback inside its processor, force the old stream's timestamp stale, and start replacement on another thread. The old binding remains published until the callback exits; the new sample rate is then prepared. |
| `multiple-routes` | Both configured inputs are required; a third stream cannot steal either player's route. |
| `rejected-format` | An unsupported newcomer cannot destroy the earlier binding. |
| `buffer-results` | Failure/empty results are forwarded once without observation or processing; silent packet storage is preserved without an active source. |
| `active-shift` | A real non-zero shifter's changed samples are written back; bypass protection does not discard the effect. |
| `concurrent-players` | Both players can be inside processing concurrently; the lifecycle gate does not serialize independent input callbacks. |
| `late-attachment` | Polling before attachment is safe; later attachment/preparation succeeds, and duplicate registration does not reset a live route. |

The concurrency test uses explicit atomic handshakes, not a hoped-for race
after a short sleep. It waits until the gate is closed while the old callback
is held, checks that replacement has not completed, and then releases that
callback. Timeouts detect a broken/hung test; they do not choose the intended
interleaving. The stale timestamp is injected directly, so the test does not
need to wait for the production inactivity threshold.

## Recorded results, 7 September 2026

Against the original implementation, three separately executed tests failed:

1. A delivering startup stream was replaced before processing became ready.
2. Zero-shift processing rewrote the original PCM/channels.
3. A stopped capture was still reported ready.

After the source corrections: **13/13 scenarios pass**. Additionally,
`concurrent-rebind` and `concurrent-players` passed **100 repetitions each**.
No 1 ms timer request is used by the harness.

## What these tests do not establish

- They do not execute Rocksmith's input manager, Wwise, PortAudio thread/event
  lifecycle or the real RS_ASIO driver callbacks.
- They do not validate executable signatures, live machine-code patch
  installation, patch races with other mods, or late discovery after the last
  unmarshal has already happened.
- They do not verify real endpoint identity, mixed cable/ASIO ordering,
  recycled COM object addresses, driver-buffer support or actual audible latency.
- They do not exercise the INI parser or assert that every inferred route in
  the current parser is appropriate.
- A changed packet in `active-shift` verifies write-back, not pitch quality.
- Passing does not identify the cause of the 7 September failed game launch.

That failed launch had the timer enabled and had already opened/received
ASIO input before stopping. See the
[change and compatibility inventory](../../docs/change-and-compatibility-inventory.md)
for the evidence and remaining acceptance checks. The current runtime still
needs repeated-launch and playing tests with the user's settings unchanged.
# Lifecycle diagnostics

The harness also compiles the production lifecycle tracer. `lifecycle-forwarding`
checks shared-vtable attachment, unchanged HRESULTs, exactly one original call
per invocation, and direct caller capture. `lifecycle-overflow` verifies bounded
event storage and explicit drops without overwriting unread events. All 15
scenarios pass with tracing compiled in. This does not reproduce device startup
or establish the cause of the reported in-game Stop.
