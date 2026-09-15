# RSModsPlus Release Notes

## Release
- **Version:** `v4.0.0`
- **Build date:** `2026-09-14`
- **Target:** `x86` (32-bit, matching Rocksmith 2014)
- **Scope:** `Pre-release`

> Note by Note is a **beta** feature toggled from the menu, and Modern Cable input is **experimental**. See Known bug #76 before promoting this to a public/stable release.

## Feature list

### Audio Bridge (new)
- Seven-channel audio mixer with an integrated routing studio for shaping and monitoring the signal path.
- ASIO proxy driver: a resilient virtual ASIO device that forwards to and taps the real interface, enabling wet/game-audio recording and output routing. Apply output links RS_ASIO.ini to the proxy for output and input (the original driver is stashed and restored on removal).
- Interface hot-plug: booting without the interface plays through a Windows device with the Real Tone Cable as input; applying the interface as output later binds it for input and output together, no restart.
- Output protection: look-ahead loudness guard plus a fixed output trim to hold a safe ceiling.
- DSP controls: input conditioner with a noise gate, and per-direction routing controls.
- Taps the game output rather than replacing it, with a power button so the bridge can be enabled or disabled without forcing an ASIO downgrade.

### Modern Cable input (experimental)
- Real Tone Cable style input path (PortAudio to shared-mode WASAPI) offered as an accessibility option, not an ASIO replacement.
- WASAPI event mode is available but is not a guaranteed latency win; whether it feels better is left for the player to judge on their own rig. No latency claim is made.
- RS_ASIO stays in charge of the ASIO path; switching to Cable mode only renames the RS_ASIO files and never rewrites RS_ASIO.ini.

### Note by Note practice mode (beta; toggle in the menu)
- In-process, per-note detection built as a single tuned blend of three components that always work together for accuracy: the game's native pitch/chord matcher, the enhanced raw-pitch verifier, and the FretNet ML detector. None of the three is optional; the bundled FretNet service is auto-launched as part of detection, and all three feed one accept decision.
- Fretboard note cues during Riff Repeater (detector feedback colouring on the cues is not currently working, see Known bugs #90).
- Bend detection and visualization groundwork.

### Guitar / tuning
- Cable users now get native Drop Pedal support: no more multipitch pitch-shifting, lower latency, and any tone can be used.
- Configurable Drop Pedal overlay colours.
- Reduced Drop Pedal down-shift latency (measured).
- ASIO input gain and an in-game override for the game's own amp noise gate, so a noisy interface input stops chopping sustain.

### HUD and general
- HUD pitch labels spelled the way guitarists read them; corner status removed.
- Speaker Mode PSARC entry scanning and packaging fixes carried forward from the v3.2 hotfixes.

## Known bugs
Severity / impact should be set per entry. Issue numbers reference github.com/Cheesewizard/RSModsPlus.

- **[High]**
	- #76 Cable: crash on startup with stock `ExclusiveMode=1` (exclusive-input hook chases an impossible cable exclusive).
	- #74 Note by Note: Drop Pedal + cable double-shift rejects the open string and accepts flat notes.
	- #63 Speaker Mode emits a piercing high-pitched tone on song load under CPU load (buffer underrun; headphone safety risk).
	- #64 Game intermittently stalls at the loading spinner just before profile load (spins forever; relaunch clears it).

- **[Medium]**
	- #82 Drop Pedal does not account for mixed ASIO and Real Tone Cable multiplayer inputs.
	- #81 Audio HUD shows misleading and unavailable latency figures across cable and ASIO.
	- #65 Riff Repeater timeline loses its orange section colour (only purple shown).
	- #59 Song rarely launches in Song Attack instead of Learn a Song (not reproduced in mod code).
	- #58 Note by Note: ringing chords satisfy repeat-strum sections without a re-pick.
	- #57 Auto-tuning sets the Drop Pedal / Speaker Mode offset too late (must apply at song-list scroll, before load).
	- #56 In-game pause-screen tuner flickers between adjacent tunings (E/Eb) under a tuning offset.
	- #48 Note by Note: wrong target after a loop restart when enabled mid-section.
	- #17 ASIO Drop Pedal volume loss on large downward shifts.

- **[Low]** (mostly Note by Note visual polish; only affect the beta feature when enabled)
	- #90 Fretboard note cues do not show detector feedback colours (no visual detector feedback on the cues).
	- #60 Top strings flicker / wrong string animation during a held plain note.
	- #47 After a loop turnover the restarted pass renders all notes gray.
	- #46 Two fretboard notes can be lit at once.
	- #45 Long sustains render gray along the tail.
	- #44 Bend visuals wrong during the hold (floating marker, plain-note repaint).
	- #42 Bends can starve waiting for confirmation.
	- #41 Single notes sometimes need multiple picks before registering.
	- #40 Pressing "practice section" while enabled silently disables it.
	- #39 Gray next-section notes at the loop end block the restart.
	- #38 Bend on high E fret 15 triggers the open-E success animation.
	- #37 One-frame flash on note markers still occurs.

## Bug fixes
- Note by Note under Drop Pedal no longer accepts the fret one above the target: the tier-0 raw verifier and the ML confirm both re-applied the input shift and tested a semitone high (2026-09-15, related to #74). The same fix lets the raw verifier rescue the correct fret again under Drop Pedal, which is what made fast quiet picks stall until a clean pick caught them. Awaiting a live pass.
- Note by Note HUD text size can no longer be changed by a mouse wheel scroll over the settings page; the boxes only respond to their arrows and typing.
- Player 2 gets its own SIGNAL row under the Player 1 row while the game is in 2-player, fed from the Player 2 cable or the second ASIO input, with explicit "no input", "cable off", "waiting" and "stalled" states.
- #62 Note by Note now works in Release builds (menu rocker and RR menu cursor behave correctly). Previously a beta blocker.
- #23 Custom string colours now match note colours (user-reported).

## Workarounds
- Cable startup crash (#76): keep `ExclusiveMode=0`, or use RS_ASIO with an M-Track style interface instead of the Modern Cable path.
- Loading spinner stall (#64): relaunch Rocksmith; it clears on the next start.
- Speaker Mode piercing tone (#63): reduce CPU load on song load; remove headphones before loading a song on a heavily loaded system.

## Validation
- Feature smoke checks completed:
	- Audio Bridge routing, DSP controls, and gameplay integration (per the three most recent audio-bridge commits).
	- Modern Cable input verified live (shared-mode, bit-exact vs the native path).
	- ASIO proxy wet-recording verified live.
- Remaining verification:
	- Cable startup with `ExclusiveMode=1` (#76).
	- Audio HUD latency figures (#81).

## Known limitations
- Modern Cable input is experimental and an accessibility option, not an ASIO replacement; RS_ASIO stays the recommended path when an ASIO interface is present. WASAPI event mode may or may not improve latency on a given setup, so it is left for players to try and decide.
- Note by Note detection is strong on single notes, including high frets. Chords are the weakest case but are generally no worse than the game's native detection. Treat the mode as beta.
- Deployed builds may be terminated by Bitdefender Advanced Threat Defense; a per-exe ATD exception is required (folder exclusion alone is not enough).
