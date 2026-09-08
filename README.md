# RSModsPlus

**Spend more time playing, less time retuning. Practise difficult parts at your own pace.**

RSModsPlus builds on [RSMods](https://github.com/Lovrom8/RSMods) with pitch shifting
and new practice tools for Rocksmith 2014 on Windows.

- **Drop Pedal** — shift your guitar to match the song.
- **Note by Note** — practise at your own pace, with machine learning assisted note detection.
- **Speaker Mode** — shift the song to match your guitar.
- **Audio bridge** — mixing, recording and a steadier audio setup.

[Download](https://github.com/Cheesewizard/RSModsPlus/releases) ·
[Getting started](#getting-started) ·
[Report a problem](https://github.com/Cheesewizard/RSModsPlus/issues)

> This README describes the latest development work. Available features and
> installation steps depend on the build; check the notes accompanying your download.

<!-- MEDIA: Add a short overview video here, showing Drop Pedal, Speaker Mode,
and Note by Note. Paste the uploaded GitHub video URL on its own line.
The existing README's video can be reused after checking it matches the new build. -->

## Drop Pedal

Play songs in different tunings without retuning every string. Shift your guitar
up or down by as much as 24 semitones, with the guitar sound, tuner and note
detection following the same shift. Stock and custom tones work normally;
no special tone setup is needed.

For example, with your guitar in **E standard**, select **−1** to play an
**Eb-standard** song.

Playing together? Each player has their own target and physical tuning setting.
The on-screen readout shows the tuning for each player, and you can customise
its colours in the settings app.

A pitch shift moves every string by the same interval. Changing from standard
tuning to a drop or open tuning still requires changing the relevant strings.

<!-- MEDIA: Screenshot of the current Drop: E -> Eb (-1) readout during a song.
Optional second screenshot: two-player readout with different targets.
Use fresh captures if the existing images show obsolete engine labels. -->

## Note by Note

Work through difficult passages one note or chord at a time. Note by Note holds
playback at the next target and continues when your playing is accepted, giving
you time to find the shape and practise the transition.

**Machine learning assisted detection** works alongside Rocksmith's native
detection and additional pitch checks to help recognise the note you are playing.
It can help confirm notes the native detector misses and reject confident
wrong-note readings. These checks work together automatically within Note by Note.

Open **Riff Repeater**, choose the section you want to practise, and turn on
**Note by Note** in its menu. Turn it off there to return to normal playback.

<!-- EDITOR: Confirm the release package installs the Note by Note menu entry
before publishing this as release documentation. -->

<!-- MEDIA: This feature needs a video. Show enabling the Riff Repeater menu
option, a note waiting, the correct note releasing playback, and a chord target.
Include a clear example of a correctly played note being recognised; do not
attribute it to machine learning without confirming that in the recording.
Add a still of the menu if the control is hard to see. -->

## Speaker Mode

Bring the music to your guitar's tuning. Speaker Mode shifts the song, making
it useful when playing through speakers and you can hear your physical guitar
in the room.

With your guitar in **E standard** and a song in **Eb**, the song moves **up one
semitone**. The pre-song tuner reads the chart's tuning automatically and guides
you through any string changes needed for drop or open tunings.

Song audio is prepared ahead of playback, so pitch processing adds no playback
delay during the song. The opening may take a moment to prepare. Choose your
physical tuning before starting; Speaker Mode locks pitch changes during gameplay.

[Speaker Mode guide](docs/speaker-mode.md)

<!-- MEDIA: Short before-and-after video using the same song passage, with the
physical guitar audible. Keep the Speaker: Eb -> E (+1) readout visible. -->

## Audio bridge

RSModsPlus can carry Rocksmith's playback itself instead of leaving it to the
game, which is what makes mixing, recording and device changes possible while a
song is running. The bridge opens from the settings app and talks to the running
game, so changes apply as you play.

**Mixing.** The mixer carries the seven channels Rocksmith mixes: **master**,
**both players**, **song**, **effects**, **voice-over** and **microphone**. Turn
the backing track down to hear yourself, or mute a channel outright. This moves
playback volume only; your guitar signal and note detection are untouched.

**Recording.** Takes capture the game's own audio to WAV, or to MP4 alongside
the Rocksmith window, so a recording sounds exactly like what you heard.

**A steadier setup.** Choose which device the game plays through without
restarting it, and test the output buffer to find a period the device holds
cleanly instead of guessing at one.

[Design notes](docs/designs/shared-audio-routing.md)

<!-- MEDIA: Screenshot of the Mixer tab with a take running, plus a short clip
of a recorded take played back. -->

## Getting started

1. Close Rocksmith and the settings app.
2. Install [RSMods](https://github.com/Lovrom8/RSMods) 1.2.8.2 first; RSModsPlus
   uses its libraries and decode tools.
3. Back up `xinput1_3.dll` and `RSMods\RSMods.exe`, then extract the complete
   RSModsPlus release ZIP into the folder containing `Rocksmith2014.exe`.
   Allow those files to be replaced.
4. To enable Drop Pedal and Speaker Mode, make sure `RSMods.ini` contains:

   ```ini
   [Drop Pedal]
   EnableDropPedal = on
   ```

5. Start Rocksmith. Press **F7** for Drop Pedal, or press it again for Speaker Mode.
   Use **F9** to set your guitar's physical tuning, then **,** / **.** to choose
   the Drop Pedal target.

Each launch starts at **Pitch: Off**. Restart the game after editing the INI.
RSModsPlus replaces the original RSMods DLL; to return to upstream RSMods,
restore your backups or reinstall it.

[Installation checks and troubleshooting](docs/quick-start.md)

## Controls

These are the default keys; pitch controls can be rebound in the settings app.

| Action | Control |
|---|---|
| Cycle Drop Pedal → Speaker Mode → Off | **F7** |
| Lower / raise the Drop Pedal target | **,** / **.** |
| Set the guitar's physical tuning | **F9** |
| Change Player 2's Drop Pedal target or physical tuning | Hold **Ctrl** with the corresponding key |
| Enable / disable Note by Note | **Riff Repeater → Note by Note** |
| Customise the pitch readout colours | **Settings app → Enable / Disable Mods → Tuning** |

Pitch keys work while Rocksmith is focused and a pitch mode is selected.
Speaker Mode follows the chart automatically at the pre-song tuner; its song
shift is shared by both players.

<!-- MEDIA: Optional screenshot of the Tuning settings, showing colour controls.
Keep the screenshot tightly cropped so the relevant controls remain readable. -->

## More from RSMods

RSModsPlus also includes the original mod suite, including extended range mode,
custom song list titles, toggle loft, force re-enumeration and GuitarSpeak.
See the [upstream README](https://github.com/Lovrom8/RSMods#readme) for the full list
and settings reference.

## Help and feedback

Report RSModsPlus problems and feature requests on
[this repository's issue tracker](https://github.com/Cheesewizard/RSModsPlus/issues).
Include your build version, what you were doing and what happened.

For troubleshooting, quit the game and copy `RSMods_debug.txt` from the Rocksmith
folder before starting it again; the log is overwritten on each launch.
Problems that also occur in unmodified RSMods belong on the
[upstream tracker](https://github.com/Lovrom8/RSMods/issues).

## Support

I'm currently looking for work, and I put a lot of time and care into RSModsPlus.
If it's made your time with Rocksmith a little more enjoyable, any donation would
mean a lot to me — especially while I'm looking for my next opportunity.

There's no pressure to donate. Thank you for playing, sharing feedback and giving
something I've worked hard on a place in your practice routine.

**[Support me with a donation](https://buymeacoffee.com/cheesewizard)**

If you know of a C# or Unity development opportunity, I'd be grateful if you'd
keep me in mind, too.

## Credits

Built on **RSMods** by **Lovrom8** and **ffio1**, with contributions from
ZagatoZee, Kokolihapihvi and L0fka.

Speaker Mode uses [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch)
by **Signalsmith Audio**. Thanks to the Rocksmith modding and custom-song community
for the work this project builds on.
