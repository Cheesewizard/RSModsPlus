# ML status and restart tests

Run `cmd /c Tests\MlControl\run.cmd` from the repository root on Windows with
Visual Studio C++/.NET Framework build tools installed.

The 32-bit fixture compiles the production launcher and starts dummy children in
a unique `build/Tests/MlControl` directory. The 64-bit C# fixture uses the production
connection client and synthetic audio/result mappings with a `.TestControl` suffix.
The normal game mappings are never written. Only the fixture's PID-specific restart
event is signalled. Unused MIDI definitions satisfy existing precompiled-header
dependencies; they are never invoked.

Checks cover child replacement ordering, explicit recovery after exit, no automatic
retry, fresh/stale audio results, protocol mismatch, missing package recovery and
child shutdown with its host. A 30-second host watchdog bounds failures.

`UiPreview.cs` hosts the production panel off-screen for layout inspection without
running the real settings form's startup/install operations. Compile it with
`GUI/UI.MlService.cs` and `RSModsPlus/MachineLearning/MlServiceConnection.cs`, referencing
System.Windows.Forms and System.Drawing, then pass a PNG output path.
