#pragma once

// Note by Note presentation gate.
//
// Rocksmith decides whether a note is drawn in exactly one place: the presentability
// predicate at 0x7A5CE0, whose only caller is 0x7A5D50 at 0x7A5D71. Its decompiled
// body reduces to "currentTime < noteEndTime". When it returns zero the engine calls
// its own retirement path 0x7A61F0 and never admits the note to the string/fret
// presentation grid, so nothing downstream constructs or draws it.
//
// This matters twice over while Note by Note owns a hold. The controller freezes the
// transport, so currentTime stops advancing and no note can ever fail that test. The
// whole remaining chart therefore stays presentable and is redrawn every frame, which
// is both why the future highway stays visible during a hold and why the frame rate
// collapses (measured 20 FPS against a normal 60, and since the scoring update is
// frame-coupled the scoring tick falls with it, to roughly 16 Hz).
//
// That tick rate was previously said to starve the onset detector and so to reject
// correct notes. It does not: the detector reports the pitch of the analysis ring's
// current frame on every call and nothing about a pluck expires between calls, so poll
// rate changes how often we look but not what is there to see. See
// docs/investigations/note-by-note-input-gates.md. The frame rate is still worth
// recovering on its own merits; it is not the reason a hold stalls.
//
// Returning zero for non-selected notes is therefore the entire feature, expressed
// through the engine's own lifecycle rather than fought from outside it.
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

	// The template finger (1..4) for one fretted chord member, valid only while a
	// chord hold drives the keep set. The 2026-08-26 transition capture proved the
	// game's numeral glyphs are screen-space quads that never repaint on frozen
	// retargets, so the host draws its own numerals from this instead.
	bool TryGetChordFingerForCoordinate(int stringIndex, int fret, int& finger);

	// The complete coordinate set the stale-marker quad filter must keep: the single
	// target plus its legato group during note holds, or the fretted chord members
	// during chord holds. Returns false (filter dormant) when neither is active.
	bool TryGetMarkerKeepCoordinates(int (&strings)[8], int (&frets)[8], int& count);

	// True while a chord hold drives the keep set. The fingering-panel layer paints
	// the WRONG chord's fingering during frozen chord holds (Philip 2026-08-26: G5's
	// 1/3/4 shown while holding C5, whose fingering is 1/1/3), so during chord holds
	// the suppressors drop that layer entirely - including at member coordinates -
	// and the overlay carries the authoritative fingering instead.
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

	// Drop every remembered dim-candidate visual pointer. Must be called on each NBN
	// teardown path (disable, probe reload, probe unload): the destructor-detour
	// eviction is probe-requested and can be unhooked mid-teardown, and a stale
	// pointer that survives can hand the synthetic dim to a reused allocation
	// (the 2026-08-31 "removed frets" bug: persistent neck elements dimmed for the
	// rest of the game session).
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

	// How the scrolling highway treats notes that are not the current gesture.
	//
	// The double-marker Philip reported as a blocker is not a neck-diagram fault. The fretboard
	// buffer consumed at 0x7AA1B0 was measured holding exactly one cell, always the target, and
	// never the extra note. The extra marker is highway geometry: suppression here is off so the
	// player can read ahead, and with the transport frozen a neighbouring note scrolls to the
	// strike plane and parks there, reading as a second note to play.
	//
	// So the choice is not "hide the highway or not". Full suppression was already tried and
	// rejected in play, because reading ahead matters. StrikePlaneOnly is the targeted form:
	// keep the whole highway, hide only what has arrived at the plane and is not the gesture.
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

	// Upcoming-marker dim (v8): while a hold is frozen, upcoming in-section
	// markers are force-dimmed so notes that cannot be played yet do not look
	// playable - the accepted fix for the double-marker beta bug. Philip's
	// 2026-08-25 report ("the notes are coloured but then become gray when time
	// is stopped") asks for the other side of that trade, so it is now
	// runtime-toggleable for a live A/B: off = upcoming notes keep their colour
	// (watch for the double-marker bug returning), on = the beta behavior.
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
