# Note by Note prerelease - draft

Note by Note adds a Riff Repeater mode that waits for the expected note before
continuing. Detection combines native scoring, raw-audio pitch and pick attacks,
and supporting FretNet ML evidence. Repeated picked-note handling and high-fret
acceptance have been improved.

The managed ML service and model are bundled. The game starts the service
automatically; no Python installation or model download is required. Startup now
uses the main-folder library even when an older copy remains in RSMods.

This candidate also includes ASIO capture-lifetime fixes, the shared signal meter,
modern Wwise audio decoding for Speaker Mode, and the Crowd Control song-load
crash fix. Modern Cable input remains experimental and defaults to off.

## Installation

Close Rocksmith and the GUI. Back up the files being replaced, then extract the
ZIP into the Rocksmith 2014 folder, preserving its paths:

- xinput1_3.dll
- rsmodsplus.dll
- RSMods/RSMods.exe

The .NET Framework 4.7.2 prerequisite remains. Supporting dependencies are embedded
and extracted automatically into a verified LocalAppData cache. Public builds
contain no research command bridge or optional developer-DLL loader.

If needed, install the Note by Note menu with the game closed:

    RSMods\RSMods.exe --install-note-by-note-menu "C:\path\to\Rocksmith2014"

Enable Note by Note through Riff Repeater. The public build does not use the
private development hotkeys.

## Validation before publication

The clean branch passed its public build, all 12 packaged audio fixtures and the
stale-library upgrade regression. Live acceptance must still confirm GUI startup, automatic ML startup, fast repeated notes/B19,
bends/chords, and the intended audio setups. Good native detection alone does
not establish that ML is running. A clean-machine installation check is also
outstanding. This draft does not claim those live checks have passed.

No release tag, push or publication is performed by creating this candidate.
