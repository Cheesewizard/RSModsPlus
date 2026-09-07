# RSModsPlus

This branch prepares the Note by Note prerelease. See the [prerelease notes](docs/prerelease-notes.md) for installation and validation status.

If RSModsPlus has saved you time or made Rocksmith more enjoyable, consider
buying me a beer. Your support helps me keep improving the mod.

<a href="https://buymeacoffee.com/cheesewizard">
  <img src="docs/images/buy-me-a-beer-alt-amplifier-v2.png" alt="Buy me a beer" width="240">
</a>
<br><br>

A fork of [RSMods](https://github.com/Lovrom8/RSMods) that lets you play songs
in a different tuning without retuning your guitar:

- **Drop Pedal** shifts the guitar to match the song.
- **Speaker Mode** shifts the song to match the guitar.

`F7` cycles through Drop Pedal, Speaker Mode and Off. Both modes support shifts
from -24 to +24 semitones. RSModsPlus supports Rocksmith 2014 Remastered
(September 2022 update) and Learn & Play (December 2024 update). Drop Pedal
also supports two-player arrangements on both versions.

https://github.com/user-attachments/assets/c8951c94-e760-4830-a8f5-6b383dbb05da

## Quick start

### 1. Enable the feature

Close Rocksmith, then open `RSMods.ini` next to `Rocksmith2014.exe`. Make sure
it contains this block:

```ini
[Drop Pedal]
EnableDropPedal = on
Engine = automatic
```

If the `[Drop Pedal]` section is missing, add it manually. Seeing the Drop
Pedal settings in `RSMods.exe` does not guarantee that this block is already in
your INI file. Settings are read when Rocksmith starts, so restart the game
after changing them.

### 2. Choose the setup that matches your input

| Your setup | What you need before playing | Guide |
|---|---|---|
| **ASIO interface with RS_ASIO** | Nothing else. Stock and custom tones work normally | [ASIO Drop Pedal](docs/asio-drop-pedal.md) |
| **Real Tone Cable without RS_ASIO** | Nothing else. The mod captures and shifts the Cable input directly; stock and custom tones work normally | [Cable Drop Pedal](docs/cable-drop-pedal.md) |
| **Real Tone Cable with speakers** | Use Speaker Mode to shift the song while the physical guitar remains audible in the room | [Speaker Mode](docs/speaker-mode.md) |

> **Real Tone Cable users:** Modern Cable input is experimental and defaults to off.
> The following direct-capture guidance applies only when it is enabled. No special tone is required for that route. The mod captures the
> Cable input and shifts it before Rocksmith detects it, so stock and custom
> tones both work and the shift is independent of the selected tone slot. The
> modern Cable input path stands down automatically when RS_ASIO is installed,
> so ASIO users are unaffected.

Both inputs use one shared input-side shifter. There is no engine selector,
Cable engine, or tone fallback: RS_ASIO and the Real Tone Cable differ only in
how the device is opened, and everything downstream is the same.

### 3. Use it in Rocksmith

1. Start Rocksmith and press `F7` once. The top-left readout changes from
   `Pitch: Off` to `Drop: E`.

2. Match the readout to the song. Press `,` to move down one semitone or `.`
   to move up one semitone. For example, with a guitar in E standard and an Eb
   song, press `,` once until the readout says `Drop: E -> Eb (-1)`.

Other controls:

| Action | Player 1 | Player 2 |
|---|---|---|
| Cycle Drop Pedal / Speaker Mode / Off for everyone | `F7` | - |
| Move the target down / up | `,` / `.` | `Control+,` / `Control+.` |
| Tell the mod the guitar's physical tuning | `F9` | `Control+F9` |

Every session starts at `Pitch: Off`. If the keys do nothing or the mod seems
missing, the full [Quick Start guide](docs/quick-start.md) walks through the
installation checks and common problems.

---

## Drop Pedal

Shifts the **guitar** so a song in any uniform tuning can be played without
touching a tuning peg. To play an Eb song on an E-standard guitar, set the
pedal to -1. Audio and note detection move together, so the tuner and scoring
follow the shift.

![Downward shift applied](docs/images/overlay-drop-tuning-down.png)

One shared input-side shifter realises the shift for every input. The raw
instrument is captured and shifted before Rocksmith receives it, so every tone,
stock or custom, receives the shifted signal and no special tone is required.
Whether the input arrives through [RS_ASIO](https://github.com/mdias/rs_asio) or
a plain Real Tone Cable changes only how the device is opened; the shift,
detection, and tuner alignment downstream are identical.

Multiplayer is supported on both inputs: each player has an independent
target and base tuning (`Control` + the pedal keys addresses Player 2), with
`[Asio.Input.0]` as Player 1 and `[Asio.Input.1]` as Player 2 under ASIO.

![Independent targets in multiplayer](docs/images/overlay-multiplayer-tunings.png)

**Setup and usage:** the **[ASIO Drop Pedal guide](docs/asio-drop-pedal.md)**
for RS_ASIO interfaces, or the
**[Cable Drop Pedal guide](docs/cable-drop-pedal.md)** for Real Tone cable
setups.

---

## Speaker Mode

Shifts the **song** instead of the guitar. Intended for playing through
speakers, where the acoustic guitar is audible in the room: the music comes to
your tuning, and the guitar stays physically untouched.

![Speaker Mode raising an Eb song to an E-standard guitar](docs/images/overlay-speaker-mode.png)

- Requires no RS_ASIO, no audio interface, and no special tone; a Real
  Tone cable alone is enough.
- Gameplay audio carries **zero added latency**: the full song is
  pitch-rendered ahead of playback into temporary audio and read by exact song
  position.
- The chart tuning is read automatically at the pre-song tuner, and **any
  chart shape works**: uniform, drop and open tunings. For non-uniform
  shapes, the tuner guides the physical retune string by string (a Drop Db
  chart with an E-standard guitar asks for one string down to D, then raises
  the song a semitone: `Speaker: Eb Drop Db -> Drop D (+1)`).
- Prepared audio is session-temporary and deleted on close; nothing persists
  between launches.

**Setup and usage:** the **[Speaker Mode guide](docs/speaker-mode.md)**.

---

## What this fork adds

- The Drop Pedal, -24 to +24 semitones, with ASIO and Cable engines and full
  multiplayer support: independent per-player targets, base tunings and
  overlay rows.
- Speaker Mode, temporary full-song pitch rendering with zero added gameplay
  latency and automatic chart synchronization for any tuning shape.
- A base tuning setting, so shifts are named from whatever the guitar is
  physically in rather than from E.
- An on-screen readout of the current mode, route and per-player state, plus
  settings and rebindable keys in the settings app (Tuning tab).
- Optional Drop Pedal overlay colours for downward shifts, upward shifts and
  status text, configured in the settings app.

Everything else comes from RSMods 1.2.8.2 and behaves as upstream documents
it: extended range mode, custom song list titles, toggle loft, force
re-enumeration, GuitarSpeak, and the rest. See
[upstream's README](https://github.com/Lovrom8/RSMods#readme) for that list
and for the full `RSMods.ini` reference.

---

## Roadmap

These items are being investigated or developed. They are **not available in
the current release** and will only ship after focused tests and in-game
validation.

| Work | Status | Goal |
|---|---|---|
| **Note by Note** | Prerelease | Add a Riff Repeater practice mode that stops at each expected chart note and waits for you to play it correctly before continuing |
| **Raw Real Tone Cable processing** ([issue #18](https://github.com/Cheesewizard/RSModsPlus/issues/18)) | Delivered | Cable input is now captured and processed before Rocksmith's detection and tone systems, exactly like the ASIO path. Cable users get an ASIO-style setup with no special tone; the old MultiPitch-tone workaround is retired |
| **Volume on large ASIO shifts** ([issue #17](https://github.com/Cheesewizard/RSModsPlus/issues/17)) | Under investigation | Reproduce the reported volume loss on large downward shifts, identify whether it comes from the shifter or the Rocksmith tone, and fix the cause without applying a blanket gain boost |

The issue links contain the investigation scope and acceptance criteria. A
roadmap entry is a direction, not a promise that the reverse-engineering work
will prove practical.

---

## Installing

This is built from RSMods 1.2.8.2 and uses the same filename, so it replaces
RSMods' own `xinput1_3.dll` rather than sitting beside it. Only one of the two
can be loaded at a time.

Install upstream RSMods 1.2.8.2 first. RSModsPlus uses its existing settings,
libraries and decode tools.

Back up `xinput1_3.dll` and `RSMods\RSMods.exe` if you want to restore plain
RSMods later. Then download the ZIP from the
[latest release](https://github.com/Cheesewizard/RSModsPlus/releases) and
extract its complete contents into the Rocksmith 2014 folder. Allow it to
replace `xinput1_3.dll` and `RSMods\RSMods.exe`.

The updated settings executable also runs invisibly when Speaker Mode prepares
full-song audio. The rest of the existing `RSMods` folder and `RSMods.ini`
remain untouched.

To uninstall, restore those two files, or reinstall upstream RSMods.

Requirements are upstream's: Steam Rocksmith 2014 Remastered on Windows, and
the MS Visual C++ 2015-2019 redistributable. The ASIO engine additionally
needs [RS_ASIO](https://github.com/mdias/rs_asio) and an ASIO audio interface;
the Cable engine and Speaker Mode need neither.

---

## How it works

**ASIO Drop Pedal.** With RS_ASIO installed, the mod validates and attaches to the
capture endpoints RS_ASIO already created and gives each configured Rocksmith
input its own persistent pitch shifter. It does not start the driver early or
create a second ASIO host. Note detection, the tuner and tone processing all
consume the same shifted signal. The shifter uses period-synchronous splicing.
At 48 kHz with 128-frame callbacks, the production-shifter harness measures
roughly 6-20 ms of observable content delay depending on the note and shift.
This is added to the interface's normal round-trip latency. For comparison,
DigiTech does not publish a latency specification for its well-regarded Drop
pedal, but independent waveform tests report roughly
[12-17 ms](https://www.thefretboard.co.uk/discussion/107282/digitech-drop-tune/p2),
including a detailed burst test measuring about
[16 ms](https://www.reddit.com/r/audioengineering/comments/r3mecr/analyzing_the_digitech_drop_pedal/). The methods are not identical,
but they put this mod's measured delay in the same broad range as dedicated
hardware. End-to-end feel also depends on the interface's round trip, so a low
ASIO buffer remains important. Input formats `Float32`, `Int32`, `Int24` and
`Int16` and buffer sizes from 1 to 4096 frames are accepted, so common
interfaces work out of the box.

**Cable Drop Pedal.** Without RS_ASIO, the mod opens the Real Tone Cable through
a modern Windows audio path and captures its samples, then shifts that buffer
with the same input-side shifter the ASIO path uses before Rocksmith detects it.
No special tone and no in-game pitch pedal are involved, so stock and custom
tones both work. In multiplayer, each player's captured input and detection
reference are attributed to their owning player.

**Speaker Mode.** The mod hooks the game's Wwise music decoding. Menu previews
are shifted live; for gameplay, the selected song is decoded and pitch-rendered
in full into a temporary delete-on-close file, which playback reads by exact
song position, with zero added latency and no change to the song's duration. The
chart tuning is synchronized automatically, the game's tuning reference is
adjusted so the chart is playable in the physical tuning, and any preparation
failure turns the mode off rather than play at a wrong pitch.

---

## Documentation

### User guides

| Document | Covers |
|---|---|
| [docs/quick-start.md](docs/quick-start.md) | Installation, enabling the feature and first-use troubleshooting |
| [docs/asio-drop-pedal.md](docs/asio-drop-pedal.md) | ASIO Drop Pedal: requirements, controls, multiplayer, bass, troubleshooting |
| [docs/cable-drop-pedal.md](docs/cable-drop-pedal.md) | Cable Drop Pedal: tone setup, constraints, multiplayer, troubleshooting |
| [docs/speaker-mode.md](docs/speaker-mode.md) | Speaker Mode: setup, choosing tunings, chart shapes, troubleshooting |

### Designs

| Document | Covers |
|---|---|
| [docs/designs/drop-pedal-multiplayer.md](docs/designs/drop-pedal-multiplayer.md) | Drop Pedal multiplayer architecture, lifecycle and performance data |
| [docs/designs/speaker-mode.md](docs/designs/speaker-mode.md) | Speaker Mode engine internals |

### Investigation records

| Document | Covers |
|---|---|
| [docs/investigations/drop-pedal-multiplayer.md](docs/investigations/drop-pedal-multiplayer.md) | Earlier findings behind multiplayer pitch processing |
| [docs/investigations/speaker-mode.md](docs/investigations/speaker-mode.md) | The investigation that led to the Speaker Mode design |

### Technical reference

| Document | Covers |
|---|---|
| [docs/wwise-plugin-internals.md](docs/wwise-plugin-internals.md) | Reverse-engineered Wwise plugin structures used by the Cable engine |

---

## Support

If RSModsPlus has saved you time or made Rocksmith more enjoyable, consider
buying me a beer. Your support helps me keep improving the mod.

<a href="https://buymeacoffee.com/cheesewizard">
  <img src="docs/images/buy-me-a-beer-alt-amplifier-v2.png" alt="Buy me a beer" width="240">
</a>

---

## Issues

Drop Pedal and Speaker Mode problems go
[on this repository](https://github.com/Cheesewizard/RSModsPlus/issues), not on
upstream's tracker. These features aren't theirs to support.

A bug in an inherited RSMods feature that reproduces on a stock upstream build
belongs [upstream](https://github.com/Lovrom8/RSMods/issues).

A debug log is written to `RSMods_debug.txt` beside `Rocksmith2014.exe`. It is
overwritten on every launch and locked while the game runs, so quit before
copying it. Attaching it makes a bug report far easier to act on.

---

## Credits

RSMods is the work of **Lovrom8** and **ffio1**, with contributions from
ZagatoZee, Kokolihapihvi and L0fka. This fork is the pitch routing on top of
their project. If you find the rest of the mod suite useful, thank them.

[RS_ASIO](https://github.com/mdias/rs_asio) by **mdias** is what makes the
ASIO engine possible; the ASIO Drop Pedal lives underneath it.

[Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch)
by **Signalsmith Audio** performs Speaker Mode's pitch shifting.

The reference-frequency technique the Cable Drop Pedal relies on is the same
one CDLC charters have long used to move a chart's expected notes by setting
an arrangement's tuning pitch.
