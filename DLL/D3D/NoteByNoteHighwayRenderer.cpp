#include "../stdafx.h"
#include "NoteByNoteHighwayRenderer.hpp"

#include "../Mods/NoteByNoteNativeScoring.hpp"
#include "../Mods/NoteByNoteController.hpp"

#include <cstdint>
#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <cmath>

// Presentability gate for Note by Note. See the header for the mechanism.
//
// Verified ABI of the predicate at 0x7A5CE0, read from the unpacked image rather than
// inferred, because an earlier convention guess corrupted a return frame:
//
//   007a5ce0  55                 PUSH EBP            ; note pointer arrives in EAX
//   007a5ce1  8B EC              MOV EBP,ESP
//   007a5ce3  51                 PUSH ECX
//   007a5ce4  80 B8 44 01 ..     CMP byte ptr [EAX + 0x144],0
//   ...
//   007a5d01  C2 04 00           RET 0x4             ; callee pops the single float
//
// and its only call site, which proves EAX still holds the note there (0x7A5D58 copies
// EAX to EDI and nothing writes EAX before the call):
//
//   007a5d58  8B F8              MOV EDI,EAX
//   007a5d6a  D9 45 08           FLD float ptr [EBP + 0x8]
//   007a5d6d  51                 PUSH ECX
//   007a5d6e  D9 1C 24           FSTP float ptr [ESP]
//   007a5d71  E8 6A FF FF FF     CALL 0x007a5ce0
//   007a5d76  84 C0              TEST AL,AL
//   007a5d78  75 0D              JNZ  admit
//   007a5d7a  E8 71 04 00 00     CALL 0x007a61f0     ; else the engine retires it
//
// So: note in EAX, one float argument at [ESP+4] on entry, boolean result in AL,
// RET 4. The detour must preserve that contract exactly.
namespace
{
	constexpr uintptr_t NATIVE_PRESENTABILITY_PREDICATE = 0x7A5CE0;
	constexpr uintptr_t NATIVE_NOTEWAY_SETUP = 0x7E22F0;
	constexpr uintptr_t NATIVE_DRAW_DESCRIPTORS = 0x7A4B50;
	constexpr uintptr_t NATIVE_DRAW_GEOMETRY = 0x7A52C0;
	constexpr uintptr_t NATIVE_DRAW_SUBMIT = 0x7A6160;
	// The neck diagram grid write. Reached from eight call sites inside 0x7A5D50, two of
	// which are downstream of a path that skips the presentability predicate entirely.
	constexpr uintptr_t NATIVE_FRETBOARD_GRID_WRITE = 0x7AA140;
	// Consumes the finished per-frame fretboard buffer, which arrives in EDI. Plain RET, no
	// stack arguments. This is the only point where the completed state exists: render
	// preparation 0x7E4870 initialises the buffer at 0x7AA0B0, fills it by looping the owner
	// note vector through 0x7A5D50, and then calls this. Reading the buffer anywhere else in
	// the frame returns the previous frame's residue, which is the mistake that made an earlier
	// dump look like the surface was wrong.
	constexpr uintptr_t NATIVE_FRETBOARD_BUFFER_CONSUME = 0x7AA1B0;
	constexpr uintptr_t NECK_PLACEMENT_SITES[] =
		{ 0x7A8B10, 0x7A8CE0, 0x7A8E90, 0x7A90B0, 0x7A9290, 0x7A93C0, 0x7A94F0 };
	constexpr size_t NECK_PLACEMENT_SITE_COUNT =
		sizeof(NECK_PLACEMENT_SITES) / sizeof(NECK_PLACEMENT_SITES[0]);
	// Slot +0x5C setter, __thiscall(this, byte): hooked separately for the dry log only -
	// the byte argument reads like a visibility flag, which would be a cleaner lever than
	// relocation if the log confirms it.
	constexpr uintptr_t NECK_VISIBILITY_SITE = 0x7A9220;
	constexpr uintptr_t NECK_MARKER_UPDATER = 0x79D070;
	constexpr uintptr_t NECK_WINDOW_CONDITION = 0x7E9ED0;
	// 16-bit dimension counts the consumer's own nested loop bounds against, read rather than
	// assumed to be six by twenty-six.
	constexpr uintptr_t FRETBOARD_BUFFER_STRING_COUNT = 0xD00;
	constexpr uintptr_t FRETBOARD_BUFFER_FRET_COUNT = 0xD02;
	constexpr uintptr_t RECORD_MASK = 0x00;
	constexpr uint32_t NOTE_MASK_HAMMERON = 0x00000200;
	constexpr uint32_t NOTE_MASK_PULLOFF = 0x00000400;
	constexpr uintptr_t NATIVE_NOTE_RECORD = 0x2C;
	// The SNG record's authored time, same layout the scoring controller reads.
	constexpr uintptr_t RECORD_TIME_OFFSET = 0x0C;

	using NativePredicate = char(*)(float);
	using NativeNotewaySetup = void(*)(int);

	using NativeDrawDescriptors = void(*)(void*, void*, float, float);
	using NativeDrawGeometry = void(*)(void*, void*, float, float);
	using NativeDrawSubmit = void(*)(float);
	// ESI is an implicit argument, so this is only ever tail-jumped to from the naked
	// detour and never called through this type.
	using NativeGridWrite = void(*)();
	// EDI is an implicit argument, so this is only ever tail-jumped to.
	using NativeBufferConsume = void(*)();
	using NativeNeckPlacement = uint32_t(__fastcall*)(void* subElement, void* unusedEdx);

	using NativeNeckVisibility = uint32_t(__fastcall*)(void* self, void* unusedEdx, uint8_t flag);
	// Measured from the 0x79D070 disassembly: PUSH owner, PUSH visual, CALL, RET 0x8 -
	// plain stack args with callee cleanup, i.e. __stdcall(visual, owner). The EDX
	// side-channel hazard applies only to HOOKING it (it parks the visual in EDX across
	// the condition call); calling it is safe, it loads EDX itself from the stack arg.
	using NativeMarkerUpdater = void(__stdcall*)(void* visual, void* owner);
	using NativeWindowCondition = uint16_t(__fastcall*)(void* windowStruct, void* unusedEdx, float time);

	NativePredicate originalPredicate = nullptr;
	NativeNotewaySetup originalNotewaySetup = nullptr;
	NativeNeckPlacement originalNeckPlacements[NECK_PLACEMENT_SITE_COUNT] = {};
	NativeNeckVisibility originalNeckVisibility = nullptr;
	NativeMarkerUpdater originalMarkerUpdater = nullptr;
	NativeWindowCondition originalWindowCondition = nullptr;
	NativeDrawDescriptors originalDrawDescriptors = nullptr;
	NativeDrawGeometry originalDrawGeometry = nullptr;
	NativeDrawSubmit originalDrawSubmit = nullptr;
	NativeGridWrite originalGridWrite = nullptr;
	NativeBufferConsume originalBufferConsume = nullptr;
	bool isGateInstalled = false;

	constexpr bool IS_PRESENTATION_SUPPRESSION_ENABLED = false;
	volatile long highwayMode =
		static_cast<long>(NoteByNoteHighwayRenderer::HighwayMode::Off);
	// The target's authored time, for the strike-plane rule. A note earlier than the target
	// should already have passed; with the transport frozen it parks at the plane instead.
	volatile float selectedRecordTime = 0.0f;
	volatile bool hasSelectedRecordTime = false;
	// Default chosen from the chart that exposed the blocker: notes 163 ms apart, and the
	// controller's dense-successor boundary at 0.270. Half a second covers both neighbours
	// either side without reaching notes that are genuinely far enough away to read.
	volatile float highwayWindowSeconds = 0.5f;
	constexpr uintptr_t RECORD_TIME = 0x0C;

	constexpr bool IS_FRETBOARD_SUPPRESSION_ENABLED = true;

	volatile bool isTargetActive = false;
	volatile bool isNativeHoldOwned = false;
	volatile uintptr_t selectedRecord = 0;

	constexpr uint32_t PRESENTATION_SETTLE_FRAMES = 8;
	volatile uint32_t presentationSettleFrames = 0;
	volatile uint64_t lastSeenEpoch = 0;
	volatile bool hasLastSeenEpoch = false;
	volatile uintptr_t lastSeenSelectedRecord = 0;
	volatile bool hasLastSeenSelectedRecord = false;
	volatile bool lastOwnsNativeHold = false;
	volatile bool hasLastOwnsNativeHold = false;
	// The same target expressed as the instance streams express it, for the D3D filter.
	volatile int selectedStringIndex = -1;
	volatile int selectedFret = -1;
	// The target's legato gesture, computed by the probe where the whole note vector is
	// available in time order. Fixed size and written only from the preparation pass.
	volatile uint32_t visualGroupCount = 0;
	uintptr_t visualGroupRecords[NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup] = {};
	// The same gesture as coordinates, for the grid-write gate, which is handed
	// (string, fret) and never a note pointer.
	int visualGroupStrings[NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup] = {};
	int visualGroupFrets[NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup] = {};
	uintptr_t loggedRecord = 0;
	volatile bool isChordTargetActive = false;
	volatile int32_t chordShapeChordId = -1;
	int chordShapeFrets[6] = { -1, -1, -1, -1, -1, -1 };
	// Template fingers parallel to chordShapeFrets (1..4, or 0 when the template
	// gives none), for the host-drawn numerals. Same tolerated-race rules.
	int chordShapeFingers[6] = { 0, 0, 0, 0, 0, 0 };
	// Diagnostics. Distinguishes three failure modes that look identical on screen:
	// the detour never firing, the decision returning false, or suppression not
	// actually preventing the draw.
	volatile uint32_t descriptorCalls = 0;
	volatile uint32_t descriptorSuppressed = 0;
	volatile uint32_t geometryCalls = 0;
	volatile uint32_t geometrySuppressed = 0;
	volatile uint32_t submitCalls = 0;
	volatile uint32_t submitSuppressed = 0;
	template <typename T>
	bool TryRead(uintptr_t address, T& value)
	{
		if (address == 0) return false;

		__try
		{
			value = *reinterpret_cast<const T*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Returns true when this note must be hidden. Integer work only: the caller has a
	// live x87 stack (the float argument was just stored through FSTP), so this must
	// not disturb the FPU.
	enum class CountedGate { Descriptors, Geometry, Submit };

	bool __cdecl ShouldSuppressNoteCounted(void* note, CountedGate gate);
	bool __cdecl ShouldSuppressNote(void* note);
	bool __cdecl ShouldSuppressOnFretboard(void* note);
	bool __cdecl IsOutsideCurrentGesture(void* note);

	bool __cdecl ShouldSuppressDescriptors(void* note)
	{
		return ShouldSuppressNoteCounted(note, CountedGate::Descriptors);
	}

	bool __cdecl ShouldSuppressGeometry(void* note)
	{
		return ShouldSuppressNoteCounted(note, CountedGate::Geometry);
	}

	bool __cdecl ShouldSuppressSubmit(void* note)
	{
		return ShouldSuppressNoteCounted(note, CountedGate::Submit);
	}

	// Whether this note is not part of the current gesture. Shared by both gates so the
	// fretboard and the highway can never disagree about what the target is.
	bool __cdecl IsOutsideCurrentGesture(void* note)
	{
		if (presentationSettleFrames > 0) return false;
		if (!isTargetActive) return false;

		const uintptr_t target = selectedRecord;
		if (target == 0) return false;

		uintptr_t record = 0;
		if (!TryRead(reinterpret_cast<uintptr_t>(note) + NATIVE_NOTE_RECORD, record))
		{
			// An unreadable note is left to Rocksmith rather than guessed at.
			return false;
		}

		if (record == target) return false;

		// Keep the target's legato continuation visible. Only the contiguous same-string
		// run published by the probe qualifies: sparing every legato note in the chart,
		// as a first attempt did, left unrelated notes on the highway beside the target
		// and implied gestures the chart never asks for.
		const uint32_t groupCount = visualGroupCount;
		for (uint32_t i = 0; i < groupCount
			&& i < NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup; ++i)
		{
			if (visualGroupRecords[i] == record) return false;
		}

		if (hasSelectedRecordTime)
		{
			float recordTime = 0.0f;
			if (TryRead(record + RECORD_TIME_OFFSET, recordTime)
				&& std::fabs(recordTime - selectedRecordTime) <= 0.002f)
			{
				return false;
			}
		}

		return true;
	}

	// Highway gates: the draw calls and the noteway pool. Off, so the player reads ahead.
	bool __cdecl ShouldSuppressNote(void* note)
	{
		const auto mode = static_cast<NoteByNoteHighwayRenderer::HighwayMode>(highwayMode);
		if (mode == NoteByNoteHighwayRenderer::HighwayMode::Off) return false;
		if (!IsOutsideCurrentGesture(note)) return false;
		if (mode == NoteByNoteHighwayRenderer::HighwayMode::All) return true;

		// NearTargetWindow. Hide non-gesture notes close in authored time to the target,
		// either side of it. Rocksmith's near pool holds every note inside a time window and
		// sweeps it as time advances; the hold stops the window moving rather than collapsing
		// it, so neighbours sit at the plane indefinitely and read as notes to play.
		//
		// Symmetric on purpose. The one-sided version, hiding only what should already have
		// passed, would have missed the reported case: the offending marker was 338 ms after
		// the target, not before it.
		if (!hasSelectedRecordTime) return false;

		uintptr_t record = 0;
		float recordTime = 0.0f;
		if (!TryRead(reinterpret_cast<uintptr_t>(note) + NATIVE_NOTE_RECORD, record)
			|| record == 0
			|| !TryRead(record + RECORD_TIME, recordTime))
		{
			// An unreadable note is left to Rocksmith rather than guessed at.
			return false;
		}
		const float window = highwayWindowSeconds;
		return std::fabs(recordTime - selectedRecordTime) <= window;
	}

	// Fretboard gate: the presentability predicate at 0x7A5CE0, which feeds the neck
	// diagram grid at 0x7AA140. On, so only the current gesture appears under the player's
	// hand while the highway stays complete.
	volatile uint32_t fretboardCalls = 0;
	volatile uint32_t fretboardSuppressed = 0;

	bool __cdecl ShouldSuppressOnFretboard(void* note)
	{
		if (!IS_FRETBOARD_SUPPRESSION_ENABLED) return false;
		const bool suppress = IsOutsideCurrentGesture(note);
		++fretboardCalls;
		if (suppress) ++fretboardSuppressed;
		return suppress;
	}

	bool __cdecl ShouldSuppressNoteCounted(void* note, CountedGate gate)
	{
		const bool suppress = ShouldSuppressNote(note);
		switch (gate)
		{
		case CountedGate::Descriptors:
			++descriptorCalls;
			if (suppress) ++descriptorSuppressed;
			break;
		case CountedGate::Geometry:
			++geometryCalls;
			if (suppress) ++geometrySuppressed;
			break;
		case CountedGate::Submit:
			++submitCalls;
			if (suppress) ++submitSuppressed;
			break;
		}
		return suppress;
	}

	// The neck-diagram grid write, gated by coordinate.
	//
	// Why this exists on top of the predicate gate above. All eight FretboardGridWrite calls
	// live inside 0x7A5D50, below its `TEST AL,AL / JNZ` on the predicate result at 0x7A5D78,
	// so a false predicate really does stop them: the note is retired at 0x7A5D7A and the
	// function returns. The predicate gate is not useless, which corrects the earlier record
	// claiming it "does not decide what is drawn there".
	//
	// But 0x7A5D50 opens with a bypass:
	//
	//   007a5d5a  MOV BL,byte ptr [EDI + 0x51]
	//   007a5d5f  JNZ 0x007a5d6a          ; nonzero -> normal path, predicate runs
	//   007a5d61  CMP byte ptr [EDI + 0x52],BL
	//   007a5d64  JZ  0x007a608a          ; both zero -> skip the predicate entirely
	//
	// 0x7A608A lands before the grid writes at 0x7A60D8 and 0x7A613D, so a note whose +0x51
	// and +0x52 are both zero reaches the neck diagram without the predicate being consulted.
	// That is why the gate above reported 157/163 and 64774/67173 suppressed while an
	// upcoming note stayed on the fretboard: that note was never one of the calls it saw.
	// 0x7AA140 sits below both paths.
	//
	// What +0x51 and +0x52 are is not established and is deliberately not guessed at.
	//
	// Gating by coordinate rather than by record is forced by the ABI, not a shortcut: the
	// write receives (string, fret, value, key) and no note pointer. Coordinates are also
	// what the diagram actually shows, so the two agree by construction.
	// Runtime-settable, because the detour must be installed at startup and so cannot be
	// hot-reloaded even though the controller can. Compiling the mode in cost a rebuild and a
	// game restart per experiment.
	//
	// Starts Off: the game draws its own diagram untouched until an experiment explicitly
	// selects a mode over the bridge. A presentation intervention must never be the startup
	// default, and a restart must never silently re-enable one.
	volatile long gridGateMode =
		static_cast<long>(NoteByNoteHighwayRenderer::GridGateMode::Off);
	volatile uint32_t gridSentinelValue = 0;
	volatile float gridSentinelKey = 0.0f;

	// Snapshot of the cell a suppressed write is about to touch, taken before the original
	// runs and consumed immediately after it. The grid write happens on one thread inside a
	// render pass, and the detour always pairs the two, so a single slot is enough.
	// The grid object, captured from the detour because it arrives in ESI and exists nowhere
	// else. Needed to read the table, which is the only way to see a marker that is stale
	// rather than being written.
	volatile uintptr_t lastSeenGrid = 0;
	// Frames still to dump. Set on a target change so a few consecutive frames are reported,
	// which distinguishes a cell that is there every frame from one that flickers.
	volatile long fretboardDumpFramesRemaining = 0;
	// Off by default since the rogue-draw investigation closed (handover 16): the grid walk
	// plus its console line cost visible frame rate around retargets, and the buffer already
	// answered its question (only the target occupies it). Flip to 1 to re-arm the dump.
	volatile long isFretboardDumpEnabled = 0;
	uintptr_t pendingGrid = 0;
	int pendingString = -1;
	int pendingFret = -1;
	uint32_t pendingValue = 0;
	float pendingKey = 0.0f;

	// The two parallel tables the write indexes, both based at the grid object:
	//   value at (string * 0x1A + fret) * 4
	//   key   at ((string + 0x1A) * 0x1A + fret) * 4
	// read directly off the index arithmetic at 0x7AA16E and 0x7AA184.
	uintptr_t GridValueCell(uintptr_t grid, int stringIndex, int fret)
	{
		return grid + (static_cast<uintptr_t>(stringIndex) * 0x1A + fret) * 4;
	}

	uintptr_t GridKeyCell(uintptr_t grid, int stringIndex, int fret)
	{
		return grid + ((static_cast<uintptr_t>(stringIndex) + 0x1A) * 0x1A + fret) * 4;
	}

	volatile uint32_t gridCalls = 0;
	volatile uint32_t gridSuppressed = 0;

	volatile uint32_t gridProbesLogged = 0;

	// grid is ESI, the object the write indexes into. Passed in so the cell's contents can
	// be read *before* the original runs, which is the only way to tell whether the grid is
	// cleared between frames. If a non-gesture cell reads the same sentinel every frame, that
	// sentinel is the cleared state and restoring it removes the marker. If it reads the
	// previous frame's note instead, nothing clears it and the gate has to write the sentinel
	// explicitly rather than restore what was there.
	bool __cdecl ShouldSuppressGridCell(uintptr_t grid, int stringIndex, int fret)
	{
		++gridCalls;
		if (grid != 0) lastSeenGrid = grid;
		const auto mode = static_cast<NoteByNoteHighwayRenderer::GridGateMode>(gridGateMode);
		if (mode == NoteByNoteHighwayRenderer::GridGateMode::Off) return false;
		if (!IS_FRETBOARD_SUPPRESSION_ENABLED) return false;
		if (!isTargetActive) return false;

		const int targetString = selectedStringIndex;
		const int targetFret = selectedFret;
		// Without a published target there is nothing to compare against, so the diagram is
		// left exactly as Rocksmith would draw it.
		if (targetString < 0 || targetFret < 0) return false;
		if (stringIndex == targetString && fret == targetFret) return false;

		// The target's hammer-on / pull-off run is one gesture with it and must stay visible;
		// a hammer-on follows its attack with no gap, so hiding it would be wrong in the
		// opposite direction.
		const uint32_t groupCount = visualGroupCount;
		for (uint32_t i = 0; i < groupCount
			&& i < NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup; ++i)
		{
			if (visualGroupStrings[i] == stringIndex && visualGroupFrets[i] == fret) return false;
		}

		++gridSuppressed;

		// Snapshot before the original runs, so the fix-up afterwards can put it back.
		pendingGrid = grid;
		pendingString = stringIndex;
		pendingFret = fret;
		pendingValue = 0;
		pendingKey = 0.0f;
		bool readValue = false;
		bool readKey = false;
		if (grid != 0)
		{
			readValue = TryRead(GridValueCell(grid, stringIndex, fret), pendingValue);
			readKey = TryRead(GridKeyCell(grid, stringIndex, fret), pendingKey);
			if (!readValue || !readKey)
			{
				// Nothing safe to restore, so leave the cell entirely alone.
				pendingGrid = 0;
				return false;
			}
		}

		// A few per target only: this runs inside a render path. This is the line that says
		// whether the grid is cleared between frames, which decides whether Restore is the
		// right mode or Sentinel is.
		if (gridProbesLogged < 4)
		{
			++gridProbesLogged;
			LOG_INFO("(NBN GRID) pre-write cell " << stringIndex << ':' << fret
				<< " value=0x" << std::hex << pendingValue << std::dec
				<< " key=" << pendingKey
				<< " readable=" << readValue << readKey
				<< " mode=" << NoteByNoteHighwayRenderer::DescribeGridGateMode(mode)
				<< " target=" << selectedStringIndex << ':' << selectedFret
				<< ". A constant key every frame means the grid is cleared, so Restore clears"
				<< " the marker; a previous note means it is not and Sentinel is needed."
				<< std::endl);
		}
		return true;
	}

	// Undoes a suppressed write, after the original has run in full.
	//
	// Running the original and then correcting the cell is deliberately not the same as
	// skipping the original. 0x7AA140 performs an unconditional container operation on
	// ESI+0xD04 via 0x7AA260 before it ever looks at the grid, and skipping that produced
	// "the selected native record expired before the hold could be established" on the one
	// run it was tried, a fault absent from all eight preceding captures. So every side
	// effect is allowed to happen and only the two words are put right.
	void __cdecl FinishSuppressedGridWrite()
	{
		const uintptr_t grid = pendingGrid;
		pendingGrid = 0;
		if (grid == 0 || pendingString < 0 || pendingFret < 0) return;

		const auto mode = static_cast<NoteByNoteHighwayRenderer::GridGateMode>(gridGateMode);
		uint32_t value = pendingValue;
		float key = pendingKey;
		if (mode == NoteByNoteHighwayRenderer::GridGateMode::Sentinel)
		{
			value = gridSentinelValue;
			key = gridSentinelKey;
		}

		__try
		{
			*reinterpret_cast<uint32_t*>(GridValueCell(grid, pendingString, pendingFret)) = value;
			*reinterpret_cast<float*>(GridKeyCell(grid, pendingString, pendingFret)) = key;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	// ESI carries the grid object implicitly and the four arguments are on the stack, with
	// the callee cleaning them (RET 0x10).
	//
	// The suppress path re-pushes the same four arguments and calls the original, which
	// cleans them itself, then runs the fix-up and performs its own RET 0x10. Four identical
	// `push [esp+0x10]` land the arguments in the right order because each push shifts the
	// frame by one slot. ESI survives: 0x7AA140 saves EBP, ECX and EDI and never writes ESI.
	__declspec(naked) void GridWriteDetour()
	{
		__asm
		{
			pushfd
			push eax
			push ecx
			push edx

			// Four pushes above, so the entry frame [ret][string][fret][value][key] now
			// starts at [esp+0x10]: string at 0x14, fret at 0x18.
			movzx eax, byte ptr [esp + 0x18]   // fret
			push eax
			movzx eax, byte ptr [esp + 0x18]   // string, 0x14 shifted by the push above
			push eax
			push esi                           // the grid object, an implicit argument
			call ShouldSuppressGridCell
			add esp, 0xc
			test al, al
			jnz suppress

			pop edx
			pop ecx
			pop eax
			popfd
			jmp originalGridWrite              // stack untouched; it does its own RET 0x10

		suppress:
			pop edx
			pop ecx
			pop eax
			popfd

			push dword ptr [esp + 0x10]        // key
			push dword ptr [esp + 0x10]        // value
			push dword ptr [esp + 0x10]        // fret
			push dword ptr [esp + 0x10]        // string
			call originalGridWrite             // cleans its own four arguments
			call FinishSuppressedGridWrite
			ret 0x10
		}
	}

	// The finished fretboard buffer, read where the engine consumes it.
	//
	// This answers the question the write gate could not. The predicate gate already stops
	// every non-target note reaching the writes (302 observed calls, all for the target cell),
	// yet a second marker still draws, so either something reaches the buffer without going
	// through 0x7AA140 or the diagram is not drawn from this buffer at all. The completed
	// buffer says which.
	//
	// Read-only, and dimensions read from the buffer rather than assumed.
	void __cdecl ReportFretboardBuffer(uintptr_t buffer)
	{
		if (buffer == 0 || isFretboardDumpEnabled == 0) return;
		if (fretboardDumpFramesRemaining <= 0) return;
		--fretboardDumpFramesRemaining;

		uint16_t stringCount = 0;
		uint16_t fretCount = 0;
		if (!TryRead(buffer + FRETBOARD_BUFFER_STRING_COUNT, stringCount)
			|| !TryRead(buffer + FRETBOARD_BUFFER_FRET_COUNT, fretCount))
		{
			LOG_INFO("(NBN FRETBOARD BUFFER) unreadable buffer=0x" << std::hex << buffer
				<< std::dec << "." << std::endl);
			return;
		}
		// Bound the walk against a corrupt read; the real values are expected to be 6 and 26.
		if (stringCount > 8) stringCount = 8;
		if (fretCount > 32) fretCount = 32;

		std::ostringstream occupied;
		uint32_t occupiedCount = 0;
		for (uint16_t stringIndex = 0; stringIndex < stringCount; ++stringIndex)
		{
			for (uint16_t fret = 0; fret < fretCount; ++fret)
			{
				uint32_t value = 0;
				float key = 0.0f;
				if (!TryRead(GridValueCell(buffer, stringIndex, fret), value)) continue;
				if (!TryRead(GridKeyCell(buffer, stringIndex, fret), key)) continue;
				if (value == 0) continue;
				if (occupiedCount < 16)
				{
					occupied << ' ' << stringIndex << ':' << fret
						<< "=0x" << std::hex << value << std::dec
						<< '/' << std::fixed << std::setprecision(3) << key;
				}
				++occupiedCount;
			}
		}

		LOG_INFO("(NBN FRETBOARD BUFFER) occupied=" << occupiedCount
			<< " target=" << selectedStringIndex << ':' << selectedFret
			<< " group=" << visualGroupCount
			<< " dims=" << stringCount << 'x' << fretCount
			<< " buffer=0x" << std::hex << buffer << std::dec
			<< " |" << occupied.str()
			<< ". Any occupied cell that is not the target or its group is what draws the extra"
			<< " marker; if only the target is here, the diagram is not drawn from this buffer."
			<< std::endl);
	}

	// EDI carries the buffer implicitly. Plain RET, no stack arguments, so the detour reports
	// and then tail-jumps with the stack untouched.
	__declspec(naked) void BufferConsumeDetour()
	{
		__asm
		{
			pushfd
			push eax
			push ecx
			push edx

			push edi                       // the buffer
			call ReportFretboardBuffer
			add esp, 4

			pop edx
			pop ecx
			pop eax
			popfd
			jmp originalBufferConsume
		}
	}

	// Suppressed notes return 0, which drives the engine's own 0x7A61F0 retirement at
	// 0x7A5D7A. Everything else tail-jumps to the original with the stack and EAX
	// untouched, so the original performs its own RET 4 back to the real caller.
	__declspec(naked) void PredicateDetour()
	{
		__asm
		{
			pushfd
			push ecx
			push edx
			push eax                    // preserve the note across the helper call

			push eax                    // argument: note
			call ShouldSuppressOnFretboard
			add esp, 4
			test al, al
			jnz suppress

			pop eax
			pop edx
			pop ecx
			popfd
			jmp originalPredicate       // stack still holds [ret][float]

		suppress:
			pop eax
			pop edx
			pop ecx
			popfd
			xor eax, eax                // AL = 0: not presentable
			ret 4
		}
	}

	// Noteway (scrolling highway) gate.
	//
	// The fretboard gate above controls 0x7AA140's six-by-twenty-six string/fret grid,
	// which is the neck diagram, not the highway. The highway is maintained by
	// 0x7E2340, which bands each note into one of three object pools by comparing the
	// note's time against the current time:
	//
	//   state = 2;
	//   if (note+0x38 < time + scale*near || note+0x180 == 0) state = 0;
	//   else if (note+0x38 < time + scale*far)                state = 1;
	//   if (note+0x178 != state) {
	//       if (note+0x178 != -1) teardown(0x7E2210);   // sets note+0x178/+0x17C = -1
	//       setup(0x7E22F0, state);                     // allocates from the pool
	//   }
	//
	// Because Note by Note freezes the transport, "time" never advances, so no note is
	// ever rebanded and nothing is torn down: the whole remaining chart stays resident
	// and is drawn every frame. That is the packed highway, the lingering previous
	// note, and the frame rate, all from one cause.
	//
	// Suppressing setup is therefore the entire fix. The engine's own teardown has
	// already run and left note+0x178 at -1, which is its "no noteway object" state, so
	// skipping setup leaves the note undrawn. On later frames teardown is skipped too
	// (already -1), leaving only two compares per note. Letting a note through restores
	// it normally, so nothing needs restoring by hand.
	//
	// ABI read from the image, not inferred:
	//   007e22f4  8B 5D 08        MOV EBX,[EBP+8]        ; arg: pool state
	//   007e231b  8B 06           MOV EAX,[ESI]          ; note = *ESI
	//   007e231d  89 98 78 01..   MOV [EAX+0x178],EBX
	//   007e2337  C2 04 00        RET 0x4
	// and its call site, which establishes the register inputs:
	//   007e24a4  56              PUSH ESI               ; state
	//   007e24a5  8D 75 FC        LEA ESI,[EBP-0x4]      ; ESI = &notePtr
	//   007e24a8  E8 43 FE FF FF  CALL 0x007e22f0        ; EAX = owner
	//
	// So: owner in EAX, &notePtr in ESI, state at [ESP+4], callee pops 4.
	__declspec(naked) void NotewaySetupDetour()
	{
		__asm
		{
			pushfd
			push ecx
			push edx
			push eax                    // preserve the owner

			mov eax, [esi]              // note = *ESI
			push eax
			call ShouldSuppressNote
			add esp, 4
			test al, al
			jnz suppressNoteway

			pop eax
			pop edx
			pop ecx
			popfd
			jmp originalNotewaySetup    // ESI and the stack are untouched

		suppressNoteway:
			pop eax
			pop edx
			pop ecx
			popfd
			ret 4                       // skip setup; note stays at -1 and is not drawn
		}
	}


	__declspec(naked) void DrawDescriptorsDetour()
	{
		__asm
		{
			pushfd
			push eax
			push ecx
			push edx

			mov eax, [esp + 20]         // 16 bytes pushed + 4 byte return address
			push eax
			call ShouldSuppressDescriptors
			add esp, 4
			test al, al
			jnz suppressDescriptors

			pop edx
			pop ecx
			pop eax
			popfd
			jmp originalDrawDescriptors

		suppressDescriptors:
			pop edx
			pop ecx
			pop eax
			popfd
			ret 0x10
		}
	}

	__declspec(naked) void DrawGeometryDetour()
	{
		__asm
		{
			pushfd
			push eax
			push ecx
			push edx

			mov eax, [esp + 20]         // 16 bytes pushed + 4 byte return address
			push eax
			call ShouldSuppressGeometry
			add esp, 4
			test al, al
			jnz suppressGeometry

			pop edx
			pop ecx
			pop eax
			popfd
			jmp originalDrawGeometry

		suppressGeometry:
			pop edx
			pop ecx
			pop eax
			popfd
			ret 0x10
		}
	}

	__declspec(naked) void DrawSubmitDetour()
	{
		__asm
		{
			pushfd
			push ecx
			push edx
			push eax                    // note arrives in EAX

			push eax
			call ShouldSuppressSubmit
			add esp, 4
			test al, al
			jnz suppressSubmit

			pop eax
			pop edx
			pop ecx
			popfd
			jmp originalDrawSubmit

		suppressSubmit:
			pop eax
			pop edx
			pop ecx
			popfd
			ret 4
		}
	}
}

namespace
{
	// The placement experiments are disabled by default. They alter shared highway state and
	// must never coexist with the stopped-preview marker filter.
	volatile long neckPlacementMode = 0;
	volatile long neckPlacementLogBudget = 40;
	volatile long neckPlacementRelocated = 0;
	// Which sites target mode may act on, as a bitmask over NECK_PLACEMENT_SITES indices.
	// Defaults to none: the dry log names the fretboard site first, then the mask admits
	// exactly that site. Sites 1 and 2 (0x7A8CE0/0x7A8E90) are the highway and must stay
	// out of the mask.
	volatile long neckPlacementSiteMask = 0;

	volatile long fadeDryLogBudget = 0;
	struct FadeDrySample
	{
		uintptr_t parent;
		uintptr_t sub;
		int stringIndex;
		int fret;
		int fadeEnabled;
		float fadeFrom;
		float fadeTo;
		float stepFrom;
		float stepTo;
		int markerPath;
		uint32_t colorState;
		int colorFlag;
		float colorFade;
		int listA;
		int listB;
	};

	// SEH-only gather; every field read is speculative (the ctx layouts differ per
	// site and the pointers can be mid-teardown). POD locals only.
	bool TryGatherFadeDry(size_t site, uintptr_t ctx, FadeDrySample* out) noexcept
	{
		__try
		{
			out->parent = *reinterpret_cast<volatile uintptr_t*>(ctx + 0x8);
			out->sub = *reinterpret_cast<volatile uintptr_t*>(ctx + 0xC);
			out->stringIndex = -1;
			out->fret = -1;
			if (out->parent >= 0x10000 && (out->parent & 3) == 0)
			{
				out->stringIndex = *reinterpret_cast<volatile uint8_t*>(out->parent + 0xC);
				out->fret = *reinterpret_cast<volatile uint8_t*>(out->parent + 0xD);
			}
			if (site == 5)
			{
				if (out->sub < 0x10000 || (out->sub & 3) != 0) return false;
				out->fadeEnabled = *reinterpret_cast<volatile uint8_t*>(out->sub + 0x68);
				out->fadeFrom = *reinterpret_cast<volatile float*>(out->sub + 0x6C);
				out->fadeTo = *reinterpret_cast<volatile float*>(out->sub + 0x70);
				out->stepFrom = *reinterpret_cast<volatile float*>(ctx + 0x14);
				out->stepTo = *reinterpret_cast<volatile float*>(ctx + 0x18);
			}
			else if (site == 4)
			{
				const auto beginA = *reinterpret_cast<volatile uintptr_t*>(ctx + 0x14);
				const auto endA = *reinterpret_cast<volatile uintptr_t*>(ctx + 0x18);
				const auto beginB = *reinterpret_cast<volatile uintptr_t*>(ctx + 0x20);
				const auto endB = *reinterpret_cast<volatile uintptr_t*>(ctx + 0x24);
				out->listA = static_cast<int>((endA - beginA) / 24);
				out->listB = static_cast<int>((endB - beginB) / 24);
			}
			else if (site == 2)
			{
				out->markerPath = *reinterpret_cast<volatile uint8_t*>(ctx + 0x28);
				out->colorState = *reinterpret_cast<volatile uint32_t*>(ctx + 0x24);
				out->colorFade = *reinterpret_cast<volatile float*>(ctx + 0x2C);
				out->colorFlag = -1;
				if (out->sub >= 0x10000 && (out->sub & 3) == 0)
				{
					out->colorFlag = *reinterpret_cast<volatile uint8_t*>(out->sub + 0x50);
				}
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}


	void LogFadeDry(size_t site, void* stepCtx, uint32_t result)
	{
		FadeDrySample s = {};
		if (!TryGatherFadeDry(site, reinterpret_cast<uintptr_t>(stepCtx), &s)) return;
		std::ostringstream line;
		line << "(NBN FADE) site=0x" << std::hex << NECK_PLACEMENT_SITES[site]
			<< " ctx=0x" << reinterpret_cast<uintptr_t>(stepCtx)
			<< " parent=0x" << s.parent << " sub=0x" << s.sub << std::dec
			<< " string=" << s.stringIndex << " fret=" << s.fret
			<< " ret=" << result << std::fixed << std::setprecision(3);
		if (site == 5)
		{
			line << " fadeEn=" << s.fadeEnabled
				<< " fadeFrom=" << s.fadeFrom << " fadeTo=" << s.fadeTo
				<< " curFrom=" << s.stepFrom << " curTo=" << s.stepTo;
		}
		else if (site == 4)
		{
			line << " listA=" << s.listA << " listB=" << s.listB;
		}
		else if (site == 2)
		{
			line << " markerPath=" << s.markerPath << " state=" << s.colorState
				<< " flag=" << s.colorFlag << " fade=" << s.colorFade;
		}
		LOG_INFO(line.str() << std::endl);
	}

	uint32_t HandleNeckPlacement(size_t site, void* subElement, uint32_t result)
	{
		const long mode = neckPlacementMode;
		if ((site == 2 || site == 4 || site == 5) && subElement != nullptr
			&& fadeDryLogBudget > 0 && InterlockedDecrement(&fadeDryLogBudget) >= 0)
		{
			LogFadeDry(site, subElement, result);
		}
		if ((site == 2 || site == 4 || site == 5) && subElement != nullptr)
		{
			NoteByNoteController::DispatchNeckPlacementStep(
				static_cast<uint32_t>(site),
				subElement);
		}
		if (mode == 0 || result != 2 || subElement == nullptr) return result;

		const auto parent = *reinterpret_cast<uintptr_t*>(
			reinterpret_cast<uintptr_t>(subElement) + 0x8);
		if (parent == 0) return result;
		const int stringIndex = *reinterpret_cast<uint8_t*>(parent + 0xC);
		const int fret = *reinterpret_cast<uint8_t*>(parent + 0xD);

		NoteByNoteProtocol::NoteByNoteState state;
		const bool hasState = NoteByNoteController::TryGetNoteByNoteState(state);

		if (mode == 1)
		{
			if (InterlockedDecrement(&neckPlacementLogBudget) >= 0)
			{
				LOG_INFO("(NBN PLACEMENT) site=0x" << std::hex << NECK_PLACEMENT_SITES[site]
					<< " placed string=" << std::dec << stringIndex
					<< " fret=" << fret
					<< " visual=0x" << std::hex << parent
					<< " sub=0x" << reinterpret_cast<uintptr_t>(subElement) << std::dec
					<< std::fixed << std::setprecision(3)
					<< " pos{" << *reinterpret_cast<float*>(parent + 0x14)
					<< "," << *reinterpret_cast<float*>(parent + 0x18)
					<< "," << *reinterpret_cast<float*>(parent + 0x1C) << "}"
					<< " hold=" << (hasState && state.ownsNativeHold != 0)
					<< std::endl);
			}
			return result;
		}

		if (!hasState || state.ownsNativeHold == 0) return result;
		if (((neckPlacementSiteMask >> site) & 1) == 0) return result;

		bool isWhitelisted = stringIndex == state.visualString && fret == state.visualFret;
		for (uint32_t i = 0; !isWhitelisted && i < state.visualGroupCount
			&& i < NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup; ++i)
		{
			isWhitelisted = stringIndex == state.visualGroupStrings[i]
				&& fret == state.visualGroupFrets[i];
		}
		if (isWhitelisted) return result;

		*reinterpret_cast<float*>(parent + 0x18) = -1000000.0f;
		const auto total = InterlockedIncrement(&neckPlacementRelocated);
		if (total == 1 || total % 100 == 0)
		{
			LOG_INFO("(NBN PLACEMENT) relocated off-screen: " << total
				<< " (site=0x" << std::hex << NECK_PLACEMENT_SITES[site] << std::dec
				<< " string=" << stringIndex << " fret=" << fret << ")" << std::endl);
		}
		return result;
	}

	template <size_t Site>
	uint32_t __fastcall NeckPlacementDetour(void* subElement, void* unusedEdx)
	{
		// The color step (site 2) applies its marker color one-shot per context, consuming
		// ctx+0x2C, so a probe policy that overrides the baked color state at ctx+0x24 must
		// see the context before the native call. The post-call forward below stays for the
		// read-only observers that need the return value.
		if (Site == 2 && subElement != nullptr)
		{
			NoteByNoteController::DispatchNeckPlacementStep(
				static_cast<uint32_t>(Site) | NoteByNoteProtocol::NECK_PLACEMENT_STEP_PRE,
				subElement);
		}
		const auto result = originalNeckPlacements[Site](subElement, unusedEdx);
		return HandleNeckPlacement(Site, subElement, result);
	}

	constexpr NativeNeckPlacement NECK_PLACEMENT_DETOURS[NECK_PLACEMENT_SITE_COUNT] =
	{
		&NeckPlacementDetour<0>,
		&NeckPlacementDetour<1>,
		&NeckPlacementDetour<2>,
		&NeckPlacementDetour<3>,
		&NeckPlacementDetour<4>,
		&NeckPlacementDetour<5>,
		&NeckPlacementDetour<6>,
	};

	// Mode 3 (window): the window-condition override, identity by TIME. The updater's
	// custom convention crashed v7.3, so it stays unhooked; the condition alone decides.
	// The window struct carries the note's [start +0x14, end +0x1C]; the start is the
	// note's onset time, which the controller state names exactly for the target
	// (selectedRecordTime) and, via the group record pointers (onset at record+0x0C),
	// for the legato group. A note whose window contains the frozen time but whose start
	// is not one of those onsets is read-ahead: report it outside its window and the
	// game's own machinery dims its marker. The first decisions are logged so the
	// time-identity assumption is verified from data before it is trusted.
	volatile long windowForcedCount = 0;
	volatile long windowLogBudget = 30;
	// The updater-visual vtable survey (one data-gathering session; see the
	// whitelist comment). Refreshed on every window-mode arm so each enable gets a
	// fresh sample.
	volatile long vtableLogBudget = 48;
	constexpr float ONSET_TOLERANCE_SECONDS = 0.002f;

	volatile uintptr_t staleCandidateVisual = 0;
	uintptr_t staleCandidateOwner = 0;
	float staleCandidateOnset = 0.0f;
	uintptr_t staleCandidateVtable = 0;
	constexpr uintptr_t UPDATER_VISUAL_VTABLE = 0x11CF1D8;
	volatile long syntheticDimActive = 0;
	volatile long staleDimLogBudget = 20;

	constexpr size_t STALE_DIM_ONSET_CAPACITY = 48;
	float staleDimOnsets[STALE_DIM_ONSET_CAPACITY] = {};
	size_t staleDimOnsetCount = 0;
	float staleDimLastTarget = -1.0f;

	bool IsStaleDimmedOnset(float time, float tolerance)
	{
		for (size_t index = 0; index < staleDimOnsetCount; ++index)
		{
			if (std::fabs(staleDimOnsets[index] - time) <= tolerance) return true;
		}
		return false;
	}

	constexpr size_t UPCOMING_DIM_CAPACITY = 16;
	float upcomingDimOnsets[UPCOMING_DIM_CAPACITY] = {};
	size_t upcomingDimCount = 0;
	float upcomingDimHoldOnset = -1.0f;
	uintptr_t upcomingCandidateVisual = 0;
	uintptr_t upcomingCandidateOwner = 0;
	float upcomingCandidateOnset = 0.0f;
	uintptr_t upcomingCandidateVtable = 0;  // same identity hardening as the stale candidate
	volatile long upcomingDimLogBudget = 24;
	volatile bool isUpcomingDimEnabled = false;


	bool IsUpcomingDimmedOnset(float time, float tolerance)
	{
		for (size_t index = 0; index < upcomingDimCount; ++index)
		{
			if (std::fabs(upcomingDimOnsets[index] - time) <= tolerance) return true;
		}
		return false;
	}

	bool TryReadRecordOnset(uintptr_t record, float* out) noexcept
	{
		if (record < 0x10000 || (record & 3) != 0) return false;
		__try
		{
			*out = *reinterpret_cast<volatile float*>(record + 0x0C);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// The target's legato-group onsets must stay lit: they are played as part of the
	// target's gesture.
	bool IsGroupOnset(const NoteByNoteProtocol::NoteByNoteState& state, float time, float tolerance)
	{
		for (uint32_t index = 0; index < state.visualGroupCount
			&& index < NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup; ++index)
		{
			float onset = 0.0f;
			if (TryReadRecordOnset(state.visualGroupRecords[index], &onset)
				&& std::fabs(onset - time) <= tolerance)
			{
				return true;
			}
		}
		return false;
	}

	// SEH-guarded reads and the guarded native call live in their own functions:
	// C++ objects and __try cannot share a frame (C2712). The visual pointer can be
	// garbage (initializer call sites do not populate EDX) or freed (note despawned
	// between holds), so every dereference of a remembered pointer is guarded.
	bool TryReadVisualOnset(uintptr_t visual, float* out) noexcept
	{
		if (visual < 0x10000 || (visual & 3) != 0) return false;
		__try
		{
			*out = *reinterpret_cast<volatile float*>(visual + 0x38);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// The visual's vtable pointer, for telling fretboard string markers apart from
	// highway note visuals (the type split the window whitelist needs).
	bool TryReadVisualVtable(uintptr_t visual, uintptr_t* out) noexcept
	{
		if (visual < 0x10000 || (visual & 3) != 0) return false;
		__try
		{
			*out = *reinterpret_cast<volatile uintptr_t*>(visual);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool TrySyntheticDim(uintptr_t visual, uintptr_t owner) noexcept
	{
		__try
		{
			reinterpret_cast<NativeMarkerUpdater>(NECK_MARKER_UPDATER)(
				reinterpret_cast<void*>(visual), reinterpret_cast<void*>(owner));
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// The decision logic, ABI-free: called by the naked thunk below with everything
	// already computed. Returns the final answer for AX. edxVisual is the caller's EDX:
	// only meaningful when the caller is the per-frame updater (which keeps the note
	// visual there); initializer call sites leave garbage, so it is never dereferenced
	// without the guarded read.
	// The per-frame marker updater's return site (the caller at 0x79D092 that keeps
	// the note visual in EDX). Queries returning there drive the 3D fretboard string
	// markers; every other return site is one of the four rebuild initializers.
	constexpr uint32_t WINDOW_UPDATER_RETURN = 0x0079D092;

	uint16_t __cdecl WindowConditionHelper(void* windowStruct, float time, uint32_t native,
		void* edxVisual, uint32_t returnAddress)
	{
		const auto result = static_cast<uint16_t>(native);
		const long placementMode = neckPlacementMode;
		if (placementMode != 3 || windowStruct == nullptr)
		{
			return result;
		}

		NoteByNoteProtocol::NoteByNoteState state;
		if (!NoteByNoteController::TryGetNoteByNoteState(state) || state.ownsNativeHold == 0)
		{
			return result;
		}
		const bool isSteadyHold =
			state.gatePhase == NoteByNoteProtocol::GatePhase::Holding;
		const bool isUpdaterCall = returnAddress == WINDOW_UPDATER_RETURN;

		// Stale-marker capture and dim: STEADY holds only. The synthetic updater call
		// must never run mid-rebuild (the remembered visual may be mid-destruction), and
		// the capture is only meaningful while the target is stable. Skipped while the
		// synthetic updater call below is on the stack (it re-enters this helper via the
		// game's own condition call).
		// ORDER MATTERS: the reconcile runs before the capture, because the first query
		// of a NEW hold is usually the new target itself - capturing first would
		// overwrite the remembered previous-target visual before it could be dimmed
		// (v7.8's bug: the trigger never fired).
		if (isSteadyHold && syntheticDimActive == 0)
		{
			// Past-onset list lifecycle: a target moving meaningfully backward is a
			// section wrap or re-arm - the old onsets belong to a retired pool.
			if (state.selectedRecordTime < staleDimLastTarget - 1.0f)
			{
				staleDimOnsetCount = 0;
			}
			staleDimLastTarget = state.selectedRecordTime;

			if (staleCandidateVisual != 0
				&& std::fabs(staleCandidateOnset - state.selectedRecordTime)
					> ONSET_TOLERANCE_SECONDS)
			{
				// The remembered visual belongs to a previous target: deal with it exactly
				// once. Forward motion dims it; backward motion is a section wrap where
				// the remembered pointer is stale by construction - drop it.
				const uintptr_t visual = staleCandidateVisual;
				const uintptr_t owner = staleCandidateOwner;
				const float onset = staleCandidateOnset;
				const uintptr_t capturedVtable = staleCandidateVtable;
				staleCandidateVisual = 0;
				bool dimmed = false;
				float onsetNow = 0.0f;
				uintptr_t vtableNow = 0;
				if (state.selectedRecordTime > onset + ONSET_TOLERANCE_SECONDS
					&& TryReadVisualOnset(visual, &onsetNow)
					&& std::fabs(onsetNow - onset) <= ONSET_TOLERANCE_SECONDS
					// Identity: the pointer must still name the SAME KIND of object it
					// was captured as. A reused allocation with a coincidentally matching
					// onset float must never receive the synthetic updater call.
					&& TryReadVisualVtable(visual, &vtableNow)
					&& vtableNow == capturedVtable)
				{
					InterlockedExchange(&syntheticDimActive, 1);
					dimmed = TrySyntheticDim(visual, owner);
					InterlockedExchange(&syntheticDimActive, 0);
				}
				if (dimmed && staleDimOnsetCount < STALE_DIM_ONSET_CAPACITY)
				{
					// Dimmed once; the past-onset capture below must not re-claim it.
					staleDimOnsets[staleDimOnsetCount++] = onset;
				}
				if (InterlockedDecrement(&staleDimLogBudget) >= 0)
				{
					LOG_INFO("(NBN PLACEMENT) stale marker "
						<< (dimmed ? "dimmed" : "dropped")
						<< " visual=0x" << std::hex << visual << std::dec
						<< std::fixed << std::setprecision(3)
						<< " onset=" << onset
						<< " newTarget=" << state.selectedRecordTime << std::endl);
				}
			}
			if (std::fabs(time - state.selectedRecordTime) <= ONSET_TOLERANCE_SECONDS)
			{
				// A query for the CURRENT target: if EDX carries a visual whose onset
				// agrees, this is the updater and EDX names the target's visual. The
				// vtable must be the updater-visual class at capture time - anything
				// else is an initializer's garbage EDX that happened to be readable.
				// Never displace a pending PAST-onset candidate (it fires next query;
				// the target's own capture repeats every frame and loses nothing).
				const bool slotHoldsPastCandidate = staleCandidateVisual != 0
					&& std::fabs(staleCandidateOnset - state.selectedRecordTime)
						> ONSET_TOLERANCE_SECONDS;
				float edxOnset = 0.0f;
				uintptr_t edxVtable = 0;
				if (!slotHoldsPastCandidate
					&& TryReadVisualOnset(reinterpret_cast<uintptr_t>(edxVisual), &edxOnset)
					&& std::fabs(edxOnset - time) <= ONSET_TOLERANCE_SECONDS
					&& TryReadVisualVtable(reinterpret_cast<uintptr_t>(edxVisual), &edxVtable)
					&& edxVtable == UPDATER_VISUAL_VTABLE)
				{
					staleCandidateVisual = reinterpret_cast<uintptr_t>(edxVisual);
					staleCandidateOwner = reinterpret_cast<uintptr_t>(windowStruct) - 0x3B0;
					staleCandidateOnset = time;
					staleCandidateVtable = edxVtable;
				}
			}
			else if (isUpdaterCall
				&& time < state.selectedRecordTime - ONSET_TOLERANCE_SECONDS
				&& staleDimOnsetCount < STALE_DIM_ONSET_CAPACITY
				&& !IsStaleDimmedOnset(time, ONSET_TOLERANCE_SECONDS)
				&& !IsGroupOnset(state, time, ONSET_TOLERANCE_SECONDS))
			{
				const bool slotFree = staleCandidateVisual == 0
					|| std::fabs(staleCandidateOnset - state.selectedRecordTime)
						<= ONSET_TOLERANCE_SECONDS;
				float edxOnset = 0.0f;
				uintptr_t edxVtable = 0;
				if (slotFree
					&& TryReadVisualOnset(reinterpret_cast<uintptr_t>(edxVisual), &edxOnset)
					&& std::fabs(edxOnset - time) <= ONSET_TOLERANCE_SECONDS
					&& TryReadVisualVtable(reinterpret_cast<uintptr_t>(edxVisual), &edxVtable)
					&& edxVtable == UPDATER_VISUAL_VTABLE)
				{
					staleCandidateVisual = reinterpret_cast<uintptr_t>(edxVisual);
					staleCandidateOwner = reinterpret_cast<uintptr_t>(windowStruct) - 0x3B0;
					staleCandidateOnset = time;
					staleCandidateVtable = edxVtable;
				}
			}

			// Upcoming-marker dim (v8). Per-hold state: the forced list belongs to one
			// target and resets when the target advances, so a dimmed note that becomes
			// the target gets natural answers again.
			if (std::fabs(upcomingDimHoldOnset - state.selectedRecordTime)
				> ONSET_TOLERANCE_SECONDS)
			{
				upcomingDimHoldOnset = state.selectedRecordTime;
				upcomingDimCount = 0;
				upcomingCandidateVisual = 0;
			}
			if (upcomingCandidateVisual != 0)
			{
				// Deferred from the query that captured it, exactly like the stale dim:
				// never issue the synthetic call from inside that note's own updater call.
				const uintptr_t visual = upcomingCandidateVisual;
				const uintptr_t owner = upcomingCandidateOwner;
				const float onset = upcomingCandidateOnset;
				const uintptr_t capturedVtable = upcomingCandidateVtable;
				upcomingCandidateVisual = 0;
				float onsetNow = 0.0f;
				uintptr_t vtableNow = 0;
				if (isUpcomingDimEnabled
					&& onset > state.selectedRecordTime + ONSET_TOLERANCE_SECONDS
					&& upcomingDimCount < UPCOMING_DIM_CAPACITY
					&& TryReadVisualOnset(visual, &onsetNow)
					&& std::fabs(onsetNow - onset) <= ONSET_TOLERANCE_SECONDS
					&& TryReadVisualVtable(visual, &vtableNow)
					&& vtableNow == capturedVtable)
				{
					// List FIRST: the synthetic call's own condition query is forced
					// outside by the list rule below, which is what makes the game dim
					// its marker; the same rule then keeps it dim for the hold.
					upcomingDimOnsets[upcomingDimCount++] = onset;
					InterlockedExchange(&syntheticDimActive, 1);
					const bool dimmed = TrySyntheticDim(visual, owner);
					InterlockedExchange(&syntheticDimActive, 0);
					if (!dimmed && upcomingDimCount > 0)
					{
						--upcomingDimCount;
					}
					if (InterlockedDecrement(&upcomingDimLogBudget) >= 0)
					{
						LOG_INFO("(NBN UPDIM) upcoming marker "
							<< (dimmed ? "dimmed" : "call failed")
							<< " visual=0x" << std::hex << visual << std::dec
							<< std::fixed << std::setprecision(3)
							<< " onset=" << onset
							<< " target=" << state.selectedRecordTime
							<< " held=" << upcomingDimCount << std::endl);
					}
				}
			}
			else if (isUpdaterCall
				&& time > state.selectedRecordTime + ONSET_TOLERANCE_SECONDS
				&& result == 0
				&& upcomingDimCount < UPCOMING_DIM_CAPACITY
				&& !IsUpcomingDimmedOnset(time, ONSET_TOLERANCE_SECONDS)
				&& !IsGroupOnset(state, time, ONSET_TOLERANCE_SECONDS))
			{
				float edxOnset = 0.0f;
				uintptr_t edxVtable = 0;
				if (TryReadVisualOnset(reinterpret_cast<uintptr_t>(edxVisual), &edxOnset)
					&& std::fabs(edxOnset - time) <= ONSET_TOLERANCE_SECONDS
					&& TryReadVisualVtable(reinterpret_cast<uintptr_t>(edxVisual), &edxVtable)
					&& edxVtable == UPDATER_VISUAL_VTABLE)
				{
					upcomingCandidateVisual = reinterpret_cast<uintptr_t>(edxVisual);
					upcomingCandidateOwner = reinterpret_cast<uintptr_t>(windowStruct) - 0x3B0;
					upcomingCandidateOnset = time;
					upcomingCandidateVtable = edxVtable;
				}
			}
		}

		// Measured live (v7.6 log): the struct is the SECTION window - one object, bounds
		// equal to the practice section - and the float argument is the NOTE'S ONSET
		// stepping through 15.347, 15.479, 15.598... So the note's identity is the time
		// argument itself; the section bounds are context.
		const float start = *reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(windowStruct) + 0x14);
		const float end = *reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(windowStruct) + 0x1C);
		(void)start; (void)end;

		if (isUpdaterCall && InterlockedDecrement(&vtableLogBudget) >= 0)
		{
			uintptr_t vtable = 0;
			float visualOnset = -1.0f;
			const bool hasVtable = TryReadVisualVtable(
				reinterpret_cast<uintptr_t>(edxVisual), &vtable);
			TryReadVisualOnset(reinterpret_cast<uintptr_t>(edxVisual), &visualOnset);
			LOG_INFO("(NBN PLACEMENT) updater visual vtable=0x" << std::hex << vtable
				<< (hasVtable ? "" : " (unreadable)")
				<< " visual=0x" << reinterpret_cast<uintptr_t>(edxVisual) << std::dec
				<< std::fixed << std::setprecision(3)
				<< " visualOnset=" << visualOnset
				<< " queryTime=" << time
				<< " target=" << state.selectedRecordTime
				<< " native=" << result << std::endl);
		}
		// v8.2: dimmed onsets stay forced outside for the rest of the hold - v8.1 proved
		// the marker relights on the next natural inside answer, so persistence is the
		// only way the dim survives. The list now only ever contains REVEALED markers
		// (v8's whole-section version grayed a highway of faces), so the collateral is
		// exactly the revealed notes' faces. Strictly upcoming: the current target's
		// onset can never match, so a retarget onto a dimmed note returns to natural
		// answers and relights.
		if (isUpdaterCall
			&& time > state.selectedRecordTime + ONSET_TOLERANCE_SECONDS
			&& IsUpcomingDimmedOnset(time, ONSET_TOLERANCE_SECONDS))
		{
			return 1;
		}

		// Directional: upcoming notes (target included) keep their native "inside"
		// answer; played notes report outside and dim. This preserves the colored
		// highway while the fretboard-marker lever is investigated independently.
		const bool isWhitelisted =
			time > state.selectedRecordTime - ONSET_TOLERANCE_SECONDS;

		if (InterlockedDecrement(&windowLogBudget) >= 0)
		{
			LOG_INFO("(NBN PLACEMENT) window check struct=0x" << std::hex
				<< reinterpret_cast<uintptr_t>(windowStruct) << std::dec
				<< std::fixed << std::setprecision(3)
				<< " start=" << start << " end=" << end << " time=" << time
				<< " native=" << result
				<< " targetTime=" << state.selectedRecordTime
				<< (isWhitelisted ? " WHITELISTED" : " forced-outside") << std::endl);
		}

		if (isWhitelisted || result != 0) return result;

		const auto total = InterlockedIncrement(&windowForcedCount);
		if (total == 1 || total % 500 == 0)
		{
			LOG_INFO("(NBN PLACEMENT) window forced outside: " << total
				<< std::fixed << std::setprecision(3) << " (last start=" << start << ")"
				<< std::endl);
		}
		return 1;
	}

	// The ABI truth about 0x7E9ED0, learned from two crash dumps: its caller at 0x79D092
	// writes through EDX immediately after the call, so the condition must PRESERVE EDX -
	// a contract no compiler-generated wrapper honors (v7.3's updater hook and v7.5's
	// compiled condition wrapper both corrupted it; dumps 88372 at 0x78B73A and 92212 at
	// 0x79D092). This thunk saves ECX/EDX, runs the original (thiscall: ECX this, one
	// stack float, callee pops), hands the decision to the cdecl helper above - including
	// the saved EDX, which for updater calls is the note visual (stale-marker capture) -
	// restores the saved registers exactly, and returns with ret 4.
	__declspec(naked) void WindowConditionNakedDetour()
	{
		__asm
		{
			push ecx                        // saved this
			push edx                        // saved side-channel EDX
			mov eax, [esp + 12]             // the float argument
			push eax
			call originalWindowCondition    // thiscall: consumes the pushed float
			movzx eax, ax
			mov edx, [esp]                  // saved side-channel EDX (scratch; restored below)
			mov ecx, [esp + 8]              // the caller's return address (ECX is scratch here;
			                                // the real ECX is restored from [esp+4] at the end)
			push ecx                        // returnAddress: [0]=ra [4]=edx [8]=ecx [12]=ret [16]=float
			push edx                        // edxVisual:     [0]=edxV [4]=ra [8]=edx [12]=ecx [16]=ret [20]=float
			push eax                        // native:        [0]=native [4]=edxV [8]=ra [12]=edx [16]=ecx [20]=ret [24]=float
			mov eax, [esp + 24]             // the float argument again
			push eax                        // time:          [0]=time [4]=native [8]=edxV [12]=ra [16]=edx [20]=ecx [24]=ret [28]=float
			mov eax, [esp + 20]             // saved this
			push eax                        // windowStruct
			call WindowConditionHelper      // cdecl: caller cleans
			add esp, 20
			pop edx                         // restore the side-channel exactly
			pop ecx
			ret 4
		}
	}

	// The __thiscall(this, byte) setter at 0x7A9220: dry-log only, with its flag argument.
	// A __fastcall detour with a dummy EDX parameter matches the thiscall ABI (ECX this,
	// stack argument, callee pops).
	uint32_t __fastcall NeckVisibilityDetour(void* self, void* unusedEdx, uint8_t flag)
	{
		if (staleCandidateVisual != 0 && self != nullptr)
		{
			const auto dyingParent = *reinterpret_cast<uintptr_t*>(
				reinterpret_cast<uintptr_t>(self) + 0x8);
			if (dyingParent == staleCandidateVisual
				|| reinterpret_cast<uintptr_t>(self) == staleCandidateVisual)
			{
				staleCandidateVisual = 0;
			}
		}
		if (neckPlacementMode == 1 && InterlockedDecrement(&neckPlacementLogBudget) >= 0)
		{
			const auto parent = self == nullptr ? 0 : *reinterpret_cast<uintptr_t*>(
				reinterpret_cast<uintptr_t>(self) + 0x8);
			LOG_INFO("(NBN PLACEMENT) site=0x7a9220 flag=" << static_cast<int>(flag)
				<< " self=0x" << std::hex << reinterpret_cast<uintptr_t>(self)
				<< " parent=0x" << parent << std::dec
				<< (parent != 0
					? " string=" + std::to_string(*reinterpret_cast<uint8_t*>(parent + 0xC))
						+ " fret=" + std::to_string(*reinterpret_cast<uint8_t*>(parent + 0xD))
					: "")
				<< std::endl);
		}
		return originalNeckVisibility(self, unusedEdx, flag);
	}
}

void NoteByNoteHighwayRenderer::EvictRememberedDimCandidates()
{
	// Called on every NBN teardown (disable, probe reload, probe unload). The remembered
	// visuals are only revalidated by onset + vtable; across a teardown the destructor
	// detour that normally evicts a dying visual may be unhooked (it is probe-requested),
	// so a pointer captured before the teardown must never survive into the next hold.
	staleCandidateVisual = 0;
	staleCandidateOwner = 0;
	staleCandidateOnset = 0.0f;
	staleCandidateVtable = 0;
	upcomingCandidateVisual = 0;
	upcomingCandidateOwner = 0;
	upcomingCandidateOnset = 0.0f;
	upcomingCandidateVtable = 0;
	upcomingDimCount = 0;
	staleDimOnsetCount = 0;
	staleDimLastTarget = -1.0f;
	upcomingDimHoldOnset = -1.0f;
}

void NoteByNoteHighwayRenderer::SetNeckPlacementMode(long mode)
{
	if (mode < 0 || mode > 3)
	{
		LOG_ERROR("(NBN PLACEMENT) Invalid neck placement mode " << mode << "." << std::endl);
		return;
	}
	if (mode == 1) InterlockedExchange(&neckPlacementLogBudget, 40);
	if (mode == 2) InterlockedExchange(&neckPlacementRelocated, 0);
	if (mode == 3)
	{
		InterlockedExchange(&windowLogBudget, 30);
		InterlockedExchange(&windowForcedCount, 0);
		InterlockedExchange(&staleDimLogBudget, 20);
		InterlockedExchange(&vtableLogBudget, 48);
		staleCandidateVisual = 0;
	}
	InterlockedExchange(&neckPlacementMode, mode);
	LOG_INFO("(NBN PLACEMENT) Neck placement mode set to " << mode
		<< " (0 off, 1 dry, 2 target, 3 window), siteMask=0x" << std::hex
		<< neckPlacementSiteMask << std::dec << "." << std::endl);
}

long NoteByNoteHighwayRenderer::GetNeckPlacementMode()
{
	return neckPlacementMode;
}

void NoteByNoteHighwayRenderer::ArmFadeDryLog()
{
	InterlockedExchange(&fadeDryLogBudget, 150);
	LOG_INFO("(NBN FADE) armed: 150 lines over sites {0x7A8E90, 0x7A9290, 0x7A93C0}."
		<< std::endl);
}

void NoteByNoteHighwayRenderer::SetNeckPlacementSiteMask(long mask)
{
	InterlockedExchange(&neckPlacementSiteMask, mask);
	LOG_INFO("(NBN PLACEMENT) Site mask set to 0x" << std::hex << mask << std::dec
		<< " over {0x7A8B10, 0x7A8CE0, 0x7A8E90, 0x7A90B0, 0x7A9290, 0x7A93C0, 0x7A94F0}."
		<< std::endl);
}

long NoteByNoteHighwayRenderer::GetNeckPlacementSiteMask()
{
	return neckPlacementSiteMask;
}

bool NoteByNoteHighwayRenderer::InstallPresentationGate()
{
	if (isGateInstalled) return true;

	originalPredicate = reinterpret_cast<NativePredicate>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_PRESENTABILITY_PREDICATE),
		reinterpret_cast<PBYTE>(&PredicateDetour)));
	if (originalPredicate == nullptr)
	{
		LOG_ERROR("(NBN PRESENTATION) Installing the presentability detour at 0x7A5CE0 failed;"
			<< " the highway will render unmodified." << std::endl);
		return false;
	}

	originalNotewaySetup = reinterpret_cast<NativeNotewaySetup>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_NOTEWAY_SETUP),
		reinterpret_cast<PBYTE>(&NotewaySetupDetour)));
	if (originalNotewaySetup == nullptr)
	{
		LOG_ERROR("(NBN PRESENTATION) Installing the noteway setup detour at 0x7E22F0 failed;"
			<< " the scrolling highway will render unmodified." << std::endl);
		return false;
	}

	originalDrawDescriptors = reinterpret_cast<NativeDrawDescriptors>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_DRAW_DESCRIPTORS),
		reinterpret_cast<PBYTE>(&DrawDescriptorsDetour)));
	if (originalDrawDescriptors == nullptr)
	{
		LOG_ERROR("(NBN PRESENTATION) Installing the descriptor draw detour at 0x7A4B50 failed;"
			<< " the scrolling highway will render unmodified." << std::endl);
		return false;
	}

	originalDrawGeometry = reinterpret_cast<NativeDrawGeometry>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_DRAW_GEOMETRY),
		reinterpret_cast<PBYTE>(&DrawGeometryDetour)));
	originalDrawSubmit = reinterpret_cast<NativeDrawSubmit>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_DRAW_SUBMIT),
		reinterpret_cast<PBYTE>(&DrawSubmitDetour)));
	if (originalDrawGeometry == nullptr || originalDrawSubmit == nullptr)
	{
		LOG_ERROR("(NBN PRESENTATION) Installing the highway draw detours failed"
			<< " (geometry=" << (originalDrawGeometry != nullptr)
			<< ", submit=" << (originalDrawSubmit != nullptr)
			<< "); the scrolling highway will render unmodified." << std::endl);
		return false;
	}

	// Where the finished per-frame fretboard buffer is consumed, which is the only valid point
	// to read it.
	originalBufferConsume = reinterpret_cast<NativeBufferConsume>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_FRETBOARD_BUFFER_CONSUME),
		reinterpret_cast<PBYTE>(&BufferConsumeDetour)));
	if (originalBufferConsume == nullptr)
	{
		LOG_ERROR("(NBN PRESENTATION) Installing the fretboard buffer detour at 0x7AA1B0"
			<< " failed; the finished neck-diagram state cannot be read." << std::endl);
		return false;
	}

	// Below both paths into the neck diagram, including the one that skips the predicate.
	originalGridWrite = reinterpret_cast<NativeGridWrite>(DetourFunction(
		reinterpret_cast<PBYTE>(NATIVE_FRETBOARD_GRID_WRITE),
		reinterpret_cast<PBYTE>(&GridWriteDetour)));
	if (originalGridWrite == nullptr)
	{
		LOG_ERROR("(NBN PRESENTATION) Installing the neck-diagram grid detour at 0x7AA140"
			<< " failed; the fretboard will keep showing notes the predicate gate cannot"
			<< " reach." << std::endl);
		return false;
	}

	for (size_t site = 0; site < NECK_PLACEMENT_SITE_COUNT; ++site)
	{
		originalNeckPlacements[site] = reinterpret_cast<NativeNeckPlacement>(DetourFunction(
			reinterpret_cast<PBYTE>(NECK_PLACEMENT_SITES[site]),
			reinterpret_cast<PBYTE>(NECK_PLACEMENT_DETOURS[site])));
		if (originalNeckPlacements[site] == nullptr)
		{
			LOG_ERROR("(NBN PLACEMENT) Installing the placement detour at 0x" << std::hex
				<< NECK_PLACEMENT_SITES[site] << std::dec
				<< " failed; read-ahead markers cannot be relocated." << std::endl);
			return false;
		}
	}
	originalNeckVisibility = reinterpret_cast<NativeNeckVisibility>(DetourFunction(
		reinterpret_cast<PBYTE>(NECK_VISIBILITY_SITE),
		reinterpret_cast<PBYTE>(&NeckVisibilityDetour)));
	if (originalNeckVisibility == nullptr)
	{
		LOG_ERROR("(NBN PLACEMENT) Installing the visibility detour at 0x7A9220 failed."
			<< std::endl);
		return false;
	}

	(void)&originalMarkerUpdater;
	originalWindowCondition = reinterpret_cast<NativeWindowCondition>(DetourFunction(
		reinterpret_cast<PBYTE>(NECK_WINDOW_CONDITION),
		reinterpret_cast<PBYTE>(&WindowConditionNakedDetour)));
	if (originalWindowCondition == nullptr)
	{
		LOG_ERROR("(NBN PLACEMENT) Installing the window-condition detour at 0x7E9ED0"
			<< " failed; mode window is unavailable." << std::endl);
		return false;
	}

	isGateInstalled = true;
	LOG_INFO("(NBN PRESENTATION) Gates installed: fretboard grid at 0x7A5CE0 and 0x7AA140,"
		<< " highway pools at"
		<< " 0x7E22F0, descriptors at 0x7A4B50, and highway draw at 0x7A52C0/0x7A6160."
		<< " 0x7A4AA0 is left running on purpose: gating it changed nothing visually and"
		<< " starved per-note colour state. Non-selected notes are only skipped at draw"
		<< " time; legato notes in the published group stay visible." << std::endl);
	return true;
}

void NoteByNoteHighwayRenderer::RefreshSelectedTarget(void* owner)
{
	NoteByNoteProtocol::NoteByNoteState state;
	const bool haveState = NoteByNoteNativeScoring::TryGetStateSnapshot(state);

	if (haveState)
	{
		const bool ownsHold = state.ownsNativeHold != 0;
		const bool epochChanged = hasLastSeenEpoch && state.epoch != lastSeenEpoch;
		const bool recordChanged = hasLastSeenSelectedRecord
			&& state.selectedRecord != lastSeenSelectedRecord;
		const bool holdReleased = hasLastOwnsNativeHold && lastOwnsNativeHold && !ownsHold;
		if (epochChanged || recordChanged || holdReleased)
		{
			presentationSettleFrames = PRESENTATION_SETTLE_FRAMES;
			LOG_INFO("(NBN PRESENTATION) Hold transition (epoch " << lastSeenEpoch << "->"
				<< state.epoch << (recordChanged ? ", record changed" : "")
				<< (holdReleased ? ", hold released" : "")
				<< "); holding the presentability gate open for " << PRESENTATION_SETTLE_FRAMES
				<< " frames so Rocksmith retires nothing while it rebuilds the note pool."
				<< std::endl);
		}
		lastSeenEpoch = state.epoch;
		hasLastSeenEpoch = true;
		lastSeenSelectedRecord = state.selectedRecord;
		hasLastSeenSelectedRecord = true;
		lastOwnsNativeHold = ownsHold;
		hasLastOwnsNativeHold = true;
	}
	if (presentationSettleFrames > 0)
	{
		--presentationSettleFrames;
	}

	if (!haveState
		|| state.isInitialized == 0
		|| state.gatePhase == NoteByNoteProtocol::GatePhase::Idle
		|| state.trackedOwner != reinterpret_cast<uintptr_t>(owner)
		|| state.visualRecord == 0)
	{
		ClearSelectedTarget();
		return;
	}
	if (state.visualChordId != -1)
	{
		// Chords deliberately fall through to stock rendering for the single-target
		// consumers: a chord is several notes sharing one target, and suppressing its
		// siblings would hide notes the player still has to play. The quad filter is
		// the exception - it activates on the stashed chord shape (matched by chordId,
		// fail-open on any mismatch) so future-note markers stop painting during
		// chord holds too.
		ClearSelectedTarget();
		isChordTargetActive = chordShapeChordId == state.visualChordId;
		return;
	}

	uint32_t groupCount = state.visualGroupCount;
	if (groupCount > NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup)
	{
		groupCount = NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup;
	}
	for (uint32_t i = 0; i < groupCount; ++i)
	{
		visualGroupRecords[i] = state.visualGroupRecords[i];
		visualGroupStrings[i] = state.visualGroupStrings[i];
		visualGroupFrets[i] = state.visualGroupFrets[i];
	}
	visualGroupCount = groupCount;
	selectedRecord = state.visualRecord;
	selectedStringIndex = state.visualString;
	selectedFret = state.visualFret;
	selectedRecordTime = state.selectedRecordTime;
	hasSelectedRecordTime = true;
	isNativeHoldOwned = state.ownsNativeHold != 0;
	isChordTargetActive = false;
	isTargetActive = true;

	if (loggedRecord != state.visualRecord)
	{
		loggedRecord = state.visualRecord;
		// Both fretboard surfaces, because the predicate and the grid write see different
		// populations: a note reaching the diagram through the predicate-skipping path in
		// 0x7A5D50 is counted here and not there.
		LOG_INFO("(NBN FRETBOARD) Grid " << gridSuppressed << "/" << gridCalls
			<< " suppressed, predicate " << fretboardSuppressed
			<< "/" << fretboardCalls << " suppressed, since last target ("
			<< selectedStringIndex << ':' << selectedFret
			<< " +" << visualGroupCount << " group)." << std::endl);
		fretboardCalls = 0;
		fretboardSuppressed = 0;
		gridCalls = 0;
		gridSuppressed = 0;
		gridProbesLogged = 0;
		// A few consecutive frames, so a steady cell is
		// distinguishable from a flickering one.
		fretboardDumpFramesRemaining = 3;

		// The earlier dump that lived here read the buffer from this point in the frame, which
		// is before render preparation initialises and fills it, so it reported the previous
		// frame's residue and made the surface look wrong. Reading it correctly happens in
		// ReportFretboardBuffer, at the consumer.

		LOG_INFO("(NBN PRESENTATION) Draw gate since last target: descriptors "
			<< descriptorSuppressed << "/" << descriptorCalls
			<< " suppressed, geometry " << geometrySuppressed << "/" << geometryCalls
			<< " suppressed, submit " << submitSuppressed << "/" << submitCalls
			<< " suppressed, group=" << visualGroupCount << "." << std::endl);
		descriptorCalls = 0;
		descriptorSuppressed = 0;
		geometryCalls = 0;
		geometrySuppressed = 0;
		submitCalls = 0;
		submitSuppressed = 0;

		LOG_INFO("(NBN PRESENTATION) Presenting only record=0x" << std::hex
			<< state.visualRecord << std::dec
			<< " (" << state.visualString << ':' << state.visualFret << ")."
			<< " Every other note is reported not presentable, so Rocksmith retires it"
			<< " through its own lifecycle." << std::endl);
	}
}

void NoteByNoteHighwayRenderer::ClearSelectedTarget()
{
	isTargetActive = false;
	isChordTargetActive = false;
	isNativeHoldOwned = false;
	selectedRecord = 0;
	selectedStringIndex = -1;
	selectedFret = -1;
	visualGroupCount = 0;
	loggedRecord = 0;
}

void NoteByNoteHighwayRenderer::SetChordTargetShape(
	int32_t chordId,
	const int* frets,
	const int* fingers)
{
	if (frets == nullptr) return;
	for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
	{
		chordShapeFrets[stringIndex] = frets[stringIndex];
		chordShapeFingers[stringIndex] = fingers != nullptr ? fingers[stringIndex] : 0;
	}
	chordShapeChordId = chordId;
}

bool NoteByNoteHighwayRenderer::TryGetChordFingerForCoordinate(
	int stringIndex,
	int fret,
	int& finger)
{
	if (!isChordTargetActive) return false;
	if (stringIndex < 0 || stringIndex >= 6) return false;
	if (chordShapeFrets[stringIndex] != fret) return false;
	const int candidate = chordShapeFingers[stringIndex];
	if (candidate < 1 || candidate > 4) return false;
	finger = candidate;
	return true;
}

bool NoteByNoteHighwayRenderer::IsChordHoldActive()
{
	return isChordTargetActive;
}

bool NoteByNoteHighwayRenderer::TryGetMarkerKeepCoordinates(
	int (&strings)[8],
	int (&frets)[8],
	int& count)
{
	count = 0;
	if (isTargetActive)
	{
		const int targetString = selectedStringIndex;
		const int targetFret = selectedFret;
		if (targetString < 0 || targetFret < 0) return false;
		strings[count] = targetString;
		frets[count] = targetFret;
		++count;
		// The legato group rides along so a hammer-on run's markers survive the
		// filter the same way they survive the fretboard suppression.
		uint32_t groupCount = visualGroupCount;
		if (groupCount > NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup)
		{
			groupCount = NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup;
		}
		for (uint32_t index = 0; index < groupCount && count < 8; ++index)
		{
			strings[count] = visualGroupStrings[index];
			frets[count] = visualGroupFrets[index];
			++count;
		}
		return count > 0;
	}
	if (isChordTargetActive)
	{
		for (int stringIndex = 0; stringIndex < 6 && count < 8; ++stringIndex)
		{
			const int fret = chordShapeFrets[stringIndex];
			// Open and muted strings have no fretboard marker quad; only fretted
			// members join the keep set.
			if (fret < 1 || fret > 24) continue;
			strings[count] = stringIndex;
			frets[count] = fret;
			++count;
		}
		return count > 0;
	}
	return false;
}

bool NoteByNoteHighwayRenderer::TryGetSelectedTargetForDiagnostics(int& stringIndex, int& fret)
{
	if (!isTargetActive) return false;

	const int candidateString = selectedStringIndex;
	const int candidateFret = selectedFret;
	if (candidateString < 0 || candidateFret < 0) return false;

	stringIndex = candidateString;
	fret = candidateFret;
	return true;
}

bool NoteByNoteHighwayRenderer::TryGetSelectedPresentation(int& stringIndex, int& fret)
{
	// Returning false leaves the instance filter dormant, so it never locks a vertex
	// buffer. That also removes it as a suspect for the ASIO input xruns.
	if (static_cast<NoteByNoteHighwayRenderer::HighwayMode>(highwayMode)
		== NoteByNoteHighwayRenderer::HighwayMode::Off)
	{
		return false;
	}
	if (!isTargetActive) return false;

	const int candidateString = selectedStringIndex;
	const int candidateFret = selectedFret;
	if (candidateString < 0 || candidateFret < 0) return false;

	stringIndex = candidateString;
	fret = candidateFret;
	return true;
}

bool NoteByNoteHighwayRenderer::HasSelectedPresentation()
{
	int stringIndex = -1;
	int fret = -1;
	return TryGetSelectedPresentation(stringIndex, fret);
}

const char* NoteByNoteHighwayRenderer::DescribeGridGateMode(GridGateMode mode)
{
	switch (mode)
	{
	case GridGateMode::Off: return "off";
	case GridGateMode::Restore: return "restore";
	case GridGateMode::Sentinel: return "sentinel";
	case GridGateMode::SkipKnownBad: return "skip-known-bad";
	}
	return "unknown";
}

void NoteByNoteHighwayRenderer::SetGridGateMode(GridGateMode mode)
{
	// Skip is refused rather than merely undocumented. It is kept in the enum so the next
	// reader can see it was tried and why it failed, but selecting it would reintroduce the
	// suppressed container operation that faulted the controller.
	if (mode == GridGateMode::SkipKnownBad)
	{
		LOG_ERROR("(NBN GRID) Refusing grid gate mode 'skip': skipping 0x7AA140 also skips its"
			<< " unconditional container operation on ESI+0xD04, which faulted the controller"
			<< " with \"the selected native record expired before the hold could be"
			<< " established\". Use restore or sentinel." << std::endl);
		return;
	}

	gridGateMode = static_cast<long>(mode);
	LOG_INFO("(NBN GRID) Grid gate mode set to " << DescribeGridGateMode(mode)
		<< ". The neck diagram shows the target plus its published legato group; every other"
		<< " cell is corrected after the original write runs." << std::endl);
}

NoteByNoteHighwayRenderer::GridGateMode NoteByNoteHighwayRenderer::GetGridGateMode()
{
	return static_cast<GridGateMode>(gridGateMode);
}

void NoteByNoteHighwayRenderer::SetGridSentinel(uint32_t valueWord, float keyWord)
{
	gridSentinelValue = valueWord;
	gridSentinelKey = keyWord;
	LOG_INFO("(NBN GRID) Sentinel set to value=0x" << std::hex << valueWord << std::dec
		<< " key=" << keyWord << ". Used only in sentinel mode." << std::endl);
}

void NoteByNoteHighwayRenderer::GetGridSentinel(uint32_t& valueWord, float& keyWord)
{
	valueWord = gridSentinelValue;
	keyWord = gridSentinelKey;
}

const char* NoteByNoteHighwayRenderer::DescribeHighwayMode(HighwayMode mode)
{
	switch (mode)
	{
	case HighwayMode::Off: return "off";
	case HighwayMode::All: return "all";
	case HighwayMode::NearTargetWindow: return "near-target";
	}
	return "unknown";
}

void NoteByNoteHighwayRenderer::SetUpcomingDimEnabled(bool enabled)
{
	isUpcomingDimEnabled = enabled;
	LOG_INFO("(NBN UPDIM) Upcoming-marker dim "
		<< (enabled ? "enabled (beta behavior: upcoming notes gray while frozen)."
			: "disabled: upcoming notes keep their colour while frozen; watch for"
			  " the double-marker bug this dim originally fixed.")
		<< std::endl);
}

bool NoteByNoteHighwayRenderer::GetUpcomingDimEnabled()
{
	return isUpcomingDimEnabled;
}

void NoteByNoteHighwayRenderer::SetHighwayMode(HighwayMode mode)
{
	highwayMode = static_cast<long>(mode);
	LOG_INFO("(NBN HIGHWAY) Highway mode set to " << DescribeHighwayMode(mode)
		<< ". off leaves the highway stock; all hides every note outside the gesture and"
		<< " removes the read-ahead; near-target hides only non-gesture notes within "
		<< highwayWindowSeconds << "s of the target either side, which are the ones the"
		<< " frozen window parks at the plane." << std::endl);
}

NoteByNoteHighwayRenderer::HighwayMode NoteByNoteHighwayRenderer::GetHighwayMode()
{
	return static_cast<HighwayMode>(highwayMode);
}

void NoteByNoteHighwayRenderer::SetHighwayWindowSeconds(float seconds)
{
	// Bounded so a bad value cannot hide the whole chart or silently do nothing.
	if (!(seconds >= 0.0f) || seconds > 5.0f)
	{
		LOG_ERROR("(NBN HIGHWAY) Refusing a highway window of " << seconds
			<< "s; it must be between 0 and 5." << std::endl);
		return;
	}
	highwayWindowSeconds = seconds;
	LOG_INFO("(NBN HIGHWAY) Highway window set to " << seconds
		<< "s either side of the target." << std::endl);
}

float NoteByNoteHighwayRenderer::GetHighwayWindowSeconds()
{
	return highwayWindowSeconds;
}
