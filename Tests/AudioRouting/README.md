# Audio routing tests

Live-control coverage includes real native/C# pipe transactions, repeated takes,
duplicate Record rejection, finalized WAVs and output changes across differing
hardware periods. ASIO mode tests rename fixture DLLs and verify rollback under
a real file lock, collision refusal and running-game refusal.

Mixer tests cover protocol-3 native/C# volume readback, all seven independent channels,
0/100 endpoints, invalid command rejection, mixer failures and adjustment during
a take. The native harness substitutes a fake mixer; audible Rocksmith behavior
still requires game acceptance. `mixer_ui_tests.cs` loads the built settings EXE
and checks disabled/offline states, readback without writes, and independent
fader values. It renders the real panel off screen with fixture levels without
connecting to or changing the game.

Optional `video_tests.cs` compiles alongside the production `AudioControlClient`
and `WindowCaptureRecorder` sources and needs `rswindowcapture.dll` beside it as
a 64-bit build. Pass a captured MP4, a 48 kHz stereo WAV take and an output path
to exercise the Media Foundation mux and its offset handling. This test does not
capture the game window.

From the repository root, run `cmd /c Tests\AudioRouting\run.cmd`.
This compiles production output and recorder code into a 32-bit harness with a
fake WASAPI endpoint. The default run does not open physical audio devices,
launch Rocksmith, change settings, or deploy DLLs. A 15-second process watchdog
fails hung tests. Artifacts are written under `artifacts/AudioRouting`.

Coverage includes 100,000 ordered queue packets across two threads, the
exclusive-facing acquire/release contract, 16-bit and float stereo parity,
silent packets, bounded backpressure, backend thread ownership, device loss,
actual COM marshaling, and WAV data/header verification after teardown.

For an explicit silent physical-device test, run:

```powershell
./Tests/AudioRouting/run-tests.ps1 -SilentDevice
```

That opens the default Windows output in shared mode, submits 300 silent
packets, and writes a WAV. It does not exercise Rocksmith or capture guitar.
The checked machine offered a 480-frame shared period on 7 September 2026.

The same command builds and runs `asio_config_tests.cs` with the production
`AsioOutputConfiguration.cs`. These tests change fixture INIs only. They verify output switching,
input preservation, restoration, unsuccessful-save rollback, and protection of
an external file edit.

Passing these tests does not establish game-hook compatibility, audible latency,
YouTube coexistence in the game, or successful Note by Note recording. Follow
the [prerelease acceptance steps](../../docs/designs/shared-audio-routing.md).
