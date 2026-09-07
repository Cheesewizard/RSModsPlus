# Managed ML runtime

The FretNet model and CQT bases are embedded in rsmodsplus.dll. RSMods/RSMods.exe
runs the service automatically for the game process, with no Python dependency.
The GUI embeds supporting libraries, tools and notices; extraction and integrity
verification happen automatically in LocalAppData/RSModsPlus/Runtime.

The game-root assembly is authoritative even when an older library remains in
the RSMods subfolder. Startup failures are recorded in
LocalAppData/RSModsPlus/Logs/startup-error.log. Service diagnostics are in
ml-service.log with bounded rotation. Native launch failures and child exit are
reported in the game log. The service is tied to the game's lifetime.

The service reads version 2 audio and expected-note shared memory and publishes
version 2 string/fret evidence. Rate, shift and clock changes invalidate old
evidence. Note by Note also uses native scoring and raw-audio pitch/attack checks;
these remain useful when ML is unavailable.

See [build and packaging](release-configurations.md). A prerelease needs live
checks of ML status, repeated notes, bends, chords and the supported audio routes.
