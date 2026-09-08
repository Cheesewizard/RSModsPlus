# Audio lifecycle investigation

This diagnostic observes the game's existing capture path on ASIO and native
WASAPI. It does not enable Modern Cable Input, recording, or output monitoring.
It does not change audio configuration, stream arguments, or timer resolution.

Build `DLL/DLL.vcxproj`, Win32, Release Public, with `AudioLifecycleTrace=true`.
Use separate output and intermediate directories and disable deployment targets.
The current local artifact is `build/AudioLifecycleTrace/xinput1_3.dll`.
Ordinary builds omit the tracer entirely.

On startup it creates `RSMods_audio_lifecycle_<process-id>.txt` beside the game.
The trace contains:

- Input audio-client and capture-client identities linked by PortAudio stream.
- Start, Stop, and Reset entry/exit, exact HRESULT, thread, and caller address.
- Caller module and relative address, plus a best-effort stack. Optimized x86
  code may omit stack frames; the direct return address is recorded separately.
- Buffer entry/return and nonempty-packet counts before processor readiness,
  last return time, frame count, and last HRESULT for each attached route.
- Explicit hook-install failures and dropped-event counts.

Lifecycle events enter a bounded ring. Buffer callbacks update counters; file
I/O and address formatting run on the existing mod polling thread. Statistics
are independent atomic samples, not a simultaneous snapshot. Compare event
sequence and monotonic tick values rather than assuming file order. Shared
vtable hooks can also report other clients of the same implementation; use
attachment records to identify the input. Unattached clients have no route
buffer counters. Events before attachment cannot be recovered.

The trace adds no device, driver host, automatic restart, or alternate input
route. Instrumentation still changes timing and can mask a race. A successful
run alone therefore does not resolve the reported intermittent failure.

## Evidence needed from a run

Preserve the diagnostic DLL hash and the lifecycle log together with fresh
`RSMods_debug.txt`, `RS_ASIO-log.txt` (if applicable), and `audiodump.txt` before
another launch overwrites them. Locate the failing input's first Stop/Reset,
resolve its caller against that game's executable, and inspect the preceding
buffer history and native error messages. A Stop call identifies a shutdown
path; determining why that path was entered may require another targeted probe.

## Verification and remaining work

The native diagnostic build succeeds. All 15 isolated capture scenarios pass,
including shared-vtable lifecycle forwarding, exact success/failure results,
caller capture, and bounded overflow without overwriting unread events.
No hardware or in-game capture has yet validated this diagnostic.

Output recording must remain separately controlled: disabled means inactive;
enabled must coexist with an existing ASIO or non-ASIO setup. The current
`MonitorOutput` implementation measures output levels and is not a recorder.
Its coupling to Modern Cable Input must not be mistaken for proof of recording
support. The requested recording feature still needs to be identified before
changing its implementation.
