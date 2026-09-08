# Audio bridge: shared playback and repeated takes

Status: local implementation on `feature/modern-cable-input`, also integrated
into an isolated Note by Note test snapshot. Not published or accepted in game.

## User workflow

Open **Audio bridge** from the RSModsPlus settings. Choose **Audio Â· WAV**
or **Video + audio Â· MP4**, then press **Record** and **Stop & save** for each
take. There is no per-take restart or launch-time recording checkbox. F9 starts
or finishes a take; F5 re-reads the playback devices.

The window is one deck over three panels. The deck carries the timecode, a dBFS
level meter with peak hold and clip indication, the transport, and readiness
chips for input transport, window capture and free disk space. Below it sit
**Playback** (output device and guitar input transport), **Recent takes** (the
last takes in the folder with length, size and timestamp; play or reveal a
selection) and the takes folder with its free-space estimate. Both path fields
are read only and are set through their dialogs.

Record is enabled only when a take can actually start. When it is disabled the
deck states the reason: no bridge, an unfinalized errored take, a relative folder
path, or MP4 selected on a Windows build without window capture. While recording,
four seconds without any submitted sample is reported as a silent output rather
than left to be discovered in the saved file.

Output changes use the live bridge when connected. The bridge must first be
loaded by the game: enabling it in an already running instance only configures
the next launch. An unavailable bridge is reported as offline, not inferred from
the presence of a running game process or from saved configuration.

The **Cable / ASIO** selector chooses the input transport for the next game launch:

- Cable: rename `avrt.dll` and `RS_ASIO.dll` to their `.dll.disabled` counterparts.
- ASIO: restore the exact filenames and file contents.
- Refuse to switch while that Rocksmith installation is running.
- Refuse incomplete pairs and filename collisions; never overwrite a DLL.
- If the second rename fails, restore the first and report the failure.

This mode switch changes loaded modules, so it cannot unload or replace an
already running ASIO stream. RS_ASIO retains its own device/channel integration.
Live Windows capture-device replacement is not implemented in this revision;
the game/cable capture route is still selected before opening its stream.

## Settings window rebuild (7 September 2026)

The first window stacked every control in one column of stock WinForms controls:
a `ProgressBar` meter, a mode combo box, a mode button labelled with both its
state and its action, and a single status line. Nothing separated the transport
from the settings, a disabled **Record** gave no reason, and MP4 could be started
with no encoder present, failing only once the take was requested.

Options considered: regroup the stock controls only; move the window to WPF or
WinUI for real styling; or stay in WinForms with a small set of owner-drawn
primitives. WPF was rejected because the settings executable is a .NET Framework
4.7.2 WinForms application and a second UI stack would be carried for one window.
Regrouping alone would not have fixed the meter, the button states or the missing
readiness reporting.

The window now composes local primitives - `StudioTheme`, `StudioCard`,
`StudioButton`, `SegmentedControl`, `StatusChip`, `LevelMeter`, `TakeList` - with
`StudioFormat` for byte, duration and free-space formatting. The bridge protocol,
INI handling and ASIO mode switching are unchanged; the panel calls the same
operations in the same order. Tradeoffs: the primitives are hand-painted, so they
carry their own hover, focus, disabled and DPI behaviour instead of inheriting it
from the common controls, and the window wants about 1060x880 to show every panel
without scrolling.

## Replacing FFmpeg with the Windows capture stack (7 September 2026)

The FFmpeg path needed two 212 MB GPL binaries the user had to install, could not
capture a fullscreen game, and reconciled clocks by reading the container's start
timestamp back with `ffprobe`. Bundling it was rejected on size and licensing; a
minimal custom build would still have meant maintaining a build pipeline for a GPL
binary.

Options considered: bundle a stock or custom-built FFmpeg; drive Xbox Game Bar,
which has no public start/stop API and writes to its own folder;
`AppRecordingManager`, which generally requires package identity that an injected,
unpackaged process does not have; or call the Windows capture and encode APIs
directly.

The last was chosen. The code lives in a small x64 helper library loaded by the
settings process rather than in `rsmodsplus.dll`, because the injected library is
x86, the settings process already resolves the game's window handle and the take
paths, and this needed no change to the bridge protocol. Tradeoffs: the repository
carries a C++ CMake target that the GUI build invokes, mirroring the existing soxr
step; capture requires Windows 10 1903 or newer; and the mux is a second pass over
the take rather than a single live write, because the audio is written by the game
process and only becomes readable once the take stops.

## Audio path and ownership

### Playback mixer

Playback contains seven 0–100 faders: Song, Player 1, Master, Player 2,
Microphone, Voice-over and Sound effects. These control `Mixer_Music`,
`Mixer_Player1`, `Master_Volume`, `Mixer_Player2`, `Mixer_Mic`, `Mixer_VO` and
`Mixer_SFX`, respectively, through the existing global and player-object scopes.
Player channels cover processed guitar/bass playback, not capture gain or detection.
The version-3 bridge returns all seven levels; operations 7–13 set them in that
order. A failed channel read returns -1 and disables only that slider. Matching
settings and game DLL builds are required. These are live game levels;
opening the panel reads them without applying a saved override.

The settings panel coalesces slider changes at 100 ms and serializes them with
recording and routing commands. Recording can continue while volumes change;
the recorded game mix follows those playback changes. Offline state disables all faders; a failed channel query disables only its
fader rather than showing an assumed level. Buffer testing
temporarily disables them while it owns the control connection.

Audio control protocol 3 includes a mixer HRESULT and seven float volumes in the
response (3144 bytes). Operations 7 through 13 set the seven channels,
accepting only decimal integers in 0â€“100. Both GUI and game DLL must be updated
together and the game restarted; older protocols are rejected explicitly. The native
routing harness uses a fake mixer to verify commands/readback and recording
continuity; this does not establish audible in-game behavior.

The game retains its exclusive-facing audio client, buffer contract and render
interface. It writes into a preallocated two-packet queue. One MTA output worker
owns physical WASAPI activation, rendering, clock reads and release. It converts
the supported stereo 48 kHz PCM/float game stream to stereo float Windows shared
playback. Desktop audio joins later in the Windows mixer, outside the recorder.

A versioned, per-process local named pipe serves bounded commands and status.
It rejects remote pipe connections and unsupported messages. The control thread
coordinates with the output worker; it never passes physical COM interfaces to
the settings process. Destruction stops the control server before the worker.

Recording opens a fresh WAV writer outside the output worker and attaches it at
a serialized packet boundary. Stop detaches it before draining/finalizing the
file off the output thread. Disk errors are reported independently of playback.
WAV contains the complete game mix, including tones and backing audio; this is
not an isolated guitar stem, microphone endpoint, or desktop loopback capture.

Output switching prepares the requested shared endpoint, validates its format,
buffer and clock, then replaces the physical endpoint. The game-facing buffer
size stays fixed even if the new hardware period differs. Failed preparation
leaves the current endpoint intact and returns an explicit error. Padding is
bounded using the active hardware period, and the presented clock keeps its
original frequency with an offset across switches. Switching may cause a brief
gap; seamless audible transitions and lost-device recovery still need game tests.

## Video path

Video uses the Windows capture and encode stack through a 44 KB helper library,
`rswindowcapture.dll`, loaded by the settings process beside itself:
Windows.Graphics.Capture for frames, a Media Foundation sink writer for H.264. It
captures the Rocksmith window handle obtained from the bridged game process and
nothing else - never the desktop, never another application's window. There is no
external encoder to install, discover, bundle or license.

The capture records the QPC and FILETIME of its first delivered frame, so the
video clock origin is known exactly rather than read back out of the container.
The bridge timestamps the first recorded audio packet as before. Stop closes the
sink writer, computes `audio start - video start`, rejects a disagreement beyond
30 seconds, then remuxes the captured MP4 and the WAV into the final MP4: H.264
samples are copied without re-encoding, the PCM is encoded to AAC, and the offset
is applied to whichever stream started later. The WAV and the intermediate MP4 are
retained when finalization fails. No encoding or file I/O runs in the game's
render callback.

This synchronization aligns initial timestamps. Game stream pauses/recreation,
hardware switching gaps and long-run clock drift require validation; the current
audio writer does not synthesize wall-clock silence across missing packets.
Windows.Graphics.Capture needs Windows 10 1903 or newer; when it is unavailable
the window says so and blocks MP4 rather than failing at Record. Fullscreen
capture is expected to work where the GDI path could not, but is not accepted as
working until it is tested in game.

## Verification

### Timed game wake-ups

Later user testing confirmed that M-Audio does play audio; earlier reports of
silence do not establish a startup defect. Crackling remains on both outputs.
The handoff counters find repeated nonzero 128-frame blocks before the render
queue, including intervals with no empty-output observations.

The bridge previously requested new game blocks as soon as queue space became
available, allowing callback bursts at a physical engine wake. A regression test
using an immediately available endpoint demonstrated that 192 ms of game audio
could be requested in a burst. Game wake-ups now use a high-resolution waitable
timer paced by the actual submitted block size. Deadlines preserve the sample
rate through timer jitter; missed full periods do not trigger catch-up bursts.
The outstanding-slot reservation remains in force. Buffer sizes are unchanged.

The regression test now passes together with existing queue, recording and live
switch tests. Windows versions without high-resolution waitable timers report
initialization failure. The audible result and repeated-block counts still need
comparison in game; this is not an accepted crackling fix yet.

### Game callback block size

The first audible game run used 128-frame user callbacks but 144-frame host
requests after the bridge advertised the physical device's 480-frame period.
An 8.541-second recording contained 93 consecutive identical 128-frame blocks
with peaks above 0.01 full scale, many on a 120 ms cycle, and no clipped samples.
This identifies repeated content before physical playback, not its exact cause.

A candidate that advertised the requested 128-frame callback count instead of
480 was rejected in game: both game and guitar audio were reported inaudible,
although buffer calls continued without errors. That change has been removed;
the prior audible game-facing contract is restored in the next local build.

New diagnostic counters inspect consecutive nonzero 128-frame blocks at game
ReleaseBuffer, before queueing, and count empty physical-output observations
and the longest pump gap. The harness checks repeated blocks spanning 144-frame
host boundaries. These are diagnostic observations, not a hardware-underrun
verdict. No automatic buffer increase was added. Audible playback and the source
of the repetitions remain subject to the next runtime test.

### Cable-mode output wake regression

The diagnostic game run on 7 September started output and stopped it about
20 ms later. The fifteenth 144-frame buffer request returned
`AUDCLNT_E_BUFFER_TOO_LARGE`; fourteen releases succeeded, while the physical
output reported no error. The worker could issue a second wake while the game
held an uncommitted queue slot, leaving a stale event after that slot filled.

Each wake now remains reserved until buffer release or cancellation. The worker
signals again only when a queue slot is available. Stop clears the outstanding
event. A regression test failed on the duplicate wake before the fix and passes
with the fix, covering held buffers, a full physical buffer and queue, resumed
draining, cancellation and restart. This does not yet establish audible game
playback or working tuner detection.

### Cable-mode output cap regression

The 7 September game test exposed a missed host contract: Rocksmith caps its
480-frame output to `MaxOutputBufferSize=144`, then requests 144 frames from the
render client. The bridge previously required exactly 480 and rejected those
requests. Capture packets still arrived and the overlay showed strong guitar
peaks, but that did not prove the game's detection path was consuming them.

The render queue now accepts nonzero requests up to the advertised buffer size
and carries each packet's actual length through playback, metering and recording.
The regression harness covers 144-frame requests on a 480-frame endpoint, varying
packet sizes, stereo sample identity, recorded frame/file lengths and stale peak
avoidance. It does not change Rocksmith.ini to bypass the problem. Game audio and
input detection still require retesting after deploying the corrected DLL.

On 7 September 2026:

- Production output harness passed thread ownership, queue ordering, formats,
  silence, buffer pressure, invalidation, COM marshaling and WAV byte checks.
- Actual named-pipe tests passed repeated Record/Stop, finalized takes, duplicate
  Record rejection, failed-device switch preservation and a 128-to-480-frame
  hardware period change with the 128-frame game buffer retained.
- The production C# control client recorded repeated takes through that native
  pipe, exercising the actual cross-process message layout.
- ASIO configuration preservation/rollback checks passed. Mode tests passed both
  renames/restoration, running-game refusal, partial rename rollback under a real
  file lock and refusal to overwrite conflicting names.
- Video finalization produced H.264/AAC MP4 from timestamped generated fixtures.
  A requested 200 ms audio offset measured approximately 199 ms after AAC encoding.
  This verifies mux timing, not capture of Rocksmith's graphics.
- An earlier physical-output smoke test passed 300 silent packets at a 480-frame
  minimum period (10 ms at 48 kHz). That is not measured guitar-to-output latency.

Before prerelease, test game startup with each input mode, repeated audible takes,
simultaneous YouTube on the same output, sustained Note by Note tones, real video
frames/synchronization, output changes, focus changes and device removal. Run the
capture harness on the combined snapshot. Build/hash evidence is not gameplay
acceptance. No publication or installation is implied by these source changes.

## References

- [Microsoft low-latency shared audio](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio)
- [Microsoft shared stream initialization](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream)
- [Windows.Graphics.Capture](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture)
- [Media Foundation sink writer](https://learn.microsoft.com/en-us/windows/win32/medfound/tutorial--using-the-sink-writer-to-encode-video)
- [RS_ASIO Windows output with ASIO input](https://github.com/mdias/rs_asio/blob/master/docs/streaming/README.md)

### Audible pacing validation, 7 September 2026

User reported that M-Track sounds good with the paced-output build, then switched
mid-game to Realtek/laptop speakers and reported that output sounds good too.
Installed DLL SHA256: 698E7696F82EB1D464877C571EB90119CA9797CA0E7BFB686C9EF2F6A023A802.

Live PID 82088 at 19:44:49-19:44:54: output running, zero acquisition failures,
zero repeated game blocks across 43,925 inspected 128-frame blocks. Submitted
frames advanced from 5,379,408 to 5,622,480. Empty-output observations remained
1,068 during the five-second comparison; the cumulative longest pump gap was
3,640 ms. These cumulative figures include earlier lifecycle activity and are
not proof of a completely glitch-free session. A fresh recorded take, extended
playback and video acceptance remain to be checked. No commit or release made.

## Tabbed mixer and buffer controls (7 September 2026)

Audio bridge separates Mixer, Recording and Setup into themed tabs. Setup shows
live output frames, milliseconds at 48 kHz and mode. An absent saved period is
Automatic (device minimum); a saved finder result is Automatic (finder result);
a user-applied period is Custom. Older saved periods without mode metadata are
identified as mode not recorded. Choosing an editing mode does not itself alter
playback; Apply custom buffer validates the supported range/step, applies op6,
reads back the actual result, then saves per-device PeriodFrames and Mode.

Disconnected faders hide the slider and say Unavailable. Live polling reads game
volumes without writing them. The custom slider draws one blue thumb for idle,
hover and drag; keyboard arrows and Page Up/Down also adjust it. Sending mixer
changes refreshes only the mixer instead of disabling/re-enabling the whole
panel. Capture gain and detection remain separate.

Validation: combined x64 GUI build, native/C# routing tests and offscreen renders
of all tabs passed. UI tests cover unavailable/readback states, keyboard changes,
no commands from readback, and current-buffer/custom-mode display. At the live
read on 7 September, the Realtek output reported 480 minimum/maximum/fundamental/
active frames (10 ms); no saved period was present. This is a current device
period observation, not proof of stability or total guitar latency. The user
confirmed guitar volume adjustment and startup readback; live acceptance of the
new slider painting and tabbed layout remains to be checked.

## Console mixer redesign (7 September 2026)

Problem: the seven faders sat in one wrap panel in bridge-report order (Song,
Player 1, Master, Player 2, Microphone, Voice-over, Sound effects), all with the
same weight and only two icons between them, inside a card that did not stretch.
The tab was mostly empty space, the two players were separated by Master, there
was no mute, and a channel at zero looked broken rather than muted.

Options considered: (a) reorder the existing flow panel only; (b) keep the flat
row but colour-code it; (c) group the strips into banks with a dedicated master.
(a) leaves the emptiness and the missing mute, (b) does not explain why Master
differs from the rest, so (c) was chosen.

Chosen approach: MixerChannels.cs holds the channel identity (bridge id, name,
tint, glyph) so nothing else has to know the ordering. The strips array stays
indexed by bridge channel id (op 7 + channel is unchanged); only the on-screen
placement changes. MixerGroup lays out Master, then Players, then Game audio,
separated by MixerDivider hairlines and centred in a card that fills the tab.
Each MixerStrip carries its own icon, tint, filled fader, percentage readout and
a MuteButton that parks the level at zero and restores the previous value. The
master strip also shows a ChannelMeter fed by the same output peak as the
recording meter. Faders gained wheel and double-click-for-100% control.

Unavailable channels now keep a greyed strip reading n/a instead of hiding the
slider, so the console does not change shape when Rocksmith drops a channel.
Audio testing merged into Setup (playback device and guitar input side by side,
output buffer below) because both tabs were half empty; tabs are now Mixer,
Recording, Setup. StudioSelector paints the combo frame so the light Windows
border no longer breaks the dark theme, and AudioRoutingPanel.cs was normalised
to UTF-8 (four stray cp1252 middle dots had been rendering as separators only by
accident of the compiler falling back to the ANSI code page).

Tradeoffs: strip widths are fixed device pixels, which is consistent with the
rest of this window and with RSMods.exe being DPI-unaware (Windows scales the
whole window), but it means a future DPI-aware manifest would need these sizes
revisited. Colour-coding adds four hues; they are tied to channel identity in
one file rather than spread through the layout code.

Validation: clean x64 GUI rebuild, plus offscreen renders of all three tabs at
96 dpi covering connected, muted and unavailable channels. Live acceptance in
game remains to be checked.
