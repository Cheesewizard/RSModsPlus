#pragma once

namespace NoteByNoteHudLabel
{
	// Installs the localized-string resolver detour. Safe no-op if the game build does not
	// match the expected resolver prologue. Call once at startup on the game's main thread.
	void Initialize();
}
