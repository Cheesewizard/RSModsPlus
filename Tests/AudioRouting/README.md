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
the [pre-release acceptance steps](#pre-release-acceptance) below.

Permanent-output coverage includes missing-device startup, timed detached
submissions, automatic same-endpoint reconnection, unplug with a held game
buffer, replacement-device attachment, clock continuity, and failed-switch
preservation. When a configured output is lost and the Windows default is adopted,
that fallback remains active after the old device reconnects; returning to it requires
an explicit output request. The shared-output limiter test verifies live control and
the rendered peak ceiling without loading the ASIO proxy. PersistentInput tests also
verify a permanent default render device with an empty physical device collection.
These fixtures do not open hardware.

## Pre-release acceptance

This is the live acceptance gate for the v4.0.0 Audio Bridge. Automated harnesses are necessary but do
not prove installation, Windows registration, Rocksmith loading, audible output, or a usable recording.
Record source, built artifact, installed artifact, and live result separately.

Do not run the clean-machine cases against a player's only Rocksmith installation. Use a Windows
Sandbox/VM or a disposable game copy. The ASIO registration is machine-wide, so a second Windows user
on the same installation is not an isolated clean-machine test.

### Evidence to capture for every case

- Test ID, date, Windows version, input device, output device, and whether Rocksmith/RSMods ran elevated.
- SHA-256 hashes of `xinput1_3.dll`, `RSMods.exe`, `RocksmithAudioBridge.dll`, and `RocksmithAudioBridgeAsio.dll` from
  the release package and installed game folder.
- Before/after SHA-256 of `RS_ASIO.ini`. Any case that does not explicitly ask the tester to edit it
  must leave it byte-for-byte unchanged.
- Presence of the 32-bit `HKLM\Software\ASIO\Rocksmith Audio Bridge ASIO` registration and the selected
  per-user proxy target.
- Relevant Audio Bridge status text, `RSMods-log.txt`, `RS_ASIO-log.txt`, and a short audible/gameplay
  observation. A created WAV is not sufficient: play it and confirm the expected source is present.

### AB-01 — Install without opting into the ASIO proxy

Start from a clean VM/Sandbox with no `Rocksmith Audio Bridge ASIO` registration. Install the release,
then launch Rocksmith without pressing **Apply output** on an ASIO device.

Expected:

- All four matching runtime binaries are installed, including `RocksmithAudioBridgeAsio.dll`.
- The proxy is not registered and no administrator prompt appears during installation or ordinary launch.
- Rocksmith and non-ASIO Bridge features remain usable; the desktop Bridge starts in the background and
  has a taskbar button.
- `RS_ASIO.ini` is unchanged.

### AB-02 — Decline the one-time registration prompt

With the proxy unregistered and Rocksmith closed, choose an ASIO-capable output, press **Apply output**,
and decline the Windows administrator prompt.

Expected:

- The UI explains that registration was cancelled; it does not crash or report success.
- The proxy remains unregistered, its target is not changed, and `RS_ASIO.ini` is unchanged.
- Repeating Apply offers a clean retry.

### AB-03 — Driver file missing from a partial/broken install

In a disposable install, move `RocksmithAudioBridgeAsio.dll` out of the game folder before pressing
**Apply output** for an ASIO-capable device. Restore it after the test.

Expected:

- The UI says the driver file is missing and asks for Rocksmith Audio Bridge to be reinstalled.
- No registry or `RS_ASIO.ini` change occurs and the desktop Bridge stays responsive.

### AB-04 — Proxy registered but RS_ASIO not linked to it

Complete registration but leave `RS_ASIO.ini` pointed at the real ASIO driver.

Expected:

- Apply records the selected real driver and gives explicit instructions to set the relevant output and
  paired input `Driver` entries to `Rocksmith Audio Bridge ASIO`.
- It does not claim that the Bridge is already in the running game's signal chain.
- ASIO game-mix recording remains unavailable until the INI is configured and Rocksmith is relaunched.

### AB-05 — Proxy unavailable to the running game

Launch Rocksmith in an ASIO configuration that does not load the proxy, then request game-mix recording
and an alternate-device route. Test dry recording separately with an active Player 1 input.

Expected:

- Game-mix recording and alternate routing fail clearly with no silent or zero-length take presented as
  success; the existing playback endpoint remains active.
- Dry recording still succeeds when its input reports ready.
- The game remains playable and the log identifies that the proxy is not loaded.

### AB-06 — Cable/shared-output operation without the proxy

Use Real Tone Cable/shared output with the proxy absent or unregistered.

Expected:

- Playback, mixer controls that belong to the shared-output path, dry recording, and native game-mix
  recording work without proxy registration.
- The UI does not send the player through ASIO setup for this path.

### AB-07 — Desktop Bridge executable unavailable

In a disposable install, move `RSMods.exe` out of the game folder and launch Rocksmith. Restore it after
the test.

Expected:

- The mod logs that the desktop Bridge cannot start; Rocksmith itself does not crash.
- The recording hotkey is ignored with one explicit log entry rather than toggling phantom state.

### AB-08 — Fully configured first launch

Register the proxy, select the real ASIO target, set the relevant RS_ASIO output and paired input drivers
to `Rocksmith Audio Bridge ASIO`, then launch Rocksmith with the interface connected.

Expected: input and output use the real ASIO device, the Bridge reports the proxy as loaded, audible
playback works, and a played-back game-mix take contains both game and instrument audio.

### AB-09 — Boot without the interface, then reconnect

Launch the fully configured setup with the interface disconnected. Reconnect it after Rocksmith reaches
the menu. Do not press Apply until the fallback has been observed.

Expected:

- Rocksmith stays running on the configured Windows fallback; input uses a Real Tone Cable only if one is
  present.
- Reconnecting the ASIO interface does not promote it automatically.
- Selecting the interface and pressing **Apply output** moves paired input/output to ASIO without a game
  restart; audible playback and note detection both work afterward.

### AB-10 — Device loss while ASIO is active

With AB-08 working, disconnect the interface during a non-song menu first, then repeat during playback at
a safe speaker/headphone volume.

Expected: the game remains responsive, output demotes to the Windows fallback, input reports its real
fallback state, no loud spike occurs, and a later explicit Apply restores paired ASIO input/output.

### AB-11 — Remove the proxy

Close Rocksmith, use **Remove audio bridge driver**, and approve the administrator prompt.

Expected: registration is removed, the UI explicitly tells the user to point `RS_ASIO.ini` back to the
real driver, and the INI is not rewritten. After that manual edit, Rocksmith launches normally on the
real driver.

### AB-12 — ASIO device becomes ready after the first hook attempt

Preferred: cold-launch the final candidate on both the interface that previously missed the startup hook
and an interface that was already reliable. If the affected hardware is no longer available, record the
best evidence that can actually be obtained rather than treating the timing problem as locally
reproducible:

- The affected user confirmed that the bespoke build fixed their setup.
- The same hook fix is present in the final candidate and the focused `late-attachment` test passes.
- A previously working ASIO setup still works with the fix present.

Expected:

- The log may report that RS_ASIO or its capture patch is not ready yet, then reports a successful
  attachment without restarting Rocksmith.
- Guitar input, note detection, and Drop Pedal processing work after attachment.
- The capture route attaches once and remains live; later polling does not reset or replace it.
- The evidence shows both sides of the compatibility change: the reported failure is fixed and a known
  working setup has not regressed. Record when these results came from different builds or testers.

This is sufficient evidence for this targeted fix, but it is limited compatibility coverage and must not
be described as proof that every ASIO interface works.

### Release gate

- `Tests\AudioRouting\run.cmd` passes from the release candidate source.
- A clean `Master` host, `Release` proxy, and `Release|x64` GUI/runtime build completes.
- The packaged four binaries have the expected architectures and match the artifacts installed for the
  live tests.
- AB-01 through AB-12 have recorded results. AB-08 through AB-10 and AB-12 require actual hardware and
  audible acceptance; logs or harness output alone are not a pass.
- `RELEASE_NOTES.md` describes only behavior demonstrated by the final candidate and carries the final
  build date.
