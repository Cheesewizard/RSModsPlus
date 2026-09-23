# RSModsPlus Release Notes

## Release
- **Version:** `v4.0.0`
- **Build date:** `TBD` (set from the final verified build)
- **Scope:** `Pre-release`

> Note by Note is a **beta** feature toggled from the menu, and Modern Cable input is **experimental**.

## Feature list

### Audio Bridge (new)
- Seven-channel playback mixer with an integrated routing studio for shaping and monitoring the signal path. Independent level control over Song (backing track), Player 1 guitar, Player 2 guitar, Microphone, Voice-over, and Effects, plus a Master that sits over everything. Player 1 and Player 2 levels are playback only and never affect note detection.
- Optional ASIO proxy driver: a resilient virtual ASIO device that forwards to and taps the real interface, enabling ASIO game-mix recording and live output routing. The installer places the driver file in the Rocksmith folder but does not register it. Choosing an ASIO-capable output and pressing **Apply output** performs the one-time Windows registration after an administrator prompt and records the real driver it should wrap.
- RS_ASIO setup remains user-owned. The bridge never rewrites `RS_ASIO.ini`. To put the proxy in the signal chain, set the relevant output and paired input `Driver` entries to `Rocksmith Audio Bridge`; removing the driver also leaves the file untouched.
- Interface recovery (ASIO mode with the proxy configured in RS_ASIO): booting without the interface keeps Rocksmith available through a shared-mode Windows output, with Real Tone Cable fallback when one is present. Reconnecting the interface does not switch automatically; choose that interface and press **Apply output** to move input and output back to ASIO without restarting the game. Windows output devices can also be selected while the game is running.
- Output protection: an optional look-ahead limiter caps the digital output at the selected ceiling. It is off by default and does not replace setting a safe interface or headphone volume.
- Guitar input conditioning that applies to any input path (ASIO interface, Real Tone Cable, or a third-party cable), running on the Player 1 input before the game's amp and note gate: make-up gain to raise a quiet input to a usable level; a noise gate (downward expander) that reduces between-note hiss without cutting sustain; an input compressor that evens out string-level variation; a 50/60 Hz mains-hum notch filter that removes ground-loop hum; and a Rocksmith gate override that sets the game's own noise floor (P1_NoiseFloor). The noise gate and the Rocksmith gate can be used separately or together.
- Taps the game output rather than replacing it, with a power button so the bridge can be enabled or disabled without forcing an ASIO downgrade.
- Recording: capture game-mix audio, dry guitar, and video takes, toggled with a bindable key (F9 by default) from the audio bridge. Cable/shared-output game-mix recording uses the native render tap; ASIO game-mix recording requires the optional proxy to be registered and configured. Dry recording follows the active Player 1 input. A red REC indicator appears in the top right of the game while recording.
- Force update song list: a button on the audio bridge Diagnostics page that re-scans the dlc folder live, so Rocksmith picks up a psarc you just added without a restart. Useful for quickly cycling through test songs.
- In-game overlay hover tips: every control in the in-game audio bridge overlay now shows a hover tooltip, using the same wording as the desktop app, so the meters, mixer, guitar conditioning, output cap, recording and misc options explain themselves without leaving the game.
- In-game overlay input meters: the input bar now reads the live guitar signal from the input tap (it previously showed the output mix, so it stayed near empty however hard you played) on a dB scale so a strum fills it. In two-player it splits into separate "input 1" and "input 2" bars, with the second shown only while the game is in multiplayer.
- In-game overlay output readout: the Output tab now names the actual device the game is playing through (for example "Playing to: M-Track (direct)") instead of the generic "your audio device", reading the same RS_ASIO output driver the desktop bridge reports.
- In-game overlay settings are now remembered: changes made from the overlay to the guitar input conditioning (input gain, noise suppressor, compressor, hum filter, Rocksmith gate) and to the output volume cap (limiter on/off and ceiling) are written back to `RSMods.ini`, so they survive a relaunch instead of resetting each session. Previously the overlay applied these live only. The Mixer faders remain session-only playback levels by design.

### Modern Cable input (experimental)
- Real Tone Cable style input path (PortAudio to shared-mode WASAPI) offered as an accessibility option, not an ASIO replacement.
- Wider USB cable compatibility: Modern Cable Input uses Windows shared-mode conversion to present Rocksmith-compatible 48 kHz capture from supported devices whose native Windows format uses another sample rate.
- The same compatibility mode uses WASAPI event-driven capture when available. That may reduce latency on some systems, but it is not guaranteed and no latency claim is made.
- RS_ASIO stays in charge of the ASIO path; switching to Cable mode only renames the RS_ASIO files and never rewrites RS_ASIO.ini.

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
- The auto-launched Audio bridge (opened behind the game when Rocksmith starts) had no taskbar icon: Windows never creates a taskbar button for a window whose very first show is already minimised, so the bridge could only be brought back through the main RSModsPlus GUI. It now starts minimised but still gets its taskbar button and icon next to Rocksmith's, so restoring it is a single click.
- Speaker Mode now works with custom DLC built on newer Wwise versions (DLC Builder / Wwise 2022/2023). The shipped WEM decoder rejected unrecognized RIFF chunks (e.g. `akd`), so Speaker Mode failed on those songs even though Rocksmith played them fine; the WEM is now rewritten keeping only the chunks the decoder recognizes. This was never fixed before this release.
- Speaker Mode PSARC entry scanning and packaging fixes (carried forward from the v3.2 hotfixes).
- #23 Custom string colours now match note colours (user-reported).

## Validation status

- The automated Audio Routing suite passes on the current source, including missing-output startup, device loss and recovery, recording lifecycle, Cable/ASIO routing policy, and the rule that the GUI never rewrites `RS_ASIO.ini`.
- The public x86 host, x86 ASIO proxy, x64 desktop bridge/runtime, four-file runtime package, and x64 installer build successfully from the current source. Package architecture and installer-embedded host/proxy hashes are verified.
- Clean-install, administrator-prompt, hardware hot-plug, audible playback, and in-game recording acceptance are still required before the build date is set. The release gate is documented in `Tests/AudioRouting/README.md`.
