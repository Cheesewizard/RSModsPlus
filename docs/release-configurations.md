# Public prerelease build

Select Release Public | Win32 in RSMods.sln. The public source tree contains the
built-in Note by Note controller, native pitch/attack detection and managed FretNet
service. Private developer tools and research history are not included.

Supply the approved ONNX build input with /p:MlModelPath=<absolute model path>.
The model is embedded in rsmodsplus.dll; end users need no model download or Python.

Build with PostBuildEventUseInBuild=false and RocksmithInstallDir pointing to a
nonexistent directory when building a package rather than installing it.
RSModsPlus/PackageRuntime.ps1 -Destination <new folder> packages exactly:

- xinput1_3.dll in the game folder.
- rsmodsplus.dll in the game folder.
- RSMods/RSMods.exe.

Supporting dependencies are embedded in the GUI and automatically extracted to
its verified LocalAppData cache. The existing .NET Framework 4.7.2 requirement
remains. Install all three files together; native and managed audio use ABI v2.

Private developer configurations remain in the local development checkout.
