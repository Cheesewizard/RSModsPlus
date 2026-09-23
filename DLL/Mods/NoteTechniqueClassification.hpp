#pragma once

#include <cstdint>

namespace NoteByNote
{
	constexpr uint32_t NOTE_MASK_HAMMER_ON = 0x00000200;
	constexpr uint32_t NOTE_MASK_PULL_OFF = 0x00000400;
	constexpr uint32_t NOTE_MASK_TAP = 0x00004000;

	// A chord hold leaves the selected-string index at the 0xFF sentinel (no single string).
	constexpr int CHORD_STRING_SENTINEL = 255;

	// Hammer-ons, pull-offs and taps are all played without re-picking: the string is
	// already ringing and the fretting hand alone changes the pitch. Requiring a fresh
	// attack for a hammer-on (the 2026-09-21 experiment) rejected real hammer-ons, which
	// produce no new pick - a half chord followed by a hammer-on on the same string never
	// accepted. All three techniques belong on the no-pick legato path; a picked hammer-on
	// still commits through the pick buffer as well (see IsPlainPickedTarget).
	inline bool UsesNoPickLegatoAcceptance(uint32_t noteMask)
	{
		return (noteMask & (NOTE_MASK_HAMMER_ON | NOTE_MASK_PULL_OFF | NOTE_MASK_TAP)) != 0;
	}

	inline bool IsHammerOn(uint32_t noteMask)
	{
		return (noteMask & NOTE_MASK_HAMMER_ON) != 0;
	}

	// A legato note continues the string just played. After a chord the predecessor's
	// string is the chord sentinel, and the chart only authors a hammer-on or pull-off on
	// a string that chord fretted (2026-09-22: [x/x/5/5/5/x] then D fret 5 -> 7), so a
	// sentinel predecessor is treated as continuing the successor's string.
	inline bool LegatoContinuesPreviousString(int previousString, int successorString)
	{
		return successorString >= 0 && successorString < 6
			&& (previousString == successorString || previousString == CHORD_STRING_SENTINEL);
	}
}
