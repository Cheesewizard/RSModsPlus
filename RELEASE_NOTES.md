# Rocksmith Audio Bridge Release Notes

## Release
- **Version:** `v4.0.0`
- **Build date:** `TBD` (set from the final verified build)
- **Scope:** `Pre-release`

> Note by Note is a **beta** feature toggled from the menu, and USB cable compatibility is **experimental**.

## Feature list

### New name: Rocksmith Audio Bridge
- RSModsPlus is now called **Rocksmith Audio Bridge**, because the old name was easily mistaken for Rocksmith+. The Audio Bridge icon is the product logo. The settings window title reads "Rocksmith Audio Bridge 4.0 (based on RSMods 1.2.8.3)", its tab and footer use the new name, and the in-game watermark and overlay show it too. The in-game overlay shows the version (4.0) in its bottom-right corner, and the log header reads "Rocksmith Audio Bridge 4.0".
- The package is now four files in the Rocksmith folder: `xinput1_3.dll`, `RocksmithAudioBridge.dll`, `RocksmithAudioBridgeAsio.dll` and `RSMods.exe`. The `%LOCALAPPDATA%\RSModsPlus` logs folder, the default `Videos\RSModsPlus` recordings folder, `RSMods.ini` settings and the registry entries are unchanged.

### Audio Bridge (new)
The Audio Bridge has two places to control it: the **in-game overlay** (press `\`) for everything you use while playing, and the **Rocksmith Audio Bridge page in RSMods** for setup with the game closed. There is no separate desktop program.

**RSMods page (game closed)**
- Power switch: turns the bridge on or off. The bridge taps the game output rather than replacing it, so turning it off never forces an ASIO downgrade.
- Input switch: Cable or ASIO. Cable mode only renames the RS_ASIO files and never rewrites `RS_ASIO.ini`.
- ASIO bridge driver: an **Install** / **Uninstall** button (one Windows admin prompt).
- Recordings folder.

**ASIO bridge driver (optional)**
- A virtual ASIO device that sits between RS_ASIO and your real interface, passing audio through while tapping it. It enables ASIO game-mix recording and switching the game output from the overlay. The driver file ships in the package but nothing is registered with Windows until you press **Install** on the RSMods page.
- RS_ASIO setup remains user-owned. Nothing ever rewrites `RS_ASIO.ini`. To put the driver in the signal chain, set the relevant output and paired input `Driver` entries to `Rocksmith Audio Bridge ASIO`; removing the driver also leaves the file untouched.
- Interface recovery (ASIO mode with the driver configured in RS_ASIO): booting without the interface keeps Rocksmith available through a shared-mode Windows output, with Real Tone Cable fallback when one is present. Reconnecting the interface does not switch automatically; pick it on the overlay's Output page to move back to ASIO without restarting the game.

**In-game overlay (press `\`)**
- The Rocksmith Audio Bridge control centre, branded with the Audio Bridge logo: a sidebar of pages (Note by Note, Mixer, Input, Output, Record, General, Drop Pedal), switch toggles, sliders with their value always readable, LED level meters, channel-strip faders and a record button with a live timer. Every control has a hover tooltip. The window sizes itself to the open page (up to 92% of the screen height, width adjustable) and the mouse does not reach the game while the cursor is over it. A REC indicator in its header shows on every page while recording.
- Mixer: seven channels. Master first, then Player 1 guitar and Player 2 guitar, then Song (backing track), Microphone, Voice-over and Effects. Player 1 and Player 2 levels are playback only and never affect note detection. Faders are session-only playback levels by design.
- Input: live guitar level meter on a dB scale (separate "input 1" and "input 2" bars in multiplayer) and guitar input conditioning for any input path (ASIO interface, Real Tone Cable, or a third-party cable), running on the Player 1 input before the game's amp and note gate: make-up gain for a quiet input; a noise suppressor that reduces between-note hiss without cutting sustain; an input compressor that evens out string-level variation; a 50/60 Hz mains-hum notch filter; and a Rocksmith gate override that sets the game's own noise floor (P1_NoiseFloor). The noise suppressor and the Rocksmith gate can be used separately or together.
- Output: names the device the game is playing through (for example "Playing to: M-Track (direct)") and lists your playback devices to move the game's sound to, live, without restarting. It works with the bridge driver in the ASIO chain (and switches back to the ASIO interface from the same list) and in Cable mode. The switch lasts until you close Rocksmith. The Windows device of the interface RS_ASIO is using is shown but locked, because playing to it would silence the interface. If the new device does not start playing within a few seconds, the overlay switches back on its own. Output buffer size (Cable mode) is also here.
- Output protection: an optional look-ahead limiter caps the digital output at the selected ceiling. It is off by default and does not replace setting a safe interface or headphone volume.
- Record: the Record button or the record hotkey (F6 by default; change it on the overlay's Record page, which refuses keys the game, Windows, Steam or another RSMods shortcut already use) captures paired game-mix and dry guitar takes, or video. Video is captured by a hidden recorder (RSMods.exe with no window) that saves an MP4 with the game sound when you stop; the picture starts about three seconds after the sound while the recorder starts up. Cable/shared-output game-mix recording uses the native render tap; ASIO game-mix recording requires the bridge driver to be installed and configured. Dry recording follows the active Player 1 input. A red REC indicator appears in the top right of the game while recording.
- Note by Note: shows or hides the detection readout, sets the readout and target sizes, moves the target Left, Center or Custom, sets line spacing, and edits the three detection colours (text, green for a correct read, amber when a detector helped pass a note but read a slightly different pitch) with a live preview. With the page open, each HUD block can be dragged, resized with the mouse wheel and reset with a double-click.
- Drop Pedal: shows the current tuning, mode and shift with your pedal shortcuts, turns the pedal on or off (applies at the next launch) and edits its three readout colours live with a preview.
- General: force update song list re-scans the dlc folder live, so Rocksmith picks up a psarc you just added without a restart.
- Overlay changes to Note by Note, Drop Pedal, input conditioning and output protection are saved to `RSMods.ini` and survive a relaunch.

### USB cable compatibility (experimental)
- Converts the cable's sample rate to the 48 kHz Rocksmith expects, so a cable that runs at a different rate is still compatible.
- Reads the cable as soon as Windows has new audio, instead of on a fixed timer, so the input delay stays consistent rather than changing with whatever else is running on the PC.

### Note by Note practice mode (beta; toggle in the menu)
- In-process, per-note detection built as a single tuned blend of three components that always work together for accuracy: the game's native pitch/chord matcher, the enhanced raw-pitch verifier, and the FretNet ML detector. None of the three is optional; the bundled FretNet service is auto-launched as part of detection, and all three feed one accept decision.
- Fretboard note cues during Riff Repeater (detector feedback colouring on the cues is not currently working, see Known bugs #90).
- Bend detection and visualization groundwork.

### Drop Pedal
- Cable users now get native Drop Pedal support: no more multipitch pitch-shifting, lower latency, and any tone can be used.
- Configurable overlay colours.
- Reduced down-shift latency (measured).

### HUD and general
- HUD pitch labels spelled the way guitarists read them; corner status removed.

## Known bugs
Severity / impact should be set per entry. Issue numbers reference github.com/Cheesewizard/RSModsPlus.

- **[High]**
	- No audio when playing notes in dense passages.
	- #63 Speaker Mode emits a piercing high-pitched tone on song load under CPU load (buffer underrun; headphone safety risk).

- **[Medium]**
	- Fretboard visual glitches: incorrect note fingers and frozen animations. Currently offset by a custom-UI readout of the target notes shown separately from the fingerboard.
	- #82 Drop Pedal does not account for mixed ASIO and Real Tone Cable multiplayer inputs.
	- #65 Riff Repeater timeline does not show the orange progress/current-section overlay on the gray timeline unless the section is highlighted (only purple is shown otherwise).
	- #58 Note by Note: ringing chords satisfy repeat-strum sections without a re-pick.
	- #57 Auto-tuning sets the Drop Pedal / Speaker Mode offset too late (must apply at song-list scroll, before load).
	- #56 In-game pause-screen tuner flickers between adjacent tunings (E/Eb) under a tuning offset.

- **[Low]** (mostly Note by Note visual polish; only affect the beta feature when enabled)
	- Gray notes on loop restart that clear once the first note is played.
	- #90 Fretboard note cues do not show detector feedback colours (no visual detector feedback on the cues).
	- #60 Top strings flicker / wrong string animation during a held plain note.
	- #47 After a loop turnover the restarted pass renders all notes gray.
	- #44 Bend visuals wrong during the hold (floating marker, plain-note repaint).
	- #42 Bends can starve waiting for confirmation.
	- #38 Bend on high E fret 15 triggers the open-E success animation.
	- #37 One-frame flash on note markers still occurs.

## Bug fixes
- #94 More ASIO audio devices are now supported. The input hook now waits for the RS_ASIO capture path to become ready instead of relying on a narrow startup timing window, fixing interfaces whose audio path initialized too late to be hooked.
- Speaker Mode now works with custom DLC built on newer Wwise versions (DLC Builder / Wwise 2022/2023). The shipped WEM decoder rejected unrecognized RIFF chunks (e.g. `akd`), so Speaker Mode failed on those songs even though Rocksmith played them fine; the WEM is now rewritten keeping only the chunks the decoder recognizes. This was never fixed before this release.
- #23 Custom string colours now match note colours (user-reported).

## Test checklist

Tick each item on the final release build (Master, installed from the package, not a dev deploy).

### Install and branding
- [ ] Upgrade over v3.2.1: the 4-file package (`xinput1_3.dll`, `RocksmithAudioBridge.dll`, `RocksmithAudioBridgeAsio.dll`, `RSMods.exe`) installs into the game folder and the old `RSMods\RSMods.exe` does not conflict.
- [ ] Game boots, log header reads "Rocksmith Audio Bridge 4.0".
- [ ] RSMods window title "Rocksmith Audio Bridge (RSMods 1.2.8.3)", tab and footer use the new name.
- [ ] In-game watermark and overlay show the new name; overlay shows "v4" bottom right.
- [ ] Existing `RSMods.ini` settings from v3.2.1 are still picked up after upgrading.

### RSMods Audio Bridge page (game closed)
- [ ] Page is a single page with no sidebar.
- [ ] Power rocker turns the bridge on and off; state survives a relaunch.
- [ ] Input rocker switches Cable / ASIO; Cable mode only renames RS_ASIO files, `RS_ASIO.ini` is byte-identical afterwards.
- [ ] ASIO bridge driver **Install** registers it (one admin prompt) and the button turns to **Uninstall**; **Uninstall** removes it.
- [ ] Declining the admin prompt leaves everything unchanged.
- [ ] Recordings folder row changes the folder and the next take lands there.
- [ ] A setting changed in game (written as `Key=Value`) is not reverted when RSMods saves.

### In-game overlay shell (`\`)
- [ ] Opens and closes with `\`; logo, sidebar pages (Note by Note, Mixer, Input, Output, Record, General, Drop Pedal); Debug page absent in the public build.
- [ ] Window height fits each page without scrolling (max 92% of screen); width adjustable.
- [ ] Every control shows a hover tooltip.
- [ ] Mouse wheel, movement and clicks over the panel do not reach the game (log: "DirectInput mouse capture installed").
- [ ] REC indicator in the header on every page while recording.

### Mixer
- [ ] Master first, then players, then game audio.
- [ ] Each fader (Master, Song, P1, P2, Mic, Voice-over, Effects) changes only its channel.
- [ ] P1/P2 faders do not change note detection.
- [ ] Faders reset on relaunch (session only, by design).

### Input conditioning
- [ ] Input meter moves with the guitar on a dB scale; strum fills it.
- [ ] Two-player: separate "input 1" / "input 2" bars, second only while multiplayer.
- [ ] Make-up gain raises a quiet input.
- [ ] Noise suppressor cuts hiss between notes without cutting sustain.
- [ ] After a pause, soft and normal picking come through immediately (suppressor opens 6 dB over threshold, no "one loud hit to wake it").
- [ ] Compressor evens out string levels.
- [ ] Hum notch removes 50/60 Hz ground-loop hum.
- [ ] Rocksmith gate override (P1_NoiseFloor) changes the game gate.
- [ ] All of the above survive a relaunch (saved to `RSMods.ini`).
- [ ] Works on ASIO interface, Real Tone Cable, and a third-party cable.

### Output
- [ ] Output page names the real device ("Playing to: ...").
- [ ] Switching to another playback device moves the game sound live, with the ASIO bridge in the chain and in Cable mode.
- [ ] Switching back to the ASIO interface from the same list works.
- [ ] The RS_ASIO interface's Windows device is shown locked ("Used by ASIO") and cannot be picked.
- [ ] A device that fails to start reverts automatically within a few seconds.
- [ ] The switch is forgotten on relaunch.
- [ ] Output buffer size (Cable mode) applies.
- [ ] Output protection limiter: off by default; when on, output never exceeds the ceiling; settings survive a relaunch.

### Interface recovery (ASIO with the bridge driver in RS_ASIO)
- [ ] Boot with the interface unplugged: game still starts on a Windows output (Real Tone Cable fallback if present).
- [ ] Reconnect, pick the interface on the overlay Output page: back on ASIO without restarting.
- [ ] *(regression #94)* Interface whose audio path starts late still gets hooked.

### Recording (F6 and overlay Record button)
- [ ] F9 cycles the Drop Pedal base tuning and no longer starts a recording; an old RSMods.ini with both on F9 moves recording to F6 on launch.
- [ ] Overlay Record page: Change key captures the next key; F8, F10, F11, F12 and keys used by other shortcuts are refused with a reason; the new key works at once and survives a relaunch.
- [ ] Audio take: game mix + dry guitar saved as a paired wet + dry take.
- [ ] Cable/shared output game-mix recording works without the ASIO driver.
- [ ] ASIO game-mix recording works with the driver registered and in RS_ASIO.
- [ ] Video take: MP4 with game sound, picture starts roughly 3 s after the sound.
- [ ] Audio/Video choice is remembered.
- [ ] Red REC indicator top right of the game while recording.
- [ ] Hidden recorder is not killed by antivirus (check with Bitdefender ATD on).

### General / diagnostics
- [ ] Force update song list picks up a newly copied psarc without a restart.

### USB cable compatibility (experimental)
- [ ] Cable at a non-48 kHz native rate is captured and plays in game.
- [ ] No exclusive-mode errors; Cable mode leaves `RS_ASIO.ini` untouched.

### Drop Pedal
- [ ] Overlay Drop Pedal page shows tuning, mode, shift and shortcuts.
- [ ] On/off toggle applies at next launch.
- [ ] Three readout colours change live with preview.
- [ ] Native Drop Pedal works on cable (any tone, no multipitch shifting).
- [ ] F7 pressed mid-song while Speaker Mode is locked flashes the mode line orange, even with the readout hidden.

### Speaker Mode
- [ ] Custom DLC built on Wwise 2022/2023 (DLC Builder) plays in Speaker Mode.
- [ ] Known #63: note any high-pitched tone on song load under CPU load (keep headphones low).

### Note by Note (beta)
- [ ] Toggle on from the menu; FretNet service starts on its own (no Python, no model download); `ml-service.log` written.
- [ ] Overlay Note by Note page: show/hide readout, readout and target size, target Left/Center/Custom, three detection colours with live preview; all saved to `RSMods.ini`.
- [ ] HUD layout editor: with the page open, drag and wheel-resize each HUD block; double-click resets; line spacing slider works; only Custom position drags; spot remembered when switching modes.
- [ ] Target string-colour swatch follows the target size, not the readout size.
- [ ] HUD goes green only for the detector(s) that passed the note, no target flash; chords show green too; works in Speaker Mode.
- [ ] After a pass, a detector row is green only when the note it shows is the passed note; a row that helped pass it but read a neighbouring pitch is amber. All three rows always describe the same note.
- [ ] After a chord pass, each detector that matched it shows the chord's root note (a D, Dsus2 or D/F# chord reads "D") instead of "chord".
- [ ] Single notes, fast repeated notes (a burst of quick re-picks all count).
- [ ] Power chords accept on the first clean strum.
- [ ] Full chords and dyads accept; single note straight after a chord (chord tone re-pick) is not stalled.
- [ ] Fast single-then-chord lick: the chord accepts on the strum that cut the single short.
- [ ] Hammer-ons and pull-offs accept without re-picking.
- [ ] Bends: meter needle moves smoothly (no teleporting), half bends reach without overbending, bends are not accepted unbent; also in Speaker Mode.
- [ ] Loop restarts in Riff Repeater continue scoring.

### Regressions from earlier fixes
- [ ] #23 Custom string colours match note colours.
- [ ] Speaker Mode PSARC scanning and packaging from v3.2.0/v3.2.1 still work.

## Validation status

- The automated Audio Routing suite passes on the current source, including missing-output startup, device loss and recovery, recording lifecycle, Cable/ASIO routing policy, and the rule that the GUI never rewrites `RS_ASIO.ini`.
- The public x86 host, x86 ASIO proxy, x64 RSMods.exe/runtime, four-file runtime package, and x64 installer build successfully from the current source. Package architecture and installer-embedded host/proxy hashes are verified.
- Clean-install, administrator-prompt, hardware hot-plug, audible playback, and in-game recording acceptance are still required before the build date is set. The release gate is documented in `Tests/AudioRouting/README.md`.
