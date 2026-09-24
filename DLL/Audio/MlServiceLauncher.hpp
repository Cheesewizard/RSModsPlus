#pragma once

#include <string>

// Starts the installed RSMods.exe background host. Its model and feature code
// live in RocksmithAudioBridge.dll; the child is bound to the game's kill-on-close job.
namespace MlServiceLauncher
{
	void EnsureStarted();
	void Shutdown();

	// Plain-language service state for the overlay Debug page (ported from the retired GUI ML card,
	// RSModsPlus/MachineLearning/MlServiceConnection.cs). connected = fresh results are flowing;
	// canRestart = RequestRestart() is meaningful right now.
	void Describe(std::string& message, bool& connected, bool& canRestart);
	// Same as the GUI's Restart button: signals the restart event EnsureStarted() polls.
	bool RequestRestart();
}
