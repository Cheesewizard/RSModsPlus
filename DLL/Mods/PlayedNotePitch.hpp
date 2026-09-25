#pragma once

namespace NoteByNote
{
	inline bool TryResolvePlayedNotePitch(int stringIndex, int fret, int tuningOffset, int inputShift, int& midi)
	{
		constexpr int OPEN_MIDI[] = { 40, 45, 50, 55, 59, 64 };
		midi = -1;
		if (stringIndex < 0 || stringIndex >= 6 || fret < 0 || fret >= 26
			|| tuningOffset < -12 || tuningOffset > 12 || inputShift < -48 || inputShift > 48) return false;
		const int resolved = OPEN_MIDI[stringIndex] + tuningOffset + fret + inputShift;
		if (resolved < 0 || resolved > 127) return false;
		midi = resolved;
		return true;
	}
}
