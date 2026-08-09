# Quick Start

The Drop Pedal is off after installation. Follow these steps to enable and use
it.

## 1. Install the files

- Install upstream RSMods 1.2.8.2 first. Its libraries and decode tools remain
  in the `RSMods` folder.
- Download the **release zip**, not the raw DLL. The ZIP contains the matching
  `RSMods.exe` required by Speaker Mode. Some antivirus tools also flag a bare
  DLL download and quarantine it, leaving the old file in place.
- Close the game, then extract the complete ZIP into the Rocksmith 2014 folder.
  Allow it to replace `xinput1_3.dll`, `RSMods\RSMods.exe` and
  `RSMods\RSMods.exe.config`. Replacing files while the game or settings app
  runs will fail because the loaded files are locked.

## 2. Enable the pedal

Make sure this is in `RSMods.ini` next to `Rocksmith2014.exe`:

```ini
[Drop Pedal]
EnableDropPedal = on
Engine = automatic
```

With RS_ASIO installed, `automatic` uses the ASIO engine. Without RS_ASIO, it
uses the Cable engine.

## 3. Use Drop Pedal in game

Every launch starts at `Pitch: Off`, regardless of the previous session.

1. With Rocksmith focused, press `F7` once. The top-left readout changes to
   `Drop: E`.
2. Press `,` to move the guitar down one semitone or `.` to move it up one
   semitone.
3. Match the target note in the readout to the song's tuning. For an Eb song
   with a guitar physically tuned to E standard, press `,` once. The readout
   becomes `Drop: E -> Eb (-1)`.

The mod shifts the guitar sound and Rocksmith's note detection together.

Controls:

| Action | Player 1 | Player 2 |
|---|---|---|
| Cycle Drop Pedal / Speaker Mode / Off for everyone | `F7` | - |
| Move the target down / up | `,` / `.` | `Control+,` / `Control+.` |
| Tell the mod the guitar's physical tuning | `F9` | `Control+F9` |

If the guitar is not in E standard, press `F9` until its tuning appears first
in the readout. Hold `Control` with the tuning keys to change Player 2. Press
`F7` again for Speaker Mode, which shifts the song instead of the guitar.

## Confirm it loaded

`RSMods_debug.txt` next to `Rocksmith2014.exe` is rewritten every launch.
After starting the game, the top of the log tells the whole story:

| Log line | Meaning |
|---|---|
| `RSModsPlus 3.2 ...` | The mod DLL loaded. The `(based on RSMods 1.2.8.2)` part is the upstream base version, not the installed version. |
| `Drop pedal engine: ASIO Drop Pedal` or `Cable Drop Pedal` | The pedal is enabled and which engine owns it. |
| No `Drop pedal engine` line at all | `EnableDropPedal` is still `off` in `RSMods.ini`. |
| Old version number at the top | The old DLL is still loading: the replacement went to the wrong folder, the copy was blocked, or antivirus interfered. |

## Keys still do nothing?

| Check | Why |
|---|---|
| Overlay says `Pitch: Off` | Press `F7` to select a mode first |
| Rocksmith is not the focused window | The keys only work with the game focused |
| Log shows an old version | Replace the DLL in the folder Steam opens via Manage > Browse local files, with the game closed |
| No log file appears at all | The DLL is not loading: check Windows Security > Protection history for a quarantine event and restore/exclude it |
