# Audio routing tests

Live-control coverage includes real native/C# pipe transactions, repeated takes,
duplicate Record rejection, finalized WAVs and output changes across differing
hardware periods. ASIO mode tests rename fixture DLLs and verify rollback under
a real file lock, collision refusal and running-game refusal.

Mixer tests cover protocol-4 native/C# volume readback, all seven independent channels,
0/100 endpoints, invalid command rejection, mixer failures and adjustment during
a take. The native harness substitutes a fake mixer; audible Rocksmith behavior
still requires game acceptance. `mixer_ui_tests.cs` loads the built settings EXE
and checks disabled/offline states, readback without writes, and independent
fader values. It renders the real panel off screen with fixture levels without
connecting to or changing the game.

Dry recording tests exercise native and C# start/stop, input availability and
sample-rate validation, repeated takes, source isolation, frame counts and first
packet timestamps while playback continues. UI fixtures verify dry readiness and
source locking during a take. AudioCapture's active-shift scenario verifies that
the real capture hook records the pitch shifter's output before game tones.

Optional `video_tests.cs` compiles alongside the production `AudioControlClient`
and `WindowCaptureRecorder` sources and needs `rswindowcapture.dll` beside it as
a 64-bit build. Pass a captured MP4, a 48 kHz stereo WAV take and an output path
to exercise the Media Foundation mux and its offset handling. This test does not
capture the game window.

From the repository root, run `cmd /c Tests\AudioRouting\run.cmd`.
This compiles production output and recorder code into a 32-bit harness with a
fake WASAPI endpoint. The default run does not open physical audio devices,
launch Rocksmith, change settings, or deploy DLLs. A 15-second process watchdog
fails hung tests. Build outputs and test recordings are written under
`build/Tests/AudioRouting`.

All repository test harnesses, including `DLL/AsioProxy/test/run-tests.sh`, use
the ignored `build/Tests` tree for generated executables, DLLs, and logs. Run
them from the checkout; do not copy a checkout to `%TEMP%` or copy/run a test
executable from a temporary directory.

Coverage includes 100,000 ordered queue packets across two threads, the
exclusive-facing acquire/release contract, 16-bit and float stereo parity,
silent packets, bounded backpressure, backend thread ownership, device loss,
actual COM marshaling, and WAV data/header verification after teardown.

The proxy failure matrix uses a controllable ASIO stub to stop callbacks while
the client remains open. It verifies stalled-mode publication, live demotion to
virtual callbacks, continued callbacks after a failed promotion, no automatic
upgrade when hardware returns, and explicit recovery. The host policy matrix
covers silent boot, first endpoint arrival, active endpoint loss, fallback
replacement, last-endpoint loss, explicit-route preservation, and removal of a
host-managed fallback after an explicit ASIO promotion. UI fixtures verify that
starting, silent, downgrading, and temporarily stalled states never display the
performance bolt or a terminal restart instruction.

For an explicit silent physical-device test, run:

```powershell
./Tests/AudioRouting/run-tests.ps1 -SilentDevice
```

That opens the default Windows output in shared mode, submits 300 silent
packets, and writes a WAV. It does not exercise Rocksmith or capture guitar.
The checked machine offered a 480-frame shared period on 7 September 2026.

The same command builds and runs `asio_config_tests.cs` with the production
`AsioProxySetup.cs`. These tests guard the contract that the GUI never writes
RS_ASIO.ini: they fail if the old Link/Unlink/LinkSettings/SynchronizeLinkedInputs
methods return, and they confirm that reading link state leaves a fixture ini
byte-for-byte unchanged. Pointing RS_ASIO at the bridge is the user's job; the GUI
only registers the proxy and records the wrapped driver in the HKCU Target.

Passing these tests does not establish game-hook compatibility, audible latency,
YouTube coexistence in the game, or successful Note by Note recording. Follow
the [prerelease acceptance steps](../../docs/designs/shared-audio-routing.md).

Permanent-output coverage includes missing-device startup, timed detached
submissions, automatic same-endpoint reconnection, unplug with a held game
buffer, replacement-device attachment, clock continuity, and failed-switch
preservation. When a configured output is lost and the Windows default is adopted,
that fallback remains active after the old device reconnects; returning to it requires
an explicit output request. The shared-output limiter test verifies live control and
the rendered peak ceiling without loading the ASIO proxy. PersistentInput tests also
verify a permanent default render device with an empty physical device collection.
These fixtures do not open hardware.
