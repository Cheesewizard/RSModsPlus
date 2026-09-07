#pragma once

namespace NoteByNoteHighwayRenderer
{
	// Installs the presentability detour. Safe to call once at startup.
	bool InstallPresentationGate();

	// Refreshes the cached target once per native preparation pass.
	void RefreshSelectedTarget(void* owner);

	// Drops the cached target so the highway immediately returns to stock.
	void ClearSelectedTarget();

	// The selected target expressed the way the D3D instance streams express it, so the
	// instance filter can decide without reaching back into native note memory.
	//
	// Engine-side per-note gating was tried first and rejected: 0x7A4AA0 and 0x7A4B50
	// populate per-note visual state rather than drawing, so suppressing them could not
	// remove geometry that was already built and left notes grey when they were later
	// revealed. Filtering instances out of the draw is non-destructive by comparison, and
	// leaves every note's state fully maintained.
	//
	// Returns false when no single-note target is active, in which case the highway must be
	// left exactly as Rocksmith submitted it.
	bool TryGetSelectedPresentation(int& stringIndex, int& fret);
	// Same predicate without the coordinates: the draw hook's cheap gate for whether the
	// instance filter could act on this draw at all, so it never issues a device query
	// while Note by Note holds nothing.
	bool HasSelectedPresentation();

	// The selected target regardless of whether suppression is enabled. Suppression is off,
	// so TryGetSelectedPresentation deliberately reports nothing; diagnostics and any future
	// target marking still need to know which note is selected.
	bool TryGetSelectedTargetForDiagnostics(int& stringIndex, int& fret);

	// Chord-hold shape for the stale-marker quad filter: the SNG chord template's
	// per-string frets (playable fret or -1), stashed by the overlay's event handler
	// which already reads the template for the target text. Matched by chordId against
	// the live state before use; a mismatch simply leaves the filter dormant.
	// fingers (1..4 per fretted string, or 0/absent) rides along for the host-drawn
	// finger numerals; nullptr keeps the previous behaviour with no fingering.
	void SetChordTargetShape(int32_t chordId, const int* frets, const int* fingers = nullptr);

	bool TryGetChordFingerForCoordinate(int stringIndex, int fret, int& finger);

	// The complete coordinate set the stale-marker quad filter must keep: the single
	// target plus its legato group during note holds, or the fretted chord members
	// during chord holds. Returns false (filter dormant) when neither is active.
	bool TryGetMarkerKeepCoordinates(int (&strings)[8], int (&frets)[8], int& count);

	bool IsChordHoldActive();

	// How the neck-diagram grid gate at 0x7AA140 treats a cell outside the current gesture.
	//
	// Runtime-settable rather than compiled in, because every experiment on this surface
	// otherwise costs a main-DLL rebuild and a game restart: the detour has to be installed
	// at startup, so the hook cannot be hot-reloaded even though the controller can. The
	// modes are the three candidate answers to "how do you stop a marker being drawn", and
	// which is correct depends on whether the grid is cleared between frames, which is
	// measured by the (NBN GRID) pre-write line rather than assumed.
	enum class GridGateMode
	{
		// No suppression. The diagram behaves exactly as stock.
		Off = 0,
		// Run the original in full, then put the two cells back as they were. Correct if the
		// grid is cleared between frames, because the restored state is then the cleared one.
		Restore = 1,
		// Run the original in full, then overwrite the two cells with a configured sentinel.
		// Needed if nothing clears the grid, because restoring would preserve a stale note.
		Sentinel = 2,
		// Skip the original entirely. Known bad and kept only so it is not re-invented:
		// 0x7AA140 also performs an unconditional container operation on ESI+0xD04 via
		// 0x7AA260, and skipping that produced "the selected native record expired before
		// the hold could be established" on the first run it was tried.
		SkipKnownBad = 3
	};

	void SetGridGateMode(GridGateMode mode);
	GridGateMode GetGridGateMode();

	// The neck-placement diagnostics are disabled by default and must remain off during
	// stopped-preview filtering.
	void SetNeckPlacementMode(long mode);

	void EvictRememberedDimCandidates();
	long GetNeckPlacementMode();
	void SetNeckPlacementSiteMask(long mask);
	long GetNeckPlacementSiteMask();
	// Grants a bounded (NBN FADE) log budget over the fade pipeline, independent of the
	// placement mode. Re-arm by calling again; zero steady-state cost once spent.
	void ArmFadeDryLog();
	const char* DescribeGridGateMode(GridGateMode mode);

	// The words written to the value and key cells in Sentinel mode. Defaults are the
	// zero/lowest-priority pair; the (NBN GRID) line reports what an untouched cell actually
	// holds so these can be set to the real cleared state once it is known.
	void SetGridSentinel(uint32_t valueWord, float keyWord);
	void GetGridSentinel(uint32_t& valueWord, float& keyWord);

	enum class HighwayMode
	{
		// Highway exactly as Rocksmith submits it. What shipped before this switch existed.
		Off = 0,
		// Hide every note that is not the target or its published legato group. Rejected in
		// play for removing the read-ahead, kept because it is the decisive test of whether
		// the extra marker is highway geometry at all.
		All = 1,
		// Hide only non-gesture notes within a time window either side of the target, which
		// are the ones the frozen window parks close enough to the plane to be mistaken for
		// something to play. Notes further out stay visible, so the read-ahead survives.
		//
		// The window has to be symmetric. A one-sided "already passed" rule was written first
		// and would have missed the reported case entirely: the chart around the blocker runs
		// green 15 at 14.102, purple 12 at 14.264, green 12 at 14.427, green 15 at 14.602, so
		// while purple 12 is the target the offending marker is 338 ms *after* it, not before.
		NearTargetWindow = 2
	};

	void SetHighwayMode(HighwayMode mode);
	HighwayMode GetHighwayMode();
	const char* DescribeHighwayMode(HighwayMode mode);

	void SetUpcomingDimEnabled(bool enabled);
	bool GetUpcomingDimEnabled();

	// Half-width of the NearTargetWindow, in seconds of authored time. Runtime-settable
	// because the right value is a feel judgement made while playing, not something to
	// discover by rebuilding. The chart that exposed the blocker spaces notes 163 ms apart,
	// and the controller's own dense-successor boundary is 0.270, so the useful range is
	// around a quarter to half a second.
	void SetHighwayWindowSeconds(float seconds);
	float GetHighwayWindowSeconds();
}
