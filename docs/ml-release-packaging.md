# Bundled C# ML service

The FretNet ONNX model and fixed training CQT bases are embedded in
`rsmodsplus.dll`. Neither resource is extracted or downloaded at runtime.
`RSMods.exe --ml-service <game PID>` hosts the service in its existing x64
process. The native game integration starts this installed executable, with
no Python path, development-directory lookup, or alternate model.

The former live Python launcher has been replaced. The Python implementation
remains an offline reference used by developer parity tests.

## Runtime

- Managed CQT matches the existing eight-octave training transform: 22,050 Hz,
  192 bins, 512-sample hop, nine context frames, retained sparse bases, and
  whole-window peak normalization.
- Bundled libsoxr 0.1.3 performs the same HQ resampling and octave downsampling.
- The trained FretNet model is loaded directly from embedded bytes by ONNX Runtime.
- The service reads the existing post-shifter audio ring and writes the existing
  112-byte v2 seqlock result mailbox. Audio-center timestamps, physical/observed
  fret separation, 0.5 unconstrained and 0.35 expected-pitch thresholds are retained.
- A rate/shift change or sample-clock rewind invalidates prior evidence. Overwritten
  snapshots are discarded; nonfinite audio and ABI mismatches fail explicitly.
- Chart expectation is read from the v2 audio mapping with a seqlock and freshness check.
  Unavailable chart context is logged; it does not invent an expected note.
- A named mutex prevents multiple managed writers. A native kill-on-close job binds
  the service to the game, and the service also watches the parent process.
- Service logs live in LocalAppData/RSModsPlus/Logs and are capped at approximately
  2 MiB plus one rotated file. Warmup runs before processing game audio.

The model, frontend, and orchestration are in rsmodsplus.dll. Native ONNX Runtime,
libsoxr, supporting runtime DLLs, tool files and notices are embedded in RSMods.exe.
On startup the GUI verifies and extracts them to its content-addressed LocalAppData
cache. The package contains exactly three files: xinput1_3.dll, RSMods/RSMods.exe
and rsmodsplus.dll in the main game directory.
The existing .NET Framework requirement remains; no Python installation or separate
model download is required.

## Build and package

Build GUI/GUI.csproj with Configuration=Release and PostBuildEventUseInBuild=false.
The library builds its bundled x64 resampler using Visual Studio CMake tools.
The approved model artifact is supplied at build time through MlModelPath (default:
tools/ml-training/models/fretnet_guitarset.onnx); a missing artifact fails the build.
CqtBasis.bin is a frozen generated resource; ExportCqtBasis.py documents regeneration
using the training frontend. End users do not run these developer tools.

Select **Release Public | Win32** in RSMods.sln for a distributable build.
**Release Developer | Win32** uses release optimization and includes the optional
private debug tools. See [release configurations](release-configurations.md).
For command-line builds disable GUI copying with PostBuildEventUseInBuild=false
and set RocksmithInstallDir to a nonexistent path. These checks do not deploy.
PackageRuntime.ps1 -Destination <new folder> packages only Release Public and
rejects known developer endpoint/loader strings in the host. No debug DLL, probe,
loose dependencies or external manifest is included in the three-file package.
The legacy installer is not built by either new solution configuration.

## Verification on 2026-09-07

Evidence: artifacts/managed-ml-20260907.

- Managed Release build and native Debug/Release builds pass.
- Audio frontend and all 138 model outputs compared with Python for 12 fixtures:
  silence, deterministic noise, low E, and high B19 tones at 22,050/44,100/48,000 Hz.
  Maximum normalized feature error below 0.000001, model-output error below 0.0001.
- Production service tested on isolated audio/result mappings using recorded
  guitar audio, a -1 shift, rate change/clock reset, low note, and silence.
  Published frets/confidence and timestamps matched the Python reference.
- Duplicate managed writer rejected; parent exit while waiting for audio exits cleanly.
- Repeated isolated stream test from the packaged directory with PATH restricted to
  Windows/System32. Loaded native modules came from the package or Windows, including
  ONNX Runtime, soxr and app-local Visual C++ runtime. No development native dependencies.
- Tests leave the game and its running Python service untouched.

These are parity, build, packaging and isolated-service checks. A clean Windows
machine test and live gameplay with the managed replacement remain outstanding.
The earlier successful B19 gameplay session used the Python ML companion; it must
not be claimed as live acceptance of this C# migration.

The three-file bootstrap was additionally tested with PATH limited to Windows and
System32: all 12 audio fixtures exited successfully with byte-identical results using embedded
ONNX and soxr dependencies. Earlier multi-file package checks above predate this
bootstrap. Public and developer gameplay still require in-game acceptance.

The optional developer transport passed an isolated request/response and shutdown
test, including invalid API rejection, duplicate-start rejection, remote-client
rejection flag and an explicit single-entry pipe DACL. This test redirects only
the pipe name; it never connects to the game bridge.

## Upgrade startup regression

A previous RSMods/RSModsPlus.dll can remain after extracting the new package.
The old assembly does not contain MlService. Starting the GUI in a game-root
application domain makes the main-folder rsmodsplus.dll authoritative, including
when the old same-name assembly is still in the GUI subfolder. GUI settings paths
remain beside the executable. Startup failures are recorded in
LocalAppData/RSModsPlus/Logs/startup-error.log before service initialization.
The native launcher validates the main-folder library and checks child exit on
its normal polling loop; an exited child is reported without automatic retries.

Verified using the actual old installed library beside the new GUI: packaged
B19 output matches the reference exactly. This is an upgrade regression check;
it does not replace live gameplay acceptance of the replacement build.
