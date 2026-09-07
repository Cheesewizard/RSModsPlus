# Real Tone Cable Drop Pedal

The Drop Pedal shifts the live guitar signal without touching a tuning peg and
keeps Rocksmith's tuner and note detection aligned with the shift. The range is
-24 to +24 semitones. To play an Eb song on an E-standard guitar, select Drop
Pedal and press `,` once.

The Real Tone Cable and RS_ASIO use the same input-side shifter. There is no
Cable engine, ASIO engine, tone fallback, or engine selector. The mod intercepts
the captured input before Rocksmith consumes it, shifts that buffer, and passes
the result through Rocksmith's normal input path.

A Real Tone Cable does **not** need a special tone or a tone-slot key press.
Tone changes and song loads do not own the pitch shift.

## Low-latency Cable input

Rocksmith contains an old PortAudio/WASAPI format-negotiation defect. Some Real
Tone Cable drivers accept their native exclusive format as 48 kHz, 16-bit mono
`WAVEFORMATEXTENSIBLE`, while Rocksmith retries the same format using the legacy
PCM description and rejects the Cable.

RSModsPlus corrects that host format at the driver boundary. Rocksmith still
receives its requested 32-bit float samples, so the Drop Pedal and note-detection
pipeline do not need a second sample format.

For lowest end-to-end latency, enable **Exclusive Mode (lower latency)** in the
RSMods settings app and restart Rocksmith. When Exclusive Mode is disabled,
Rocksmith output uses the slower Windows shared route and RSModsPlus writes a
clear warning to `RSMods_debug.txt`.

The exclusive-format correction is direct. It does not silently switch a failed
exclusive Cable to shared mode. If the driver rejects its native format, the log
reports the driver error and the stream fails visibly.

## Experimental Modern Cable input

Modern Cable input is parked for this release and is **off by default**. It
remains available for testing through **Enable Modern Cable input for testing
(experimental)** in the settings app, or `ModernCableInput = on` under
`[Mod Settings]` in `RSMods.ini`. Restart Rocksmith after changing it.

When off, this feature does not install its input-opening hook or request its
system timer adjustment. An existing explicit `ModernCableInput = on` setting
is preserved; turn it off to stop using the experimental path. When RS_ASIO is
installed, Modern Cable input stands down.

Other Windows audio input devices can use the experimental capture path.
Third-party compatibility is unverified, so we cannot guarantee your device
will work. This does not confirm removal of other device restrictions in
Rocksmith.

## Controls

| Action | Player 1 | Player 2 |
|---|---|---|
| Pitch down / up | `,` / `.` | `Control+,` / `Control+.` |
| Select Drop Pedal / Speaker Mode / Off | `F7` | `F7` |
| Base tuning | `F9` | `Control+F9` |

- `F7` cycles `Drop Pedal -> Speaker Mode -> Off -> Drop Pedal`.
- Keys register only while Rocksmith is focused.
- While pitch processing is Off, every key except `F7` is ignored.
- Keys are rebindable in the settings app under **Tuning**.

The overlay shows `Input: Ready` once the captured input is attached. If the
stream is not available it says `Drop: Input unavailable`; it never asks for a
pedal in the current tone.

Every launch starts with pitch processing Off, target 0, and base E standard.

## Base tuning

`F9` tells the display what the guitar is physically tuned to. It cycles
`E -> Eb -> D -> ... -> F -> E`. It changes the displayed tuning names, not the
audio shift. Leave it at E unless the guitar is physically tuned differently.

## Note detection and true tuning

The shifted buffer is the same buffer Rocksmith receives for its tuner and note
detection, so there is no separate Cable reference offset to maintain. Authored
non-A440 tunings remain Rocksmith's reference and the shifted signal is measured
against them. Target changes affect the next captured audio buffers, including
the tuner path.

## Latency and crackling

Pitch shifting adds a small, note-dependent delay because the shifter must
observe part of the waveform before producing a stable shifted period. That is
separate from Rocksmith's input and output buffering.

If the guitar is already late at target 0, check the base audio route first:

- **Shared Mode (higher latency)** means Windows is buffering Rocksmith's
  output. Enable Exclusive Mode to use the lower-latency route.
- A larger `LatencyBuffer` is more stable but slower.
- A smaller buffer can reduce delay but may crackle if the PC cannot service it
  consistently.
- `Win32UltraLowLatencyMode` affects Rocksmith's output path, not the Drop Pedal
  algorithm.

Do not describe ordinary menu or device-startup pops as a steady-state Drop
Pedal failure. Test a sustained clean note after the audio devices have settled.

## Troubleshooting

| Symptom | Meaning |
|---|---|
| `Drop: Input unavailable` | The capture stream has not attached; inspect `RSMods_debug.txt` |
| `Input: Ready` but no audible change | Confirm Drop Pedal mode is selected and the target is not 0 |
| Cable rejected with an exclusive-format error | The driver did not accept native 48 kHz/16-bit mono; there is no shared fallback |
| Warning says shared output is slower | Enable Exclusive Mode in the settings app and restart |
| Crackles continue after startup | Increase the audio buffer one step or investigate DPC/device contention |
| Pitch keys do nothing | Rocksmith is unfocused, pitch processing is Off, or Speaker Mode has locked the controls |

Logging is opt-in. The log is `RSMods_debug.txt` next to
`Rocksmith2014.exe`; it is overwritten on every launch and may be locked while
the game is running.
