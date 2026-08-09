# Release packaging

The public ZIP is an update for an existing RSMods 1.2.8.2 installation. It
must contain the game DLL and the matching settings executable used by Speaker
Mode. The existing RSMods libraries and decode tools are unchanged and are not
duplicated in the update.

Restore the GUI packages, then build the GUI and DLL without deploying either
output to an installed Rocksmith directory:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild .\GUI\GUI.csproj /t:Restore /p:Configuration=Release /p:Platform=x64
& $msbuild .\RSMods.sln '/t:GUI;DLL' /m /p:Configuration=Release /p:Platform=Win32 /p:PostBuildEventUseInBuild=false
```

Create the release archive:

```powershell
$version = "3.2.0"
.\scripts\Package-Release.ps1 -OutputPath ".\artifacts\RSModsPlus-v$version.zip"
```

The script rejects a stale `RSMods.exe` that does not contain the Speaker Mode
extraction command. The ZIP must contain exactly:

```text
xinput1_3.dll
RSMods/RSMods.exe
RSMods/RSMods.exe.config
```

Extracting it into the Rocksmith 2014 directory updates the two binaries and
the helper configuration while preserving `RSMods.ini`, libraries, decode
tools and other existing RSMods files.
