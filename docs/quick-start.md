# Quick Start

Two defaults catch most new users: the pedal feature is **off** until it is
enabled in `RSMods.ini`, and every game session starts with the pitch mode
**Off** until `F7` selects one. If you installed the mod and the pedal keys do
nothing, one of the steps below was skipped.

## 1. Install the files

- Download the **release zip**, not the raw DLL. Some antivirus tools flag a
  bare DLL download and quarantine it, leaving the old file in place.
- Close the game, then extract `xinput1_3.dll` next to `Rocksmith2014.exe`,
  replacing the existing file. Replacing while the game runs fails because the
  loaded DLL is locked.
- Install the matching `RSMods` settings-app folder from the same release.

## 2. Enable the pedal

The feature is off by default. In `RSMods.ini` next to `Rocksmith2014.exe`,
add both lines exactly as shown (add the `Engine` line too if your file does
not have one):

```ini
[Drop Pedal]
EnableDropPedal = on
Engine = automatic
```

With RS_ASIO installed the pedal promotes itself
to the ASIO engine; without it the Cable engine is used. See the
[ASIO](asio-drop-pedal.md) and [Cable](cable-drop-pedal.md) guides for the
input-side setup each engine needs.

## 3. Select a mode in game

The session always starts at `Pitch: Off`. With Rocksmith focused:

| Action | Key |
|---|---|
| Cycle Off -> Drop Pedal -> Speaker Mode | `F7` |
| Pitch target down / up | `,` / `.` |
| Physical guitar tuning (Speaker Mode) | `F9` |

The overlay in the top-left corner shows the current mode. The pitch keys do
nothing while the mode is Off or the game window is not focused.

## Confirm it loaded

`RSMods_debug.txt` next to `Rocksmith2014.exe` is rewritten every launch.
After starting the game, the top of the log tells the whole story:

| Log line | Meaning |
|---|---|
| `RSModsPlus 3.0 ...` | The mod DLL loaded. The `(based on RSMods 1.2.8.2)` part is the upstream base version, not the installed version. |
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
