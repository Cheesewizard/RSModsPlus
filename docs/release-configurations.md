# Release configurations

| Configuration | Optimization | Developer bridge |
| --- | --- | --- |
| Release Public | Release | Compiled out, including the optional DLL loader |
| Release Developer | Release | Enabled only with the matching rsmodsdebug.dll |

The named configurations are available in RSMods.sln. Both map the GUI and managed
library to their existing Release settings. The legacy installer is excluded.
Existing configurations remain available for existing development workflows; only
Release Public is accepted by RSModsPlus/PackageRuntime.ps1.

Public gameplay uses the built-in Note by Note controller. ML obtains chart
expectations from the version 2 audio shared-memory header, so neither feature
requires the research command pipe or a separate NoteByNoteProbe.dll.

Release Developer builds rsmodsdebug.dll separately under
Installer/Resources/Developer/Release Developer. The native host looks beside the
game executable, verifies the DLL against its build-time SHA-256 before loading,
and checks the bridge API. Missing tooling leaves gameplay running without a
research endpoint. Keep the host and optional debug DLL from the same build.
The pipe is local-only, restricted to the current logon SID, and uses bounded
request/response buffers. Public releases exclude this loader and transport.

## Public download

- xinput1_3.dll and rsmodsplus.dll in the main game directory.
- RSMods/RSMods.exe.

The GUI embeds supporting dependencies and tools. It verifies and extracts these
to %LOCALAPPDATA%/RSModsPlus/Runtime/<payload hash> automatically. The model remains
embedded in rsmodsplus.dll. No manual dependency or model download is needed;
the existing .NET Framework 4.7.2 requirement remains.

This changes the native/managed audio ABI to version 2. Install the three files
together; the earlier Python live companion uses the older mapping.

Building and packaging do not constitute live gameplay acceptance or permission
to deploy. The newly built public/developer variants need an in-game check.
