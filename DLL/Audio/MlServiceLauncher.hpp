#pragma once

// Spawns the FretNet string/fret ML companion (tools/ml-string-fret-service/service.py)
// once, bound to the game's lifetime. That service is what the Note-by-Note native
// scoring reads for its ML "stuck hold" rescue (DLL\Audio\MlStringFretReader.cpp ->
// the Local\RSModsPlus.MlStringFret.v2 mailbox); the host-side rescue toggle already
// defaults on, so the only thing that must be running for ML confirmations to work is
// this process. Making it the mod's default removes the manual "remember to start the
// service" step - the reason ML looked absent while the game refused high notes.
//
// Lifetime: the child is placed in a kill-on-close Job Object owned by the game
// process, so it exits when the game exits (clean shutdown or crash) - "game
// lifecycle", spawned exactly once.
//
// Safe everywhere: a no-op when a service is already running (respects a manually
// started instance and prevents the duplicate-writer bug), and a graceful no-op when
// the service files are not present (a Release box or another machine without the
// research tree), so it never blocks startup.
namespace MlServiceLauncher
{
	// Call once from the main mod thread after the probe host is initialized.
	// Idempotent: only the first call attempts a spawn.
	void EnsureStarted();

	// Call on game shutdown. Closes the job handle we own, which kills the child if
	// we launched it. (The kernel also kills it automatically if the game crashes.)
	void Shutdown();
}
