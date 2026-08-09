# Speaker Mode

Speaker Mode shifts the song instead of the guitar. It is intended for playing
through speakers, where the acoustic guitar is audible in the room.

Speaker Mode processes the song rather than the guitar input or the current
in-game tone, so it works identically with any input setup: a Real Tone Cable
or an ASIO interface through RS_ASIO. It requires no MultiPitch tone. Install
upstream RSMods first for its PSARC library and Wwise decode tools, then extract
the complete RSModsPlus release so its matching `RSMods.exe` can prepare the
temporary full-song audio.

Enable the feature in `RSMods.ini`:

```ini
[Drop Pedal]
EnableDropPedal = on
```

The `Engine` setting in the same section applies only to Drop Pedal mode's
input path and has no effect on Speaker Mode, which always uses the game's
Wwise music output. Leave `Engine` at its default (`automatic`) or set it for
your Drop Pedal setup as described in the
[ASIO](asio-drop-pedal.md) and [Cable](cable-drop-pedal.md) Drop Pedal guides.

For example, if the guitar is physically in E and the selected song is in Eb,
the overlay reads:

```text
Speaker: Eb -> E (+1)
```

The song is raised one semitone to E while the guitar remains untouched.

![Speaker Mode raising an Eb song to an E-standard guitar](images/overlay-speaker-mode.png)

In multiplayer the overlay still shows this one row: the shift applies to the
shared song mix, so there is no per-player state for a Player 2 row to control.

## Controls

| Action | Key |
|---|---|
| Select Off / Drop Pedal / Speaker Mode | `F7` |
| Preview / Drop Pedal target down / up | `,` / `.` |
| Physical guitar tuning (cycles E, Eb, D, ...) | `F9` |

The session starts Off. `F7` cycles in this order:

```text
Off -> Drop Pedal -> Speaker Mode -> Off
```

Speaker Mode physical-tuning and mode changes are locked once gameplay begins.
Choose the physical guitar tuning before leaving the pre-song tuner; Speaker Mode
reads the selected chart tuning from Rocksmith automatically.

The tuning keys work only while a pitch mode is selected and Rocksmith is the
focused window. Once Speaker Mode has read a chart in the pre-song tuner, the
comma and period target keys are locked because that target is owned by the
chart. State is session-only and starts at E for both physical and target tuning.

## Choosing the tunings

Use `F9` to tell the mod how the guitar is physically tuned; each press cycles
down one name, wrapping back to E. Comma and
period choose the target used for live song-list previews and Drop Pedal. When a
Speaker Mode song reaches the pre-song tuner, Rocksmith's chart tuning replaces
the pitch-class part of that preview target before the full-song render is
requested. A whole-octave selection is retained, so selecting `-12` before an
E-standard song produces `Speaker: E -> E (+12)` instead of collapsing to
unison. The signed interval and colour then describe the chart relative to the
physical guitar, even though Speaker Mode applies the inverse interval to the
song. Changing the physical tuning immediately recalculates the required audio
shift and render request.

Negative shifts are green, positive shifts are amber, and exact unison is white.

Examples:

| Guitar | Song | Speaker readout | Result |
|---|---|---|---|
| E | Eb | `Speaker: Eb -> E (+1)` | Song rises one semitone |
| Eb | D | `Speaker: D -> Eb (+1)` | Song rises one semitone |
| Eb | Eb | `Speaker: Eb` | No shift |
| E, one octave above the chart | E | `Speaker: E -> E (+12)` | Song and note detection rise one octave |
| Drop D (physically) | Drop Db | `Speaker: Drop Db -> Drop D (+1)` | Song rises one semitone |
| Open A (physically) | Open G | `Speaker: Open G -> Open A (+2)` | Song rises two semitones |

Non-uniform charts (drop tunings, open tunings, anything the game defines) are
supported through the same principle: the physical guitar must match the chart's
shape moved by the shift, and the pre-song tuner guides exactly that, because
its per-string targets already carry the shift. A Drop Db chart with an
E-standard base asks for D on the low string; an Open G chart asks for the Open
A shape. The shift is chosen so the strings most of the chart shares stay at
the physical base tuning -- a drop chart retunes one string, an open tuning a
few more. Shapes are named from the game's tuning list when a name exists.

This direction is deliberately opposite to Drop Pedal mode. `Drop: E -> Eb (-1)`
moves the guitar down to the song; `Speaker: Eb -> E (+1)` moves the song up to
the guitar. The signed number describes the audio direction in the selected mode
and is retained because note names repeat every octave:
`Drop: E -> E (-12)` is an octave shift, while exact unison remains simply
`Drop: E`.

## Audio behavior

Speaker Mode processes streamed preview and full-song music. Menu sound effects
are left alone. Previews use a 60 ms streaming analysis window because their
timing is not used for playing. Entering the pre-song tuner starts preparing the
selected full song in the background after Rocksmith's tuning label stabilizes,
and selecting another song cancels an
unfinished decode or render. The opening is prepared first, then later positions
render ahead while the song plays. Gameplay reads fixed-length processed PCM by
Wwise's exact song position, so Speaker Mode adds no rolling-window delay to the
full song and does not change its duration. Riff Repeater reads already prepared
positions immediately; an unusually early forward seek waits for its requested
range rather than playing the wrong pitch.

A song entered without a pre-song tuner frame (instant transitions) synchronizes
at the start of gameplay instead: the live shift and detection correct within
the first moments and the prepared audio takes over once ready.

The first use of a song/pitch combination may briefly wait for the opening if the
pre-song tuner did not provide enough preparation time. It does not wait for the
whole song. A prepared song can be reused for two minutes after leaving it, which
keeps quick back-and-forth selection natural. Speaker Mode holds the chosen pitch
and mode fixed throughout gameplay, so playback never has to wait for a second
render or change synchronization against the chart.

Prepared audio is session-temporary. The decoded WAV is deleted as soon as its
render worker finishes. The processed PCM is a delete-on-close mapped temporary
file retained only while active or during the two-minute cooldown. Retired audio
is capped at 256 MiB and oldest entries are discarded first. Nothing is reused
across Rocksmith launches; startup also removes debris left by a crash.

The live guitar input and tone are not pitch-shifted in Speaker Mode. The game's
tuning reference is adjusted so the chart can be played using the physical
guitar tuning.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `Pitch: Off` | Press `F7` twice from Off to reach Speaker Mode |
| Readout has no arrow | Physical and target tunings match exactly, so the interval is zero |
| Target keys do nothing in the pre-song tuner | Expected after chart synchronization; back out to select an octave offset, or use `F9` to correct the physical tuning |
| Keys do nothing | Rocksmith is not focused, the mode is Off, or Speaker Mode gameplay has locked pitch changes |
| Menu clicks do not change pitch | Expected; bank-backed menu SFX are excluded |
| The settings window opens when a song starts | The installed `RSMods.exe` is older than the RSModsPlus DLL; extract the complete release ZIP into the Rocksmith folder |
| Log says `RSMods.exe` or a decode tool is missing | Install upstream RSMods, then extract the complete RSModsPlus release ZIP into the Rocksmith folder |
| Song takes longer to enter the first time | The opening was not ready before leaving the tuner; later positions still render ahead |
| Pitch cannot be changed during a song | Expected; Speaker Mode locks it to preserve zero-added-delay synchronization |
| Music stays unshifted | Temporary-audio preparation or identity resolution failed; quit and attach `RSMods_debug.txt` to a bug report |
| Speaker Mode switches to Off | Preparation or processing failed; this prevents a silent wrong-pitch Speaker state. Quit and attach `RSMods_debug.txt` |
| Non-uniform chart plays but some strings read wrong | The guitar was not physically retuned to the shape the tuner showed; every string must match its tuner target |

The debug log is `RSMods_debug.txt` next to `Rocksmith2014.exe`. It is overwritten
on launch and locked while the game runs, so quit before copying it.
