# RS_ASIO Drop Pedal

The Drop Pedal shifts the live instrument before Rocksmith consumes it. To play
an Eb song with a guitar in E standard, select Drop Pedal and press `,` once.
The readout becomes `Drop: E -> Eb (-1)`.

RS_ASIO and the Real Tone Cable use the same input-side shifter. There is no
engine selection, promotion, Cable fallback, or special-tone requirement; they
differ only in how the input device is opened.

## Before you start

- Set up [RS_ASIO](https://github.com/mdias/rs_asio) and confirm the unshifted
  instrument works in Rocksmith.
- Enable the feature using the [Quick Start](quick-start.md).
- A single-player setup needs no extra Drop Pedal channel setting.
- For two players, configure both Rocksmith inputs in `RS_ASIO.ini` as normal.

## How it works

The mod attaches to the capture endpoint RS_ASIO already created. It does not
start another ASIO host or open the driver a second time. Each captured buffer
passes through the shared Drop Pedal processor immediately before Rocksmith
receives it. The tuner, note detection, and tone chain therefore consume the
same shifted signal.

At a zero-semitone target or while disabled, the shifter bypasses pitch
detection and splicing. A non-zero shift adds a small, note-dependent content
delay because the shifter must observe part of the waveform before producing a
stable shifted period. The interface driver and buffer size add their own
latency.

## Controls

| Action | Player 1 | Player 2 |
|---|---|---|
| Pitch down / up | `,` / `.` | `Control+,` / `Control+.` |
| Base tuning | `F9` | `Control+F9` |
| Select Drop Pedal / Speaker Mode / Off | `F7` | `F7` |

- `F7` cycles `Drop Pedal -> Speaker Mode -> Off -> Drop Pedal`.
- Keys register only while Rocksmith is focused.
- While pitch processing is Off, every key except `F7` is ignored.
- Keys are rebindable in the settings app under **Tuning**.

In multiplayer each row shows that player's own target. Nothing is saved
between sessions: pitch processing starts Off, target 0, and base E standard.

## Emulated bass

Player identity follows the Rocksmith input slot, not the selected arrangement.
Switching Player 1 from Lead to Emulated Bass or Physical Bass keeps the same
input route and shifter.

- Emulated Bass from a guitar: use the song tuning offset only; Rocksmith adds
  the octave.
- Physical Bass from a bass: use the song tuning offset only.
- Physical Bass from a guitar: include the octave in the target, such as `-12`,
  and configure Rocksmith for physical bass input.

## Troubleshooting

| Symptom | Meaning |
|---|---|
| `Input: Waiting for capture` | RS_ASIO has not created a supported capture endpoint yet |
| Guitar works but pitch does not change | Confirm Drop Pedal mode is selected and the target is not 0 |
| Wrong interface channel shifts | Change `Channel =` under the corresponding input in `RS_ASIO.ini` |
| No output device on launch | Restore the interface to 48 kHz and restart Rocksmith |
| Player 2 input unavailable | `[Asio.Input.1]` is not configured or has not opened |
| PortAudio attachment never validates | The installed RS_ASIO capture patch is incompatible with this build |

Logging is opt-in. The log is `RSMods_debug.txt` next to
`Rocksmith2014.exe`; it is overwritten on every launch and may be locked while
the game is running.
