#pragma once

// Starts the installed RSMods.exe background host. Its model and feature code
// live in RSModsPlus.dll; the child is bound to the game's kill-on-close job.
namespace MlServiceLauncher
{
	void EnsureStarted();
	void Shutdown();
}
