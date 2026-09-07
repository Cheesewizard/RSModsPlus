#include "NoteByNoteRuntime.hpp"
#include "MlConfirmationState.hpp"
#include "PickedAttackQueue.hpp"
#include "NoteByNoteNativeScoring.hpp"
#include "NoteByNoteScoringCore.hpp"

#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace
{
	std::atomic<bool> isHeapCheckEnabled{ true };
	std::atomic<bool> wasHeapCheckTripped{ false };

	bool TryValidateOneHeap(HANDLE heap) noexcept
	{
		__try
		{
			return HeapValidate(heap, 0, nullptr) != FALSE;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	SIZE_T TryQueryHeapBlockSize(HANDLE heap, void* block) noexcept
	{
		__try
		{
			const SIZE_T size = HeapSize(heap, 0, block);
			return size == static_cast<SIZE_T>(-1) ? 0 : size;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	void HeapCheckpoint(const char* seam)
	{
		if (!isHeapCheckEnabled.load(std::memory_order_relaxed)) return;
		if (wasHeapCheckTripped.load(std::memory_order_relaxed)) return;

		HANDLE heaps[64] = {};
		const DWORD heapCount = GetProcessHeaps(64, heaps);
		const DWORD checked = heapCount < 64 ? heapCount : 64;
		for (DWORD index = 0; index < checked; ++index)
		{
			if (TryValidateOneHeap(heaps[index])) continue;
			wasHeapCheckTripped.store(true, std::memory_order_relaxed);
			LOG_ERROR("(NBN HEAP CHECK) HEAP CORRUPTION DETECTED at seam=" << seam
				<< " heap=" << index << "/" << checked
				<< " handle=0x" << std::hex
				<< reinterpret_cast<uintptr_t>(heaps[index]) << std::dec
				<< ". The corrupting write happened between the previous clean"
				<< " checkpoint and this seam." << std::endl);
			return;
		}
	}

	// Native addresses proven from the captured live image (non-ASLR executable).
	constexpr uintptr_t LAS_OWNER_VTABLE = 0x11D1430;      // GamePlaysongLAS
	constexpr uintptr_t PLAYER_SONG_VTABLE = 0x119F668;    // GameComponentPlayerSong
	constexpr uintptr_t NATIVE_HIT_DECISION = 0x7E2640;    // owner vtable +0xF0
	constexpr uintptr_t STOP_TMUSIC = 0x44C130;            // PlayerSong vtable +0x24
	constexpr uintptr_t SET_FIVE_CLOCKS = 0x7DE490;        // owner vtable +0x50
	constexpr uintptr_t COORDINATED_REBUILD = 0x7E87F0;    // owner vtable +0x54
	constexpr uintptr_t MARK_ACTIVE_NOTES = 0x7E8920;      // owner in EDX, plain ret
	constexpr uintptr_t PLAY_SOUND_CSTRING = 0x7CE8D0;     // event name in ESI
	constexpr uintptr_t ENGINE_COMPENSATION = 0x1224A20;   // double, 0.053 engine constant
	constexpr uintptr_t LAS_SECTION_START = 0x3C0;
	constexpr uintptr_t LAS_SECTION_START_MIRROR = 0x3C4;
	constexpr uintptr_t LAS_SECTION_END = 0x3C8;
	constexpr uintptr_t LAS_SECTION_END_MIRROR = 0x3CC;

	constexpr uintptr_t LAS_PHRASE_SECTION_CONTAINER = 0x78;
	constexpr uintptr_t PHRASE_SECTION_VECTOR_BEGIN = 0xF4;
	constexpr uintptr_t PHRASE_SECTION_VECTOR_END = 0xF8;
	constexpr uintptr_t PHRASE_SECTION_STRIDE = 0x58;
	constexpr uintptr_t PHRASE_SECTION_START = 0x24;
	constexpr uintptr_t PHRASE_SECTION_END = 0x28;
	constexpr uint32_t PHRASE_SECTION_MAX = 1024;
	constexpr uintptr_t NOTE_VFX_VTABLE = 0x119BFB0;
	constexpr uintptr_t SPECIALIZED_NOTE_VFX_VTABLE = 0x119BFD8;
	constexpr uintptr_t BEAT_VFX_VTABLE = 0x119BF28;
	constexpr uintptr_t ARM_NOTE_VFX_PROMPT = 0x40D2E0;    // NoteVfx vtable +0x20
	constexpr uintptr_t CLEAR_NOTE_VFX_PROMPT = 0x40D360;  // NoteVfx vtable +0x24
	constexpr uintptr_t ARM_SPECIALIZED_PROMPT_BC = 0x40D710; // Specialized NoteVfx vtable +0x14
	constexpr uintptr_t ARM_SPECIALIZED_PROMPT_C0 = 0x40D790; // Specialized NoteVfx vtable +0x18
	constexpr uintptr_t CLEAR_SPECIALIZED_PROMPT = 0x40D810;  // Specialized NoteVfx vtable +0x1C
	constexpr char FREEZE_NOTE_TRACK_EVENT[] = "Play_FreezeNoteTrack";

	// Rocksmith's own un-windowed onset detector, the shipped lesson's wait-for-note
	// query (NDGetOnsetNote core). The windowed per-note decision cannot release a long
	// hold because onset timestamps live on the input-stream clock, which keeps
	// advancing while the song clock is frozen.
	//
	// Fully disassembled in docs/investigations/note-by-note-input-gates.md, which
	// corrects two things previously written here.
	//
	// It does not "edge-detect so each new pluck reports exactly once". It **dedupes on the
	// returned value** against a single global, 0x012F6920: `if (note == last) return -1;
	// last = note;`. The note itself is read fresh from the analysis ring's current frame on
	// every call, and is only reported when that frame's pitch equals the previous frame's,
	// which is where the "settled fundamental" requirement actually lives. Two consequences
	// the controller has to respect: a pitch that is already the last reported value is
	// invisible until some different value passes through, and **the call is not read-only**,
	// because it writes that global. Do not call it for diagnostics.
	//
	// Nothing else in the game touches 0x012F6920, and the only native caller of this core
	// is the Lua thunk, so during Learn a Song the controller owns the dedupe state outright.
	constexpr uintptr_t ONSET_NOTE_QUERY = 0x48DC40;
	// NDGetLoudestPlayedNote core, arrangement index in EAX.
	//
	// It returns detector+0x5F4 when two amplitude gates pass, and -1 otherwise. -1 is
	// therefore **not** "no valid note is sounding": it is "the engine's gates refused", and
	// detector+0x5F4 can still hold a valid note when that happens. The onset query is gated
	// on exactly the same two comparisons, which is why both queries go quiet together.
	constexpr uintptr_t LOUDEST_PLAYED_NOTE_QUERY = 0x48E5F0;
	// Per-string tuning offsets (int16[6]) used by the NDGetMidi core 0x48DC00,
	// combined with the standard guitar string bases it hardcodes.
	constexpr uintptr_t TUNING_OFFSETS = 0x1199D2C;
	constexpr int GUITAR_STRING_BASE_MIDI[6] = { 0x28, 0x2D, 0x32, 0x37, 0x3B, 0x40 };

	constexpr uintptr_t MOTION_NOTE_SINGLETON = 0x0135F57C;
	constexpr uintptr_t MOTION_NOTE_SINGLETON_STEP = 0x10;
	constexpr uintptr_t MOTION_NOTE_CONTAINER_STEP = 0x04;
	constexpr uintptr_t MOTION_NOTE_ARRAY_POINTER = 0x1284;
	constexpr uintptr_t MOTION_NOTE_ARRAY_COUNT = 0x1288;
	constexpr uintptr_t MOTION_NOTE_RECORD_STRIDE = 0x50;
	constexpr uintptr_t MOTION_NOTE_RECORD_PITCH = 0x28;
	constexpr uintptr_t MOTION_NOTE_RECORD_ACTIVE = 0x3C;
	constexpr uintptr_t MOTION_NOTE_RECORD_LOCATED = 0x3D;
	// Ten in every observation, but the count is read rather than assumed; this only
	// bounds the loop against a corrupt read.
	constexpr uint32_t MOTION_NOTE_MAX_RECORDS = 64;
	constexpr float BEND_UNDERBEND_SEMITONES = 0.3f;
	constexpr float BEND_VARIANCE_SEMITONES = 0.5f;
	constexpr float BEND_OVERBEND_ALLOWANCE_SEMITONES = 1.5f;

	constexpr uintptr_t DETECTION_ROOT = 0x0135F57C;
	constexpr uintptr_t DETECTION_ARRANGEMENT_GUITAR = 0x10;   // kind 2 (bass) would be +0x14
	constexpr uintptr_t DETECTION_ENGINE = 0x08;
	constexpr uintptr_t DETECTION_DETECTOR = 0x04;
	constexpr uintptr_t DETECTOR_CURRENT_NOTE = 0x5F4;         // what 0x48E5F0 returns
	constexpr uintptr_t DETECTOR_ANALYSIS_CLOCK = 0xD08;
	constexpr uintptr_t DETECTOR_GATE_QUALITY = 0xD38;
	constexpr uintptr_t DETECTOR_RING_BUFFER = 0xDB8;
	constexpr uintptr_t DETECTOR_RING_INDEX = 0xDBC;
	constexpr uintptr_t DETECTOR_RING_CAPACITY = 0xDC0;
	constexpr uintptr_t DETECTOR_RING_VALID_COUNT = 0xDC8;
	constexpr uintptr_t RING_FRAME_TIMESTAMP = 0x730;
	constexpr uintptr_t DETECTOR_GATE_LEVEL = 0xDD8;
	constexpr uintptr_t DETECTOR_PITCH_MODE = 0x11EC;
	constexpr uintptr_t DETECTOR_RING_STRIDE = 0x7D0;
	constexpr uintptr_t RING_FRAME_PITCH_MODE_TWO = 0x0C;
	constexpr uintptr_t RING_FRAME_PITCH_DEFAULT = 0x14;
	constexpr uintptr_t RING_FRAME_SEQUENCE = 0x748;
	// Private to 0x48DC40: the only two instructions that touch it are its own compare and
	// store, so during Learn a Song the controller is its sole owner.
	constexpr uintptr_t ONSET_DEDUPE_GLOBAL = 0x012F6920;
	// The gate thresholds are read from the game rather than hardcoded, so the log shows
	// what the engine is actually comparing against. These are the values recorded in the
	// investigation and are used only if the read fails.
	constexpr uintptr_t DETECTOR_GATE_LEVEL_THRESHOLD = 0x01224418;
	constexpr uintptr_t DETECTOR_GATE_QUALITY_THRESHOLD = 0x012243A0;
	constexpr double DETECTOR_GATE_LEVEL_FALLBACK = -55.0;
	constexpr double DETECTOR_GATE_QUALITY_FALLBACK = 50.0;
	// Only bounds the ring indexing against a corrupt read; the capacity is read, not assumed.
	constexpr int32_t DETECTOR_RING_MAX_CAPACITY = 4096;
	constexpr uintptr_t DETECTOR_SOUNDING_TABLE = 0x604;
	constexpr uintptr_t DETECTOR_SOUNDING_COUNT = 0x6A4;
	constexpr uintptr_t ND_STRENGTH_THRESHOLD_GLOBAL = 0x01199DD4;
	constexpr float ND_STRENGTH_THRESHOLD_FALLBACK = 5.0f;
	constexpr double ND_SOUNDING_HOLD_SECONDS = 0.18;
	constexpr int32_t ND_SOUNDING_MAX_ENTRIES = 64;
	volatile bool isNdAcceptEnabled = true;

	// A stalled hold reports its detector state at this cadence. Deliberately wall-clock
	// rather than tick-counted: a tick-counted cadence would itself be frame-coupled, which
	// is the thing under suspicion.
	constexpr double DETECTOR_SAMPLE_INTERVAL_SECONDS = 1.0;
	constexpr float DETECTOR_SPIKE_JUMP_DB = 2.5f;
	// Post-spike burst length: at the observed 30-60 ticks/s this records roughly a
	// quarter second of the attack transient, enough to see whether quality or the
	// ring pitch ever responds to the pick.
	constexpr int32_t DETECTOR_SPIKE_BURST_TICKS = 12;
	constexpr double BEND_RELEASE_GUARD_SECONDS = 0.4;
	constexpr float ONSET_EVIDENCE_LEVEL_FLOOR_DB = -65.0f;
	constexpr uint32_t ONSET_EVIDENCE_MIN_FRAMES = 10;
	// Plain picks use the raw-audio attack stream.
	// Other techniques retain their four-frame comparison. An onset flag without
	// a measurable rise is insufficient: the detector also flags ringing strings.
	constexpr int32_t ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES = 4;
	constexpr float ONSET_EVIDENCE_RISE_DB = 1.5f;
	// Bound per-tick scan work when the cursor falls far behind (a hitch between
	// scoring ticks); the newest frames are the ones trusted.
	constexpr int32_t ONSET_SCAN_MAX_FRAMES_PER_TICK = 120;
	constexpr float DETECTOR_RAW_BEND_QUALITY_FLOOR = 15.0f;
	constexpr int32_t DETECTOR_RAW_BEND_STREAK_TICKS = 3;
	constexpr float DETECTOR_RAW_BEND_STRONG_QUALITY = 70.0f;
	constexpr int32_t BEND_ACCEPT_MIN_HOLD_TICKS = 2;
	constexpr int32_t REATTACK_WINDOW_TICKS = 15;
	constexpr int32_t REATTACK_STREAK_TICKS = 3;

	// ML can confirm a held note from distinct post-target audio observations.
	volatile bool isMlStuckRescueEnabled = true;
	constexpr float ML_STUCK_RESCUE_CONF = 0.45f;
	constexpr int32_t BEND_RESCUE_STREAK = 4;

	// Owner offsets (GamePlaysongLAS).
	constexpr uintptr_t OWNER_CLOCK_PRIMARY = 0x3B4;
	constexpr uintptr_t OWNER_CLOCK_SECONDARY = 0x3B8;
	constexpr uintptr_t OWNER_CLOCK_RENDER = 0x3D0;
	constexpr uintptr_t OWNER_CLOCK_EPOCH_LOW = 0x3D4;
	constexpr uintptr_t OWNER_CLOCK_EPOCH_HIGH = 0x3D8;
	constexpr uintptr_t OWNER_COMPONENTS = 0x18;
	constexpr uintptr_t OWNER_CHILD_FLAG = 0x395;
	constexpr uintptr_t OWNER_NOTES_BEGIN = 0x41C;
	constexpr uintptr_t OWNER_NOTES_END = 0x420;

	// Native note object offsets.
	constexpr uintptr_t NOTE_RECORD = 0x2C;
	constexpr uintptr_t NOTE_EVENT_TIME = 0x38;
	constexpr uintptr_t NOTE_DETECT_WINDOW_END_DELTA = 0x94;
	constexpr uintptr_t NOTE_DETECT_WINDOW_START_DELTA = 0x98;
	// The chord template the evaluator judges the note against (query slot [7]).
	constexpr uintptr_t NOTE_CHORD_RECORD = 0x30;

	struct ChordTemplateView
	{
		uint32_t mask;
		uint8_t frets[6];
		uint8_t fingers[6];
		int32_t notes[6];
		char name[32];
	};
	static_assert(sizeof(ChordTemplateView) == 0x48, "SNG chord template stride is 0x48");
	constexpr uintptr_t NOTE_WINDOW_ENTRY = 0xA4;
	constexpr uintptr_t NOTE_WINDOW_EXIT = 0xA8;
	constexpr uintptr_t NOTE_STATE_C0 = 0xC0;
	constexpr uintptr_t NOTE_SPECIALIZED_PROMPT_STATE = 0xC8;
	constexpr uintptr_t NOTE_SPECIALIZED_PROMPT_SUPPRESS = 0xD0;
	constexpr uintptr_t NOTE_VFX_WRAPPER = 0x154;

	// NoteVfx controller and BeatVfx fork offsets.
	constexpr uintptr_t NOTE_VFX_PROMPT_FORK = 0xA4;
	constexpr uintptr_t SPECIALIZED_PROMPT_BC_FORK = 0xBC;
	constexpr uintptr_t SPECIALIZED_PROMPT_C0_FORK = 0xC0;
	constexpr uintptr_t BEAT_VFX_ACTIVE = 0x04;
	constexpr uintptr_t BEAT_VFX_ENTITY = 0x10;
	constexpr uintptr_t BEAT_VFX_PROMPT_REQUEST = 0x25;

	// SNG record offsets (standard Rocksmith 2014 note record layout, verified against
	// the held lesson snapshot: mask 0x800000, time 20.000, string 0, fret 3).
	// SNG note-mask bits. Verified against live records rather than assumed: the open
	// low E logged mask 0x802004 (SINGLE | SUSTAIN | OPEN), and Rocksmith's own
	// presentability predicate at 0x7A5CE0 tests 0x2000 and then reads record+0x3C as
	// the sustain length, which only makes sense if 0x2000 is SUSTAIN.
	constexpr uint32_t NOTE_MASK_HAMMERON = 0x00000200;
	constexpr uint32_t NOTE_MASK_PULLOFF = 0x00000400;
	constexpr uint32_t NOTE_MASK_BEND = 0x00001000;
	// A tapped note (right- or left-hand tap) is fretted without a pick, exactly like a
	// hammer-on or pull-off, so it is treated as a no-pick legato target: confirmed by pitch
	// rather than waiting for a pick onset that never arrives. Verified live on Through the Fire
	// and the Flames: the tapped notes carry mask 0x904000 (SINGLE | RIGHTHAND | TAP), and
	// without this handling they armed the freezer and then stuck (no pick onset ever came).
	constexpr uint32_t NOTE_MASK_TAP = 0x00004000;

	// A hammer-on or pull-off is played without a pick attack, so the edge-detected
	// onset query frequently reports nothing at all for it: the string is already
	// sounding and only the fretted pitch changes. The current-pitch query does see
	// it. Requiring a settled reading avoids accepting the momentary pitches crossed
	// on the way to the target.
	constexpr uint32_t LEGATO_CONFIRMATION_TICKS = 2;
	constexpr double HOLD_SAFETY_RELEASE_SECONDS = 60.0;
	constexpr double CHORD_HOLD_SAFETY_RELEASE_SECONDS = 15.0;
	constexpr double COMMITTED_CHORD_RESELECT_GUARD_SECONDS = 2.0;

	// Rocksmith grades a bend by whether the player reaches the target pitch, so the
	// bent pitch is the correct thing to accept. Requiring the unbent fundamental is
	// wrong, and at a degraded tick rate it is close to unpassable: the unbent pitch
	// exists for only a few tens of milliseconds before the bend takes it away.
	constexpr int MAX_BEND_SEMITONES = 3;

	constexpr uintptr_t RECORD_MASK = 0x00;
	constexpr uintptr_t RECORD_FLAGS = 0x04;
	constexpr uintptr_t RECORD_HASH = 0x08;
	constexpr uintptr_t RECORD_TIME = 0x0C;
	constexpr uintptr_t RECORD_STRING = 0x10;
	constexpr uintptr_t RECORD_FRET = 0x11;
	constexpr uintptr_t RECORD_CHORD_ID = 0x14;
	constexpr uintptr_t RECORD_BEND_AMOUNT = 0x40;
	// Only one bend size has been observed, so the value is trusted within a sane musical
	// range and the old range check is used outside it. An unexpected chart therefore
	// degrades to the previous behaviour rather than making bends unplayable.
	constexpr float BEND_AMOUNT_MIN_SEMITONES = 0.5f;
	constexpr float BEND_AMOUNT_MAX_SEMITONES = 4.0f;
	constexpr uintptr_t RECORD_CHORD_NOTES_ID = 0x18;
	constexpr uintptr_t RECORD_PHRASE_ITERATION = 0x20;

	constexpr uint32_t NOTE_MASK_IGNORE = 0x00040000;
	constexpr uint32_t NOTE_MASK_CHILD = 0x10000000;

	// PlayerSong offsets.
	constexpr uintptr_t PLAYER_SONG_MODE = 0x18;
	constexpr uintptr_t PLAYER_SONG_PENDING_STATE = 0x14;
	constexpr uintptr_t PLAYER_SONG_PENDING_FLAG = 0x1C;
	constexpr uintptr_t PLAYER_SONG_RUNNING = 0xD9;
	constexpr uintptr_t PLAYER_SONG_STOPPED = 0xDA;
	constexpr uintptr_t PLAYER_SONG_CLOCK = 0x174;

	constexpr float ROLLBACK_THRESHOLD = 0.2f;
	constexpr float GREY_EPSILON = 0.001f;
	constexpr float BOUNDARY_EPSILON = 0.001f;
	constexpr float HELD_TIME_EPSILON = 0.0005f;
	constexpr float HOLD_BOUNDARY_MAX_OVERSHOOT = 0.25f;
	constexpr float PLAYER_SONG_RESTART_SECONDS = 0.27f;
	constexpr float CHORD_DENSE_WINDOW_SECONDS = 0.60f;
	constexpr uint32_t INPUT_RELEASE_CONFIRMATION_TICKS = 3;
	// A bend target whose bent pitch is distinguishable from the previous note may sit dead
	// steady on target during the input-release wait (a fast pick-and-bend arrives already
	// bent, with no rise left to observe); accept it after this many on-target polls even
	// without an observed rise. Small, because the same-pitch bar has already excluded the
	// previous note's ring (#68).
	constexpr uint32_t BEND_WAIT_ON_TARGET_TICKS = 3;
	constexpr uint32_t DENSE_REBUILD_TIMEOUT_TICKS = 600;
	constexpr float BEND_DIAGNOSTIC_START_TIME = 14.55f;
	constexpr float BEND_DIAGNOSTIC_END_TIME = 14.75f;

	using ScoringUpdateFn = void(__stdcall*)(void* owner, float updateTime);
	using HitDecisionFn = bool(__fastcall*)(void* owner, void* unusedEdx, void* note);
	using ThiscallVoidFn = void(__fastcall*)(void* self, void* unusedEdx);
	using ThiscallFloatFn = void(__fastcall*)(void* self, void* unusedEdx, float value);
	using MarkNotesFn = void(__fastcall*)(void* unusedEcx, void* owner);

	ScoringUpdateFn originalScoringUpdate = nullptr;
	HitDecisionFn originalHitDecision = nullptr;

	enum class GatePhase
	{
		Idle,
		Armed,
		WaitingForInputRelease,
		Holding,
		CommitBeforeRelease,
		DenseRebuildPending,
		DenseRecommitAfterRebuild,
		DensePlayerSongStartPending,
		PostRelease,
		RecommitAfterRelease
	};

	const char* DescribeGatePhase(GatePhase phase)
	{
		switch (phase)
		{
		case GatePhase::Idle: return "idle";
		case GatePhase::Armed: return "armed";
		case GatePhase::WaitingForInputRelease: return "waiting-for-input-release";
		case GatePhase::Holding: return "holding";
		case GatePhase::CommitBeforeRelease: return "commit-before-release";
		case GatePhase::DenseRebuildPending: return "dense-rebuild-pending";
		case GatePhase::DenseRecommitAfterRebuild: return "dense-recommit-after-rebuild";
		case GatePhase::DensePlayerSongStartPending: return "dense-play-packet-pending";
		case GatePhase::PostRelease: return "post-release";
		case GatePhase::RecommitAfterRelease: return "recommit-after-release";
		}
		return "unknown";
	}

	enum class DenseChainResult
	{
		NotRequired,
		Advanced,
		Faulted
	};

	struct DenseSuccessor
	{
		uintptr_t record = 0;
		uintptr_t note = 0;
		float recordTime = 0.0f;
		float holdTime = 0.0f;
		int stringIndex = -1;
		int fret = -1;
		int chordId = -1;
		int chordNotesId = -1;
		int expectedMidi = -1;
		bool isBend = false;
		bool isBendChild = false;
		bool isLegato = false;
	};

	struct BendDecisionObservation
	{
		uintptr_t record = 0;
		uint32_t packedStates = 0;
		int loudestMidi = -2;
		bool originalResult = false;
		bool hasObservation = false;
	};

	std::recursive_mutex controllerMutex;
	bool isInitialized = false;

	void* trackedOwner = nullptr;
	float lastUpdateTime = 0.0f;
	bool hasLastUpdateTime = false;
	bool isEpochConfirmed = false;
	// Diagnostics: which path confirmed the current section (grid latch vs delayed-identity
	// fallback), surfaced in status so arming can be verified without racing the log.
	bool confirmedViaGrid = false;
	// Throttle for the periodic arming-state diagnostic (survives the log-buffer churn that
	// buries the one-time latch line).
	uint32_t armDiagTicks = 0;
	bool hasNativeSectionRangeFailureLogged = false;
	// The two endpoints of the first backward jump, held until a live record resolves one of
	// them to the section boundary by the authored+native identity condition.
	bool hasPendingBoundary = false;
	float pendingBoundaryBeforeRollback = 0.0f;
	float pendingBoundaryAfterRollback = 0.0f;
	float greyCutoff = 0.0f;
	// The upper selected-section boundary, latched once from +0x3C8 at confirmation.
	float sectionEndBoundary = 0.0f;
	bool hasSectionEndBoundary = false;

	struct TimelineSection { float start; float end; };
	std::vector<TimelineSection> timelineSections;
	void* timelineOwner = nullptr;
	uintptr_t timelineContainer = 0;
	uintptr_t timelineBegin = 0;
	uintptr_t timelineEnd = 0;
	// When the current hold began, for the safety release. Reset whenever the player makes
	// progress within a legato run, so a long but advancing run is never cut short.
	std::chrono::steady_clock::time_point holdProgressAnchor{};
	bool hasHoldProgressAnchor = false;
	uint64_t epochIndex = 0;

	// Selection and hold state.
	GatePhase gatePhase = GatePhase::Idle;
	uintptr_t selectedRecord = 0;
	float selectedRecordTime = 0.0f;
	int selectedString = -1;
	int selectedFret = -1;
	int selectedChordId = -1;
	int selectedChordNotesId = -1;
	float selectedHoldTime = 0.0f;
	double selectedCompensation = 0.0;
	int expectedMidi = -1;
	int researchChordTones[6] = {};
	int researchChordToneCount = 0;
	uintptr_t researchChordTonesRecord = 0;
	// The pitch of the note that was just committed. A carried onset necessarily has
	// this pitch, which is what makes it distinguishable from fresh playing.
	int previousExpectedMidi = -1;
	// The string of the note just committed. A hammer-on or pull-off can only be
	// sounded by a string that is already ringing, so legato handling only applies
	// when the successor is on this same string.
	int previousSelectedString = -1;
	// Whether the note just committed was a bend. A bend sweeps through a range of
	// pitches, so a carried onset from it does not necessarily carry that note's
	// nominal pitch and cannot be told apart by pitch alone.
	bool previousWasBend = false;
	// Set when the selected record carries NOTE_MASK_BEND, which widens acceptance
	// upward to the pitches the bend itself produces.
	bool isBendTarget = false;
	// The exact pitch a bend must reach, or -1 when the chart's bend amount was unreadable
	// or implausible, in which case acceptance falls back to the old tolerance range.
	int bendAcceptMidi = -1;
	bool isBendChildTarget = false;
	// Defined beside the acceptance test it feeds, but needed by hold establishment above.
	void ResolveBendAcceptance(uintptr_t record);
	// Forward decl: FaultWithoutRelease recovers by re-bootstrapping, but ResetBootstrap is
	// defined below it.
	void ResetBootstrap(const char* reason, void* liveOwner);
	// Defined beside the legato run it builds on, but needed by the hold tick above it.
	void BeginBendConfirmation();
	void StashPrimedOnset(int primedOnset);
	bool MatchesPickPitch(int sounding);
	// Defined beside the tracker read it explains, but needed by the hold tick above it.
	void LogMotionTrackers(const char* context, int wantedMidi);
	// Throttles the tracker report to once a second while a bend is being confirmed.
	std::chrono::steady_clock::time_point trackerSampleAnchor{};
	bool hasTrackerSampleAnchor = false;
	// The target's legato continuation, published so the renderer can keep it visible.
	uint32_t visualGroupCount = 0;
	uintptr_t visualGroupRecords[NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup] = {};
	// The same gesture as string/fret pairs, for the neck-diagram gate, which receives
	// coordinates rather than a note pointer.
	int32_t visualGroupStrings[NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup] = {};
	int32_t visualGroupFrets[NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup] = {};
	uintptr_t lastVisualGroupRecord = 0;
	// Set for hammer-on and pull-off records, which are accepted from the detector's
	// current pitch because they produce no pick attack for the onset query to latch.
	bool isLegatoTarget = false;
	uint32_t legatoConfirmTickCount = 0;
	// True once the current-pitch query has reported something other than the target's
	// pitch since this target became active. Legato acceptance requires it: the pitch
	// must arrive after the target exists, so a string still ringing at the right pitch
	// (a bend release sweeping down, a lingering previous note) cannot commit a note
	// the player has not articulated.
	bool hasLegatoPitchDeparted = false;
	constexpr uint32_t MAX_LEGATO_RUN = 8;
	int legatoRunMidi[MAX_LEGATO_RUN] = {};
	// Which elements of the run are a bend rather than a fretted legato note. The two need
	// different tests: a fretted note lands on a discrete fret and its pitch is exact, a
	// bend sweeps continuously and has to be judged against a band. Before this existed a
	// bend target that also began a legato run had its bend requirement dropped outright,
	// because the run branch consumed the pick and never looked at bendAcceptMidi.
	bool legatoRunIsBend[MAX_LEGATO_RUN] = {};
	uint32_t legatoRunCount = 0;
	uint32_t legatoRunIndex = 0;
	bool isConfirmingLegatoRun = false;

	using DetectionStrategy = NoteByNoteNativeScoring::DetectionStrategy;
	enum class DetectionTechnique { Single = 0, Chord = 1, Bend = 2 };
	volatile int g_chordDetectionStrategy = static_cast<int>(DetectionStrategy::Blend);
	volatile int g_noteDetectionStrategy = static_cast<int>(DetectionStrategy::Blend);
	volatile int g_bendDetectionStrategy = static_cast<int>(DetectionStrategy::Blend);

	// Blend only lets ML VETO a native accept when it is confidently seeing a genuinely
	// different note. This bar sits above the 0.5 accept bar so an unsure or borderline ML
	// read never blocks a correct native read; an octave-equivalent read never vetoes at all.
	constexpr float ML_BLEND_VETO_CONF = 0.70f;

	DetectionTechnique CurrentDetectionTechnique()
	{
		if (selectedChordId != -1) return DetectionTechnique::Chord;
		if (isBendTarget) return DetectionTechnique::Bend;
		return DetectionTechnique::Single;   // legato/tap ride the single-note native path
	}

	DetectionStrategy CurrentTechniqueStrategy()
	{
		switch (CurrentDetectionTechnique())
		{
			case DetectionTechnique::Chord: return static_cast<DetectionStrategy>(g_chordDetectionStrategy);
			case DetectionTechnique::Bend:  return static_cast<DetectionStrategy>(g_bendDetectionStrategy);
			default:                        return static_cast<DetectionStrategy>(g_noteDetectionStrategy);
		}
	}

	// ML may RESCUE (accept-only, fills a native/tier-0 miss) in Blend and MlOnly, never in
	// NativeOnly. Native still detects; the rescue only adds accepts, it never blocks.
	bool MlMayRescueNow() { return CurrentTechniqueStrategy() != DetectionStrategy::NativeOnly; }

	// Live native-vs-ML agreement sampler + per-session CSV. Defined further down, next to the
	// shadow loggers where the passive native/ML queries live; forward-declared here so the
	// hold-phase sampler can call it.
	void SampleDetectionComparison();

	static constexpr uint32_t DETECTION_COMPARE_HISTORY = 24;
	struct DetectionComparison
	{
		uint32_t agree = 0;
		uint32_t disagree = 0;
		int lastTechnique = -1;
		bool lastNativeMatch = false;
		bool lastMlMatch = false;
		bool lastMlHadOpinion = false;
		bool lastValid = false;
		uint32_t historyCount = 0;
		uint8_t history[DETECTION_COMPARE_HISTORY] = {};   // 0 red, 1 green, 2 amber
	};
	DetectionComparison g_detectionComparison;
	// -------------------------------------------------------------------------------------
	// True when the run being confirmed is a bend rather than fretted legato notes.
	//
	// The two need different comparisons. A hammer-on or pull-off lands on a discrete fret,
	// so its pitch is exact. A bend sweeps continuously and wobbles around the top, so
	// requiring the detector to report exactly the bent pitch on consecutive polls made
	// bends very hard to trigger. Reaching the target pitch is the musically correct test,
	// and overbending slightly still counts.
	bool isBendRunConfirmation = false;
	// Keeps the tracked-pitch line to one per bend. The confirmation runs every scoring
	// tick, so logging unconditionally would bury the surrounding hold diagnostics.
	bool wasBendTrackerPitchLogged = false;
	// Raw-detector bend acceptance state. Armed only once the detector has been seen
	// off-target since the confirmation began, so a clean leftover ring at the target
	// pitch (a same-pitch predecessor, which this chart's orange-14 bend has) cannot
	// complete a bend that was never bent.
	bool isRawBendAcceptArmed = false;
	int32_t rawBendAcceptStreak = 0;
	int32_t ndBendSightingStreak = 0;
	// The same off-target-first rule for the plain integer fallback
	// (currentMidi >= target): with the whole bend band now routed into confirmation
	// (ticket #52 second half), a carried in-band onset reaches this flow with the ring
	// still sounding AT the accept pitch, and an unarmed >= comparison would confirm the
	// bend on the first poll. The gesture must be observed approaching from below first.
	bool hasBendApproachBeenObserved = false;
	// Defined below, next to the visual grouping it mirrors, but needed by hold
	// establishment above it.
	void BuildLegatoRun(void* owner, float runTime, int runString);
	bool isHoldSuppressed = false;
	volatile bool areChordHoldsEnabled = true;
	volatile bool isFlowUntilMissEnabled = false;
	volatile float flowLateGraceSeconds = 0.15f;
	volatile bool verboseTrace = true;
	volatile bool isSafetyReleaseEnabled = false;
	// Seconds mark of the last stuck-hold warning, so the disabled path logs once
	// per budget interval instead of every tick. Reset when a hold (re)latches.
	double lastStuckWarnHeldSeconds = 0.0;
	uint64_t chordDecisionEvalCount = 0;
	std::chrono::steady_clock::time_point chordDecisionLogAnchor;
	bool hasChordDecisionLogAnchor = false;
	// Throttle for the observe-first native-matcher log (issue #58).
	std::chrono::steady_clock::time_point matcherObserveAnchor;
	bool hasMatcherObserveAnchor = false;
	uintptr_t matcherFreshnessRecord = 0;
	bool matcherFreshnessArmed = false;
	volatile bool isChordWindowSlideEnabled = true;
	uint64_t chordWindowSlideCount = 0;
	std::chrono::steady_clock::time_point chordWindowLogAnchor;
	bool hasChordWindowLogAnchor = false;
	volatile bool areRepeatStrumHoldsEnabled = true;
	// Double-strum guard state; see COMMITTED_CHORD_RESELECT_GUARD_SECONDS.
	uintptr_t lastCommittedChordRecord = 0;
	std::chrono::steady_clock::time_point lastCommittedChordAt{};
	bool sawSpikeDuringHold = false;
	// A stricter fresh-attack signal set ONLY by the level-spike gate (a genuine loud
	// attack), never by the analysis-ring onset flag. The onset-flag half of
	// sawSpikeDuringHold fires on a still-ringing chord's own onset stamps (issue #58: it
	// cascaded a whole repeat-strum section off one pick because the ring kept re-stamping
	// evidence). A real chord strum always spikes the level meter; a decaying ring never
	// does - so the matcher accept gates on THIS, immune to the ring. Reset every hold and
	// every dense successor with sawSpikeDuringHold.
	bool sawLevelSpikeDuringHold = false;
	// The analysis-ring timestamp captured when the current target latched. This is the
	// window ORIGIN for the native onset-window scan port - the value the native code takes
	// from the (now-frozen) detector clock. Frames newer than this are "since we latched
	// this note"; the onset+match must occur in that span. Reset per hold and per dense
	// successor so each note requires its own fresh attack.
	double holdLatchRingTime = 0.0;
	bool hasHoldLatchRingTime = false;
	// The selected single note's AUTHORED pitch in the detector's tuning frame (read from
	// its own template, exactly as the chord path reads view.notes), so the native
	// onset+harmonic scan can judge single notes the same way it judges chords - no
	// display-frame onset shift, transpose-correct in every tuning. -1 = unavailable
	// (dense successor / unreadable template) -> the single note falls back to the legacy
	// onset path. Bends/legato keep the legacy pitch-tracking path regardless.
	int selectedNativeTone = -1;
	// #52: when a bend last committed, so a follower can require attack energy for the release
	// decay window (see BEND_RELEASE_GUARD_SECONDS).
	std::chrono::steady_clock::time_point lastBendCommitAt{};
	bool hasLastBendCommit = false;
	int32_t onsetScanRingIndex = -1;
	uint32_t onsetScanFramesSinceLatch = 0;
	bool hasOnsetScanAnchor = false;
	NoteByNote::PickedAttackQueue pickedAttacks;
	NoteByNote::PickedAttack acceptedPick;
	uintptr_t acceptedPickRecord = 0;
	uint64_t pickDiagnosticSample = 0;
	uint64_t pickScanSample = 0;
	uint32_t pickSampleRate = 0;


	float heldEpoch = 0.0f;
	void* heldPlayerSong = nullptr;
	uint64_t holdTickCount = 0;
	// Rocksmith's scoring update is frame-coupled, so the tick rate is the frame rate and
	// worth reporting. It is reported as a measurement only: the claim that a rate in the
	// forties starves the detector was checked against the detector's own code and does not
	// hold, because the reported note is read fresh from the analysis ring on every call and
	// nothing about a pluck expires between polls. See
	// docs/investigations/note-by-note-input-gates.md.
	std::chrono::steady_clock::time_point holdTickRateAnchor;
	uint64_t holdTickRateAnchorTick = 0;
	// Detector sampling state. The ring write index is kept so a stalled hold can say
	// whether the engine's analysis ring is still advancing, which no single sample shows.
	std::chrono::steady_clock::time_point detectorSampleAnchor;
	bool hasDetectorSampleAnchor = false;
	int32_t lastSampledRingIndex = -1;
	bool hasLastSampledRingIndex = false;
	// Level-spike capture. A re-pick attack is a sharp upward level jump against a
	// ~1 dB/s decay, so comparing consecutive scoring ticks catches every pick even
	// when the once-a-second interval logger sleeps through it and the console
	// buffer floods. A spike opens a short burst of per-tick compact samples so the
	// attack transient's quality/pitch/dedupe evolution is on record.
	float lastTickDetectorLevel = 0.0f;
	bool hasLastTickDetectorLevel = false;
	int32_t spikeBurstTicksRemaining = 0;
	// Spike re-attack acceptance state (see REATTACK_WINDOW_TICKS). Deliberately its
	// own level tracker rather than sharing the logger's, so the logger stays a pure
	// observer.
	float reattackLastLevel = 0.0f;
	bool hasReattackLastLevel = false;
	int32_t reattackWindowTicks = 0;
	int32_t reattackStreak = 0;
	NoteByNoteDetection::MlConfirmationState mlConfirmationState;
	int32_t bendRescueStreak = 0;      // consecutive ticks tier-0/ML confirm the bend reached its target

	// Flow-work instrumentation (docs/designs/nbn-flow-until-miss.md phase 1): measure the
	// per-note cost that makes fast play choppy - the coordinated PlayerSong/fretboard rebuild
	// span (heavy + restarts the audio) and the wall-clock gap between consecutive commits.
	std::chrono::steady_clock::time_point denseRebuildQueuedAt{};
	bool hasDenseRebuildTiming = false;
	std::chrono::steady_clock::time_point lastCommitWallClock{};
	bool hasLastCommitWallClock = false;
	// Ticks since the last LEVEL SPIKE specifically (never set by the onset-opened
	// window). A decaying ring cannot spike, so recent spike energy is the one
	// signal that separates a fresh pick of a same-pitch successor from the
	// previous note still ringing at the identical pitch.
	int32_t spikeRecencyTicks = 0;
	// A played-ahead pick rescued from the arming's edge-drain (see
	// StashPrimedOnset); adopted by the first holding tick, single-shot.
	int pendingPrimedOnset = -1;
	// Departure-then-lock acceptance for picked targets, the same rule the legato
	// path uses (and which plays flawlessly): the gated current-pitch query must
	// first report a DIFFERENT pitch during the hold (a re-pick's attack transient
	// always provides one; a decaying ring only ever reads the expected pitch or
	// nothing, so it can never arm this), then the expected pitch across
	// consecutive polls commits. Tracked in the holding phase only.
	bool hasPickPitchDeparted = false;
	uint32_t pickPitchConfirmTicks = 0;
	uint64_t postReleaseTickCount = 0;
	uint64_t commitTickCount = 0;
	uint32_t inputReleaseTickCount = 0;
	// Bend-target input-release confirmation (#68): the lowest raw pitch seen during the wait
	// (a fresh bend climbs above it; a decaying ring does not) and how many consecutive polls
	// the sounding pitch has sat on the bent target. Both reset at every wait entry.
	int bendWaitPitchFloor = -1;
	uint32_t bendWaitOnTargetTicks = 0;
	bool wasCommitOverrideLogged = false;
	bool isReleaseRequested = false;
	// Latched by RequestReArm on every NBN enable, honored on the next scoring tick:
	// forces a clean ResetBootstrap so a section change that kept the same
	// GamePlaysongLAS owner still re-arms for the new section. See RequestReArm.
	bool reArmRequested = false;
	std::unordered_set<uintptr_t> consumedRecords;
	DenseSuccessor denseSuccessor;
	float observedScoringUpdateTime = 0.0f;
	std::array<BendDecisionObservation, 8> bendDecisionObservations = {};
	size_t bendDecisionObservationCount = 0;

	// Render-side observation counters (evidence only; never a control input).
	uint64_t renderFramesWhileHeld = 0;

	// The onset core 0x48DC40 takes the arrangement kind in EAX (0 guitar, 1 bass) and
	// passes the resolved detector through EDI/ECX internally; no C calling convention
	// matches, so the call needs explicit register setup. A C-style cast call left EAX
	// as garbage, made the mode resolver return null, and produced a permanent -1.
	// Input-onset shift, in semitones, to ADD to a native pitch reading. PERMANENTLY DISABLED
	// (kApplyInputOnsetShift = false): expectedMidi is now built in the DETECTOR'S tuning
	// frame (state+0x134C[string] + state+0x1358 + fret, the exact convention the native
	// matcher validates - see TryReadDetectorOpenMidi), which is the same frame the native
	// onset is detected in. A correctly fretted note therefore reads the same MIDI as
	// expectedMidi with NO shift, for both plain play and any transpose (Speaker Mode). The
	// earlier +1/-1 shift attempts existed only to reconcile expectedMidi's OLD authored-table
	// frame (0x1199D2C) against the detector frame, and both signs were wrong (accepted a
	// fret high or low); building expectedMidi in the detector frame removes the divergence at
	// the source. The shift hook is kept as a no-op in case a genuine ASIO input retuner
	// (headphone Drop Pedal that actually RETUNES the input, not Speaker Mode) is ever added;
	// that would need real Speaker-Mode-vs-ASIO detection, which the old host value lacked.
	constexpr bool kApplyInputOnsetShift = false;
	int ShiftNativePitchToDisplay(int nativeMidi)
	{
		if (!kApplyInputOnsetShift) return nativeMidi;
		return nativeMidi >= 0 ? nativeMidi + NoteByNoteRuntime::GetInputOnsetShiftSemitones()
							   : nativeMidi;
	}
	float ShiftNativePitchToDisplay(float nativePitch)
	{
		if (!kApplyInputOnsetShift) return nativePitch;
		return nativePitch >= 0.0f
			? nativePitch + static_cast<float>(NoteByNoteRuntime::GetInputOnsetShiftSemitones())
			: nativePitch;
	}

	int QueryNativeOnsetNote()
	{
		int result = -1;
		__asm
		{
			xor eax, eax        // arrangement kind 0 (guitar)
			mov edx, 0x48DC40   // ONSET_NOTE_QUERY
			call edx
			mov result, eax
		}
		// Normalise the onset into expectedMidi's display frame (see ShiftNativePitchToDisplay).
		const int shifted = ShiftNativePitchToDisplay(result);
		if (shifted != result)
		{
			static uint32_t onsetShiftLogThrottle = 0;
			if ((onsetShiftLogThrottle++ % 120) == 0)
			{
				LOG_INFO("(NBN TRANSPOSE) Native onset " << result << " shifted -> " << shifted
					<< " to match the display-tuning expected pitch (DropPedal input shift active)."
					<< std::endl);
			}
		}
		return shifted;
	}

	int QueryNativeLoudestPlayedNote()
	{
		int result = -1;
		__asm
		{
			xor eax, eax        // arrangement kind 0 (guitar)
			mov edx, 0x48E5F0   // LOUDEST_PLAYED_NOTE_QUERY
			call edx
			mov result, eax
		}
		return result;
	}

	volatile bool isFreezePromptSoundEnabled = false;

	void PlayFreezeNoteTrack()
	{
		if (!isFreezePromptSoundEnabled)
		{
			LOG_INFO("(NBN LAS PROMPT) " << FREEZE_NOTE_TRACK_EVENT
				<< " skipped (sound freeze disabled)." << std::endl);
			return;
		}
		const char* eventName = FREEZE_NOTE_TRACK_EVENT;
		const uintptr_t playSound = PLAY_SOUND_CSTRING;
		__asm
		{
			push esi
			mov esi, eventName
			mov eax, playSound
			call eax
			pop esi
		}
		LOG_INFO("(NBN LAS PROMPT) Dispatched Rocksmith's native "
			<< FREEZE_NOTE_TRACK_EVENT << " event before the hold." << std::endl);
	}

	template <typename T>
	bool TryRead(uintptr_t address, T& value)
	{
		if (address == 0) return false;
		MEMORY_BASIC_INFORMATION memory = {};
		if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) == 0) return false;
		if (memory.State != MEM_COMMIT
			|| (memory.Protect & PAGE_GUARD) != 0
			|| (memory.Protect & PAGE_NOACCESS) != 0)
		{
			return false;
		}
		const auto regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
		if (address > regionEnd || sizeof(T) > regionEnd - address) return false;
		value = *reinterpret_cast<const T*>(address);
		return true;
	}

	// The onset dedupe global is plain writable game data and, during Learn a Song, the
	// controller owns its state outright (nothing else calls the onset query; see the
	// ONSET_NOTE_QUERY notes). Writing -1 "un-consumes" an edge: the next query re-reports
	// whatever settled pitch is in the ring.
	bool TryWriteDedupeGlobal(int32_t value) noexcept
	{
		__try
		{
			*reinterpret_cast<volatile int32_t*>(ONSET_DEDUPE_GLOBAL) = value;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Same contract as TryWriteDedupeGlobal, generalized to an instance field. Used
	// only for the held chord's window deltas, written and restored around a single
	// original-decision call on the scoring thread, which is the thread evaluating
	// the note - nothing else reads the deltas in between.
	bool TryWriteGameFloat(uintptr_t address, float value) noexcept
	{
		__try
		{
			*reinterpret_cast<volatile float*>(address) = value;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool TryReadNativeSectionRange(void* owner, float& start, float& end)
	{
		const auto address = reinterpret_cast<uintptr_t>(owner);
		uintptr_t vtable = 0;
		float startMirror = 0.0f;
		float endMirror = 0.0f;
		if (!TryRead(address, vtable) || vtable != LAS_OWNER_VTABLE
			|| !TryRead(address + LAS_SECTION_START, start)
			|| !TryRead(address + LAS_SECTION_START_MIRROR, startMirror)
			|| !TryRead(address + LAS_SECTION_END, end)
			|| !TryRead(address + LAS_SECTION_END_MIRROR, endMirror))
		{
			return false;
		}

		return std::isfinite(start)
			&& std::isfinite(end)
			&& start >= 0.0f
			&& end > start
			&& std::fabs(start - startMirror) <= BOUNDARY_EPSILON
			&& std::fabs(end - endMirror) <= BOUNDARY_EPSILON;
	}

	// Enumerate Rocksmith's stable authored phrase-section grid for this owner into the
	// cached timeline, PER SONG LIFECYCLE. The cache is keyed on the grid identity (owner +
	// the +0x78 container and its vector begin/end), not the owner alone, so a new song that
	// repopulates the grid - even under the same owner pointer - invalidates it and rebuilds.
	// Every read is guarded and any failure/transitional read leaves the cache empty so the
	// caller retries rather than arming on garbage. See LAS_PHRASE_SECTION_CONTAINER.
	bool EnsureTimelineForOwner(void* owner)
	{
		const auto address = reinterpret_cast<uintptr_t>(owner);
		uintptr_t vtable = 0;
		if (!TryRead(address, vtable) || vtable != LAS_OWNER_VTABLE) return false;

		uintptr_t container = 0;
		if (!TryRead(address + LAS_PHRASE_SECTION_CONTAINER, container) || container == 0)
		{
			return false;
		}

		uintptr_t begin = 0;
		uintptr_t end = 0;
		if (!TryRead(container + PHRASE_SECTION_VECTOR_BEGIN, begin) || begin == 0
			|| !TryRead(container + PHRASE_SECTION_VECTOR_END, end) || end < begin)
		{
			return false;
		}

		// Reuse only when the whole grid identity is unchanged.
		if (timelineOwner == owner && timelineContainer == container
			&& timelineBegin == begin && timelineEnd == end && !timelineSections.empty())
		{
			return true;
		}

		timelineSections.clear();
		timelineOwner = nullptr;
		timelineContainer = 0;
		timelineBegin = 0;
		timelineEnd = 0;

		const uintptr_t span = end - begin;
		if (span == 0 || (span % PHRASE_SECTION_STRIDE) != 0) return false;
		const uint32_t count = static_cast<uint32_t>(span / PHRASE_SECTION_STRIDE);
		if (count == 0 || count > PHRASE_SECTION_MAX) return false;

		std::vector<TimelineSection> sections;
		sections.reserve(count);
		float previousEnd = -std::numeric_limits<float>::infinity();
		for (uint32_t index = 0; index < count; ++index)
		{
			const uintptr_t entry = begin + static_cast<uintptr_t>(index) * PHRASE_SECTION_STRIDE;
			float start = 0.0f;
			float sectionEnd = 0.0f;
			if (!TryRead(entry + PHRASE_SECTION_START, start)
				|| !TryRead(entry + PHRASE_SECTION_END, sectionEnd))
			{
				return false;
			}
			// The grid is authored, monotonic and half-open. A non-finite or out-of-order
			// read means we snapped the wrong object (or mid-alloc garbage); reject the
			// whole build rather than cache a bad row.
			if (!std::isfinite(start) || !std::isfinite(sectionEnd)
				|| sectionEnd <= start || start < previousEnd - BOUNDARY_EPSILON)
			{
				return false;
			}
			previousEnd = sectionEnd;
			sections.push_back({ start, sectionEnd });
		}

		timelineSections = std::move(sections);
		timelineOwner = owner;
		timelineContainer = container;
		timelineBegin = begin;
		timelineEnd = end;
		LOG_INFO("(NBN LAS TIMELINE) Cached " << timelineSections.size()
			<< " authored sections for owner 0x" << std::hex
			<< reinterpret_cast<uintptr_t>(owner) << std::dec << " ["
			<< std::fixed << std::setprecision(3) << timelineSections.front().start
			<< ".." << timelineSections.back().end << "s]; arming reads the stable grid,"
			<< " not the marching +0x3C0/+0x3C8." << std::endl);
		return true;
	}

	// Find the cached section index whose start (or end) matches a native boundary time.
	// Returns -1 when nothing is within BOUNDARY_EPSILON.
	int FindTimelineIndexByStart(float startTime)
	{
		for (size_t index = 0; index < timelineSections.size(); ++index)
		{
			if (std::fabs(timelineSections[index].start - startTime) <= BOUNDARY_EPSILON)
			{
				return static_cast<int>(index);
			}
		}
		return -1;
	}
	int FindTimelineIndexByEnd(float endTime)
	{
		for (size_t index = 0; index < timelineSections.size(); ++index)
		{
			if (std::fabs(timelineSections[index].end - endTime) <= BOUNDARY_EPSILON)
			{
				return static_cast<int>(index);
			}
		}
		return -1;
	}

	// Latch the loop bounds directly from the stable grid, immediately, with no rollback
	// wait. The native loop range is read once here to IDENTIFY which grid rows the loop
	// spans; the bounds themselves come from the grid, so a marched +0x3C0 or a churning
	// mirror cannot corrupt the latched values. +0x3C8 (end) stays pinned to the loop's
	// last section end; +0x3C0 (start) is clean until NBN owns its first hold (which has
	// not happened at first confirmation). Returns false on a transient read failure so
	// the caller simply retries next tick - never going inert, never hanging on "Waiting".
	bool TryLatchSectionFromGrid(void* owner, float startHint = -1.0f)
	{
		if (!EnsureTimelineForOwner(owner)) return false;
		const auto address = reinterpret_cast<uintptr_t>(owner);

		float loopEnd = 0.0f;
		float loopEndMirror = 0.0f;
		if (!TryRead(address + LAS_SECTION_END, loopEnd)
			|| !TryRead(address + LAS_SECTION_END_MIRROR, loopEndMirror)
			|| !std::isfinite(loopEnd)
			|| std::fabs(loopEnd - loopEndMirror) > BOUNDARY_EPSILON)
		{
			return false; // transient; retry next tick, never inert.
		}

		const int endIndex = FindTimelineIndexByEnd(loopEnd);
		if (endIndex < 0) return false; // loopEnd not yet a grid boundary; retry next tick.

		int startIndex = -1;
		float loopStart = 0.0f;
		float loopStartMirror = 0.0f;
		if (TryRead(address + LAS_SECTION_START, loopStart)
			&& TryRead(address + LAS_SECTION_START_MIRROR, loopStartMirror)
			&& std::isfinite(loopStart)
			&& std::fabs(loopStart - loopStartMirror) <= BOUNDARY_EPSILON)
		{
			const int candidate = FindTimelineIndexByStart(loopStart);
			if (candidate >= 0 && candidate <= endIndex) startIndex = candidate;
		}
		if (startIndex < 0 && startHint >= 0.0f)
		{
			const int hinted = FindTimelineIndexByStart(startHint);
			if (hinted >= 0 && hinted <= endIndex) startIndex = hinted;
		}
		if (startIndex < 0) startIndex = endIndex;

		const float gridStart = timelineSections[static_cast<size_t>(startIndex)].start;
		const float gridEnd = timelineSections[static_cast<size_t>(endIndex)].end;
		if (!(gridEnd > gridStart)) return false;

		greyCutoff = gridStart;
		sectionEndBoundary = gridEnd;
		hasSectionEndBoundary = true;
		isEpochConfirmed = true;
		confirmedViaGrid = true;
		epochIndex = 1;
		consumedRecords.clear();
		LOG_INFO("(NBN LAS BOOTSTRAP) Section latched from the authored grid: rows "
			<< startIndex << ".." << endIndex << " -> [" << std::fixed << std::setprecision(6)
			<< gridStart << ".." << gridEnd << "]; greyCutoff=" << gridStart
			<< " end=" << gridEnd << "; epoch 1 begins immediately (no rollback wait)."
			<< " Native loop read was [" << loopStart << ".." << loopEnd << "]." << std::endl);
		HeapCheckpoint("bootstrap-confirmed-grid");
		return true;
	}

	bool HasSelectionMovedToNewSection(void* owner)
	{
		if (!isEpochConfirmed || timelineSections.empty()) return false;
		const auto address = reinterpret_cast<uintptr_t>(owner);
		float currentEnd = 0.0f;
		float currentEndMirror = 0.0f;
		if (!TryRead(address + LAS_SECTION_END, currentEnd)
			|| !TryRead(address + LAS_SECTION_END_MIRROR, currentEndMirror)
			|| !std::isfinite(currentEnd)
			|| std::fabs(currentEnd - currentEndMirror) > BOUNDARY_EPSILON)
		{
			return false; // transient read; do not churn the latch.
		}
		if (std::fabs(currentEnd - sectionEndBoundary) <= BOUNDARY_EPSILON) return false;
		// Only react when the new end actually snaps to a grid section, so a momentary
		// garbage read can never trigger a spurious re-latch.
		return FindTimelineIndexByEnd(currentEnd) >= 0;
	}

	// The sounding fractional pitch nearest to a wanted pitch, from Rocksmith's own
	// continuous trackers.
	//
	// Every read is guarded, and any failure anywhere in the chain reports false so the
	// caller falls back to the integer query rather than mis-scoring the hold. Nothing is
	// registered or released: this reads records the game is already maintaining.
	//
	// A note on why it picks the nearest record rather than a particular tracker id.
	// What registers these trackers during ordinary play, and against what target, is not
	// yet established (see the investigation's remaining unknowns). Selecting by proximity
	// to the pitch we are already waiting for avoids depending on that answer: the record
	// is used purely as a measurement of what is sounding, and the decision about which
	// pitch matters stays here. If the registration semantics later turn out to bind a
	// tracker to the selected note, this can become a direct lookup and get stricter.
	bool TryGetSoundingPitchNear(float wantedMidi, float withinSemitones, float& pitchOut)
	{
		uintptr_t singleton = 0;
		if (!TryRead(MOTION_NOTE_SINGLETON, singleton) || singleton == 0) return false;

		uintptr_t step = 0;
		if (!TryRead(singleton + MOTION_NOTE_SINGLETON_STEP, step) || step == 0) return false;

		uintptr_t container = 0;
		if (!TryRead(step + MOTION_NOTE_CONTAINER_STEP, container) || container == 0) return false;

		uintptr_t records = 0;
		uint32_t count = 0;
		if (!TryRead(container + MOTION_NOTE_ARRAY_POINTER, records) || records == 0) return false;
		if (!TryRead(container + MOTION_NOTE_ARRAY_COUNT, count)) return false;
		if (count == 0 || count > MOTION_NOTE_MAX_RECORDS) return false;

		bool found = false;
		float bestPitch = 0.0f;
		float bestDistance = 0.0f;
		for (uint32_t index = 0; index < count; ++index)
		{
			const uintptr_t record = records + (index * MOTION_NOTE_RECORD_STRIDE);

			// Both guards are the native accessors' own preconditions: _GetLocation
			// refuses a record whose +0x3C is not 1, and _IsLocated's return value at
			// +0x3D is what says the string is still sounding rather than merely
			// registered.
			uint8_t active = 0;
			uint8_t located = 0;
			if (!TryRead(record + MOTION_NOTE_RECORD_ACTIVE, active) || active != 1) continue;
			if (!TryRead(record + MOTION_NOTE_RECORD_LOCATED, located) || located == 0) continue;

			float pitch = 0.0f;
			if (!TryRead(record + MOTION_NOTE_RECORD_PITCH, pitch)) continue;
			// -1.0f is the sentinel for "not located"; the flag above should already have
			// excluded it, but the two are written independently so both are checked.
			if (!std::isfinite(pitch) || pitch < 0.0f) continue;

			const float distance = std::fabs(pitch - wantedMidi);
			if (distance > withinSemitones) continue;
			if (!found || distance < bestDistance)
			{
				found = true;
				bestPitch = pitch;
				bestDistance = distance;
			}
		}

		if (found) pitchOut = bestPitch;
		return found;
	}

	void LogMotionTrackers(const char* context, int wantedMidi)
	{
		uintptr_t singleton = 0;
		uintptr_t step = 0;
		uintptr_t container = 0;
		uintptr_t records = 0;
		uint32_t count = 0;

		if (!TryRead(MOTION_NOTE_SINGLETON, singleton) || singleton == 0
			|| !TryRead(singleton + MOTION_NOTE_SINGLETON_STEP, step) || step == 0
			|| !TryRead(step + MOTION_NOTE_CONTAINER_STEP, container) || container == 0
			|| !TryRead(container + MOTION_NOTE_ARRAY_POINTER, records) || records == 0
			|| !TryRead(container + MOTION_NOTE_ARRAY_COUNT, count))
		{
			LOG_INFO("(NBN TRACKER) verdict=chain-unreadable context=" << context
				<< " wanted=" << wantedMidi
				<< " singleton=0x" << std::hex << singleton
				<< " arrangement=0x" << step
				<< " container=0x" << container
				<< " records=0x" << records << std::dec
				<< ". The tracker chain did not resolve, so every bend falls back to the"
				<< " integer query." << std::endl);
			return;
		}

		if (count == 0 || count > MOTION_NOTE_MAX_RECORDS)
		{
			LOG_INFO("(NBN TRACKER) verdict=bad-count context=" << context
				<< " wanted=" << wantedMidi << " count=" << count << "." << std::endl);
			return;
		}

		std::ostringstream detail;
		uint32_t activeCount = 0;
		uint32_t locatedCount = 0;
		uint32_t inBandCount = 0;
		for (uint32_t index = 0; index < count; ++index)
		{
			const uintptr_t record = records + (index * MOTION_NOTE_RECORD_STRIDE);
			uint8_t active = 0;
			uint8_t located = 0;
			float pitch = 0.0f;
			const bool readActive = TryRead(record + MOTION_NOTE_RECORD_ACTIVE, active);
			const bool readLocated = TryRead(record + MOTION_NOTE_RECORD_LOCATED, located);
			const bool readPitch = TryRead(record + MOTION_NOTE_RECORD_PITCH, pitch);
			if (readActive && active == 1) ++activeCount;
			if (readLocated && located != 0) ++locatedCount;
			if (readActive && active == 1 && readLocated && located != 0 && readPitch
				&& std::isfinite(pitch) && pitch >= 0.0f
				&& std::fabs(pitch - static_cast<float>(wantedMidi))
					<= BEND_UNDERBEND_SEMITONES)
			{
				++inBandCount;
			}
			// Every record, flags and pitch, active or not. The frozen-transport
			// question this must answer: does the pitch field keep updating while
			// registration (the flags) is dead? If yes, the bend can be read from
			// here directly and the fallback stops being blind.
			detail << ' ' << index << '=' << static_cast<int>(active)
				<< '/' << static_cast<int>(located)
				<< '/' << std::fixed << std::setprecision(1) << (readPitch ? pitch : -99.0f);
		}

		const char* verdict = "in-band";
		if (activeCount == 0) verdict = "none-active";
		else if (locatedCount == 0) verdict = "none-located";
		else if (inBandCount == 0) verdict = "located-out-of-band";

		LOG_INFO("(NBN TRACKER) verdict=" << verdict << " context=" << context
			<< " wanted=" << wantedMidi
			<< " count=" << count << " active=" << activeCount
			<< " located=" << locatedCount << " inBand=" << inBandCount
			<< " |" << detail.str()
			<< " container=0x" << std::hex << container << std::dec
			<< "." << std::endl);
	}

	// Every condition the two native input queries test, read directly rather than inferred
	// from their -1 return.
	struct DetectorGateSample
	{
		uintptr_t detector = 0;
		float level = 0.0f;
		float quality = 0.0f;
		double levelMinimum = DETECTOR_GATE_LEVEL_FALLBACK;
		double qualityMinimum = DETECTOR_GATE_QUALITY_FALLBACK;
		bool passesLevel = false;
		bool passesQuality = false;
		int32_t currentNote = -1;
		int32_t ringIndex = -1;
		int32_t ringCapacity = 0;
		int32_t ringSequence = 0;
		int32_t pitchMode = 0;
		int32_t pitchNow = -1;
		int32_t pitchPrevious = -1;
		int32_t dedupe = -1;
		bool hasRing = false;
	};

	DetectorGateSample lastStateGateSample = {};
	bool hasLastStateGateSample = false;
	std::chrono::steady_clock::time_point lastStateGateSampleAt{};

	constexpr float NBN_INPUT_PRESENT_FLOOR_DB = -60.0f;

	// True when the guitar is currently producing input, read from the same per-tick
	// level cache that drives the top-left input display (fresh within 2s, above the
	// floor). Cheap; call it to gate spammy trace logs so evidence survives a pause.
	bool NbnInputPresent()
	{
		if (!hasLastStateGateSample || !std::isfinite(lastStateGateSample.level)) return false;
		if (std::chrono::duration<double>(
				std::chrono::steady_clock::now() - lastStateGateSampleAt).count() >= 2.0)
		{
			return false;
		}
		return lastStateGateSample.level >= NBN_INPUT_PRESENT_FLOOR_DB;
	}

	bool TryReadDetectorGates(DetectorGateSample& sample)
	{
		uintptr_t root = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return false;

		uintptr_t arrangement = 0;
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0) return false;

		uintptr_t engine = 0;
		if (!TryRead(arrangement + DETECTION_ENGINE, engine) || engine == 0) return false;

		uintptr_t detector = 0;
		if (!TryRead(engine + DETECTION_DETECTOR, detector) || detector == 0) return false;
		sample.detector = detector;

		// The two gates, in the order the accessor tests them.
		if (!TryRead(detector + DETECTOR_GATE_LEVEL, sample.level)) return false;
		if (!TryRead(detector + DETECTOR_GATE_QUALITY, sample.quality)) return false;
		if (!TryRead(DETECTOR_GATE_LEVEL_THRESHOLD, sample.levelMinimum)
			|| !std::isfinite(sample.levelMinimum))
		{
			sample.levelMinimum = DETECTOR_GATE_LEVEL_FALLBACK;
		}
		if (!TryRead(DETECTOR_GATE_QUALITY_THRESHOLD, sample.qualityMinimum)
			|| !std::isfinite(sample.qualityMinimum))
		{
			sample.qualityMinimum = DETECTOR_GATE_QUALITY_FALLBACK;
		}
		sample.passesLevel = std::isfinite(sample.level)
			&& static_cast<double>(sample.level) >= sample.levelMinimum;
		sample.passesQuality = std::isfinite(sample.quality)
			&& static_cast<double>(sample.quality) >= sample.qualityMinimum;

		TryRead(detector + DETECTOR_CURRENT_NOTE, sample.currentNote);
		TryRead(detector + DETECTOR_PITCH_MODE, sample.pitchMode);
		TryRead(detector + DETECTOR_RING_INDEX, sample.ringIndex);
		TryRead(detector + DETECTOR_RING_CAPACITY, sample.ringCapacity);
		TryRead(ONSET_DEDUPE_GLOBAL, sample.dedupe);

		uintptr_t ring = 0;
		if (TryRead(detector + DETECTOR_RING_BUFFER, ring) && ring != 0
			&& sample.ringIndex >= 0
			&& sample.ringCapacity > 0
			&& sample.ringCapacity <= DETECTOR_RING_MAX_CAPACITY
			&& sample.ringIndex < sample.ringCapacity)
		{
			// Which frame field carries the pitch is selected by the detector itself.
			const uintptr_t pitchField = sample.pitchMode == 2
				? RING_FRAME_PITCH_MODE_TWO
				: RING_FRAME_PITCH_DEFAULT;
			// The accessor's own wrap: one before index 0 is the last slot.
			const int32_t previousIndex = sample.ringIndex > 0
				? sample.ringIndex - 1
				: sample.ringCapacity - 1;
			const uintptr_t current = ring
				+ static_cast<uintptr_t>(sample.ringIndex) * DETECTOR_RING_STRIDE;
			const uintptr_t previous = ring
				+ static_cast<uintptr_t>(previousIndex) * DETECTOR_RING_STRIDE;
			sample.hasRing = TryRead(current + pitchField, sample.pitchNow)
				&& TryRead(previous + pitchField, sample.pitchPrevious)
				&& TryRead(current + RING_FRAME_SEQUENCE, sample.ringSequence);
		}
		return true;
	}

	// Native-drive step 4: the ND sounding streak. The accepter requires the
	// expected pitch to sit in the detector's current-sounding table above the
	// native threshold CONTINUOUSLY for the lesson duration; one lapse resets
	// the streak, so a decaying ring that dips under the bar cannot accept.
	std::chrono::steady_clock::time_point ndSoundingStreakStart{};
	bool hasNdSoundingStreak = false;
	float ndSoundingLastStrength = 0.0f;

	// Returns the current sounding strength of `pitch` in the native table, or
	// -1.0f when absent/below threshold/unreadable. Pure reads.
	float ReadNdSoundingStrength(int pitch)
	{
		uintptr_t root = 0, arrangement = 0, detector = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return -1.0f;
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0) return -1.0f;
		if (!TryRead(arrangement + DETECTION_DETECTOR, detector) || detector == 0) return -1.0f;
		int32_t count = 0;
		if (!TryRead(detector + DETECTOR_SOUNDING_COUNT, count)
			|| count <= 0 || count > ND_SOUNDING_MAX_ENTRIES)
		{
			return -1.0f;
		}
		float threshold = ND_STRENGTH_THRESHOLD_FALLBACK;
		if (!TryRead(ND_STRENGTH_THRESHOLD_GLOBAL, threshold) || !std::isfinite(threshold))
		{
			threshold = ND_STRENGTH_THRESHOLD_FALLBACK;
		}
		for (int32_t index = 0; index < count; ++index)
		{
			int32_t midi = -1;
			if (!TryRead(detector + DETECTOR_SOUNDING_TABLE + static_cast<uintptr_t>(index) * 8, midi)) break;
			if (midi != pitch) continue;
			float strength = 0.0f;
			if (!TryRead(detector + DETECTOR_SOUNDING_TABLE + static_cast<uintptr_t>(index) * 8 + 4, strength)) break;
			return (std::isfinite(strength) && strength > threshold) ? strength : -1.0f;
		}
		return -1.0f;
	}

	// Diagnostic sibling of ReadNdSoundingStrength (#58/open-A false accept): returns the
	// tone's RAW strength from the sounding table WITHOUT the accept threshold, so a weak
	// tone that is present but below the bar (e.g. an open string ringing sympathetically
	// when the player never actually picked it) shows its real number in the log instead
	// of just "no". Returns NaN when the tone is not in the table at all. Reads only.
	float ReadNdRawSoundingStrength(int pitch)
	{
		uintptr_t root = 0, arrangement = 0, detector = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0)
			return std::numeric_limits<float>::quiet_NaN();
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0)
			return std::numeric_limits<float>::quiet_NaN();
		if (!TryRead(arrangement + DETECTION_DETECTOR, detector) || detector == 0)
			return std::numeric_limits<float>::quiet_NaN();
		int32_t count = 0;
		if (!TryRead(detector + DETECTOR_SOUNDING_COUNT, count)
			|| count <= 0 || count > ND_SOUNDING_MAX_ENTRIES)
		{
			return std::numeric_limits<float>::quiet_NaN();
		}
		for (int32_t index = 0; index < count; ++index)
		{
			int32_t midi = -1;
			if (!TryRead(detector + DETECTOR_SOUNDING_TABLE + static_cast<uintptr_t>(index) * 8, midi)) break;
			if (midi != pitch) continue;
			float strength = 0.0f;
			if (!TryRead(detector + DETECTOR_SOUNDING_TABLE + static_cast<uintptr_t>(index) * 8 + 4, strength)) break;
			return strength;
		}
		return std::numeric_limits<float>::quiet_NaN();
	}

	double CurrentNdStreakSeconds()
	{
		if (!hasNdSoundingStreak) return 0.0;
		return std::chrono::duration<double>(
			std::chrono::steady_clock::now() - ndSoundingStreakStart).count();
	}

	constexpr double ND_CHORD_TONE_MEMORY_SECONDS = 0.30;
	int ndChordTones[6] = {};
	std::chrono::steady_clock::time_point ndChordToneLastSeen[6] = {};
	int ndChordToneCount = 0;
	uintptr_t ndChordForRecord = 0;
	bool hasNdChordGroup = false;
	std::chrono::steady_clock::time_point ndChordGroupSince{};

	// Called at chord hold latch with the note's template. Dense-relatched
	// chords never pass through here and simply keep the evaluator path (the
	// record guard in the tick refuses stale tone sets).
	void InitializeNdChordTones(uintptr_t noteAddress, uintptr_t record)
	{
		ndChordToneCount = 0;
		ndChordForRecord = 0;
		hasNdChordGroup = false;
		if (!isNdAcceptEnabled) return;
		uintptr_t templateAddress = 0;
		ChordTemplateView view = {};
		if (!TryRead(noteAddress + 0x30, templateAddress) || templateAddress == 0) return;
		if (!TryRead(templateAddress, view)) return;
		// The authored template tones are in the guitar frame; the sounding table they are looked
		// up in is in the detected frame, so add the input shift (0 for Speaker/Off, the Drop
		// amount for Drop Pedal) to bring them into it (see the single-note hold).
		const int ndInputShift = NoteByNoteRuntime::GetInputOnsetShiftSemitones();
		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			if (view.frets[stringIndex] >= 0x1A || view.notes[stringIndex] < 0) continue;
			ndChordTones[ndChordToneCount] = view.notes[stringIndex] + ndInputShift;
			ndChordToneLastSeen[ndChordToneCount] = {};
			++ndChordToneCount;
		}
		if (ndChordToneCount == 0) return;
		ndChordForRecord = record;
	}

	bool TickNdChordAcceptance(uintptr_t record)
	{
		if (!isNdAcceptEnabled || ndChordToneCount == 0 || record != ndChordForRecord) return false;
		if (!sawSpikeDuringHold)
		{
			hasNdChordGroup = false;
			return false;
		}
		const auto now = std::chrono::steady_clock::now();
		for (int index = 0; index < ndChordToneCount; ++index)
		{
			if (ReadNdSoundingStrength(ndChordTones[index]) >= 0.0f)
			{
				ndChordToneLastSeen[index] = now;
			}
		}
		bool groupRecent = true;
		for (int index = 0; index < ndChordToneCount; ++index)
		{
			if (ndChordToneLastSeen[index] == std::chrono::steady_clock::time_point{}
				|| std::chrono::duration<double>(now - ndChordToneLastSeen[index]).count()
					> ND_CHORD_TONE_MEMORY_SECONDS)
			{
				groupRecent = false;
				break;
			}
		}
		if (!groupRecent)
		{
			hasNdChordGroup = false;
			return false;
		}
		if (!hasNdChordGroup)
		{
			hasNdChordGroup = true;
			ndChordGroupSince = now;
			return false;
		}
		if (std::chrono::duration<double>(now - ndChordGroupSince).count()
			< ND_SOUNDING_HOLD_SECONDS)
		{
			return false;
		}
		LOG_INFO("(NBN ND) Native chord accept: all " << ndChordToneCount
			<< " playable tones seen above threshold within "
			<< ND_CHORD_TONE_MEMORY_SECONDS << "s, group held "
			<< std::fixed << std::setprecision(3)
			<< std::chrono::duration<double>(now - ndChordGroupSince).count()
			<< "s." << std::endl);
		hasNdChordGroup = false;
		return true;
	}

	// The step-4 accepter: expected pitch sounding above the native threshold
	// for the lesson duration. Returns the accepted pitch or -1.
	int TickNdSoundingAcceptance(int expectedPitch)
	{
		if (!isNdAcceptEnabled || expectedPitch < 0) return -1;
		// Same native ring-carryover rule as the chord path: duration credit
		// requires fresh-attack evidence since this latch (spike gate or the
		// ring's own onset stamp). A previous target's ring cannot stamp an
		// attack, so it can never earn credit here.
		if (!sawSpikeDuringHold)
		{
			hasNdSoundingStreak = false;
			return -1;
		}
		const float strength = ReadNdSoundingStrength(expectedPitch);
		if (strength < 0.0f)
		{
			hasNdSoundingStreak = false;
			return -1;
		}
		ndSoundingLastStrength = strength;
		if (!hasNdSoundingStreak)
		{
			hasNdSoundingStreak = true;
			ndSoundingStreakStart = std::chrono::steady_clock::now();
			return -1;
		}
		const double held = CurrentNdStreakSeconds();
		if (held < ND_SOUNDING_HOLD_SECONDS) return -1;
		LOG_INFO("(NBN ND) Native sounding-table accept: expected " << expectedPitch
			<< " above threshold for " << std::fixed << std::setprecision(3) << held
			<< "s (strength " << ndSoundingLastStrength << "). The lesson primitive"
			<< " accepted where the onset edge did not." << std::endl);
		hasNdSoundingStreak = false;
		return expectedPitch;
	}

	// What the chord matcher 0x4E7B30 actually evaluates: the CURRENT analysis
	// frame's level (frame+0x700, gated against the -55dB global) and its
	// per-pitch energy pairs (20 pairs at frame+0x1C, count at frame+0xBC).
	// Logged per refused evaluation so a "clean strum still refused" names its
	// own failure mode: level gate, empty pairs, or wrong pitches.
	constexpr uintptr_t RING_FRAME_PAIRS = 0x1C;
	constexpr uintptr_t RING_FRAME_PAIR_COUNT = 0xBC;
	constexpr uintptr_t RING_FRAME_LEVEL = 0x700;
	constexpr uintptr_t RING_FRAME_ONSET_FLAG = 0x7B5;

	bool TryDescribeCurrentAnalysisFrame(char* buffer, size_t bufferLength)
	{
		if (buffer == nullptr || bufferLength == 0) return false;
		buffer[0] = '\0';
		uintptr_t root = 0, arrangement = 0, engine = 0, detector = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return false;
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0) return false;
		if (!TryRead(arrangement + DETECTION_ENGINE, engine) || engine == 0) return false;
		if (!TryRead(engine + DETECTION_DETECTOR, detector) || detector == 0) return false;
		uintptr_t ring = 0;
		int32_t writeIndex = 0;
		int32_t capacity = 0;
		if (!TryRead(detector + DETECTOR_RING_BUFFER, ring) || ring == 0
			|| !TryRead(detector + DETECTOR_RING_INDEX, writeIndex)
			|| !TryRead(detector + DETECTOR_RING_CAPACITY, capacity)
			|| writeIndex < 0 || capacity <= 0 || writeIndex >= capacity
			|| capacity > DETECTOR_RING_MAX_CAPACITY)
		{
			return false;
		}
		const uintptr_t frame = ring
			+ static_cast<uintptr_t>(writeIndex) * DETECTOR_RING_STRIDE;
		float level = 0.0f;
		int32_t pairCount = 0;
		uint8_t onsetFlag = 0;
		TryRead(frame + RING_FRAME_LEVEL, level);
		TryRead(frame + RING_FRAME_PAIR_COUNT, pairCount);
		TryRead(frame + RING_FRAME_ONSET_FLAG, onsetFlag);
		size_t at = static_cast<size_t>(std::snprintf(buffer, bufferLength,
			"lvl=%.1f onset=%u pairs=%d", level, onsetFlag, pairCount));
		const int32_t reportCount = pairCount < 6 ? pairCount : 6;
		for (int32_t i = 0; i < reportCount && at < bufferLength; ++i)
		{
			// The pitch half of each pair is an int32 MIDI note, not a float
			// (session 8 logged them as denormals before this was understood).
			int32_t pitch = -1;
			float energy = 0.0f;
			TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8, pitch);
			TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8 + 4, energy);
			at += std::snprintf(buffer + at, bufferLength - at, " %d:%.2f",
				pitch, energy);
		}
		return true;
	}

	// Resolves the analysis ring for the onset-evidence scan. Same chain as
	// TryDescribeCurrentAnalysisFrame; returns false when any link is unreadable.
	bool TryResolveAnalysisRing(uintptr_t& ring, int32_t& writeIndex, int32_t& capacity)
	{
		uintptr_t root = 0, arrangement = 0, engine = 0, detector = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return false;
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0) return false;
		if (!TryRead(arrangement + DETECTION_ENGINE, engine) || engine == 0) return false;
		if (!TryRead(engine + DETECTION_DETECTOR, detector) || detector == 0) return false;
		return TryRead(detector + DETECTOR_RING_BUFFER, ring) && ring != 0
			&& TryRead(detector + DETECTOR_RING_INDEX, writeIndex)
			&& TryRead(detector + DETECTOR_RING_CAPACITY, capacity)
			&& writeIndex >= 0 && capacity > 0 && writeIndex < capacity
			&& capacity <= DETECTOR_RING_MAX_CAPACITY;
	}

	bool IsPlainPickedTarget()
	{
		return expectedMidi >= 0 && selectedChordId == -1 && !isBendTarget
			&& !isBendChildTarget && !isLegatoTarget && !isConfirmingLegatoRun;
	}

	void ResetChordOnsetEvidenceAnchor()
	{
		onsetScanFramesSinceLatch = 0;
		hasOnsetScanAnchor = false;
		onsetScanRingIndex = -1;
		uintptr_t ring = 0;
		int32_t writeIndex = 0;
		int32_t capacity = 0;
		if (!TryResolveAnalysisRing(ring, writeIndex, capacity)) return;
		onsetScanRingIndex = writeIndex;
		hasOnsetScanAnchor = true;
	}

	bool TryMeasureOnsetRise(uintptr_t ring, int32_t index, int32_t capacity,
		float level, float& rise)
	{
		rise = 0.0f;
		if (!std::isfinite(level) || capacity <= ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES) return false;
		const int32_t baseline = (index - ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES + capacity) % capacity;
		float priorLevel = 0.0f;
		if (!TryRead(ring + static_cast<uintptr_t>(baseline) * DETECTOR_RING_STRIDE
			+ RING_FRAME_LEVEL, priorLevel) || !std::isfinite(priorLevel)) return false;
		rise = level - priorLevel;
		return true;
	}

	bool ReadPickedAttackBaseline(NoteByNote::PickedAttack& attack, int midi, float& power)
	{
		if (midi < 0 || midi > 127 || attack.sampleRate == 0) return false;
		const int slot = midi == attack.candidateMidi ? 0 : 1;
		if (attack.baselineMidi[slot] == midi)
		{
			power = attack.baselinePower[slot];
			return true;
		}
		const double frequency = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
		const uint64_t count = static_cast<uint64_t>((frequency >= 330.0 ? 0.02f : 0.15f) * attack.sampleRate);
		if (attack.minimumSample <= count) return false;
		NoteByNoteProtocol::RawNoteConfirmation raw;
		if (!NoteByNoteRuntime::QueryRawNoteConfirmation(frequency, raw,
			attack.minimumSample - count, attack.minimumSample)) return false;
		attack.baselineMidi[slot] = midi;
		attack.baselinePower[slot] = raw.attackPower;
		attack.baselineMinusPower[slot] = raw.attackMinusPower;
		attack.baselinePlusPower[slot] = raw.attackPlusPower;
		power = raw.attackPower;
		return true;
	}

	void ConfirmLatestPickedAttack(uint64_t maximumSample = 0)
	{
		auto* attack = pickedAttacks.Latest();
		if (attack == nullptr || attack->confirmedMidi >= 0) return;
		const int candidates[] = { attack->candidateMidi, QueryNativeLoudestPlayedNote() };
		for (int index = 0; index < 2; ++index)
		{
			const int midi = candidates[index];
			if (midi < 0 || midi > 127 || (index != 0 && midi == candidates[0])) continue;
			NoteByNoteProtocol::RawNoteConfirmation raw;
			const double frequency = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
			if (!NoteByNoteRuntime::QueryRawNoteConfirmation(frequency, raw, attack->minimumSample, maximumSample)
				|| raw.confirmed == 0) continue;
			if (raw.attackChange < 0.2f) continue;
			float baseline = 0.0f;
			if (!ReadPickedAttackBaseline(*attack, midi, baseline)) continue;
			const int slot = midi == attack->candidateMidi ? 0 : 1;
			const float targetRise = std::sqrt(raw.attackPower) - std::sqrt(baseline);
			const float minusRise = std::sqrt(raw.attackMinusPower) - std::sqrt(attack->baselineMinusPower[slot]);
			const float plusRise = std::sqrt(raw.attackPlusPower) - std::sqrt(attack->baselinePlusPower[slot]);
			const float neighbourRise = minusRise > plusRise ? minusRise : plusRise;
			if (neighbourRise > 1e-3f && neighbourRise > (targetRise > 0.0f ? targetRise : 0.0f)) continue;
			const auto strategy = CurrentTechniqueStrategy();
			if (strategy != DetectionStrategy::NativeOnly)
			{
				NoteByNoteProtocol::MlNoteEvidence ml;
				const bool available = NoteByNoteRuntime::QueryMlNoteEvidence(
					midi, 0.5f, attack->minimumMlSample, ml);
				if (strategy == DetectionStrategy::MlOnly
					&& (!available || ml.verdict != NoteByNoteProtocol::MlNoteVerdict::Confirmed)) continue;
				if (available && ml.verdict == NoteByNoteProtocol::MlNoteVerdict::Conflicting
					&& ml.confidence >= ML_BLEND_VETO_CONF && ml.observedMidi >= 0
					&& (ml.observedMidi - midi) % 12 != 0) continue;
			}
			attack->confirmedMidi = midi;
			attack->confirmedSample = raw.endSampleIndex;
			LOG_INFO("(NBN PICK BUFFER) Confirmed attack=" << attack->time << " midi=" << midi
				<< " samples=" << attack->minimumSample << ".." << raw.endSampleIndex
				<< " change=" << raw.attackChange << " targetRise=" << targetRise << " neighbourRise=" << neighbourRise
				<< " queued=" << pickedAttacks.Count() << std::endl);
			return;
		}
	}

	void TickPickedAttackStream()
	{
		RawPitchVerifier::RawAttackBatch batch;
		if (!NoteByNoteRuntime::QueryRawAttacks(pickScanSample, batch)) return;
		const double now = static_cast<double>(batch.endSampleIndex) / batch.sampleRate;
		if (batch.count != 0 && (batch.endSampleIndex < pickDiagnosticSample
			|| batch.endSampleIndex - pickDiagnosticSample >= batch.sampleRate))
		{
			const auto& latest = batch.frames[batch.count - 1];
			LOG_INFO("(NBN RAW ATTACK) sample=" << latest.endSampleIndex
				<< " inputDb=" << latest.inputLevelDb
				<< " queued=" << pickedAttacks.Count() << std::endl);
			pickDiagnosticSample = batch.endSampleIndex;
		}
		if (batch.reset != 0 || pickScanSample == 0 || pickSampleRate != batch.sampleRate || batch.endSampleIndex < pickScanSample
			|| (expectedMidi >= 0 && !IsPlainPickedTarget()))
		{
			pickedAttacks.Clear();
			pickScanSample = batch.endSampleIndex;
			pickSampleRate = batch.sampleRate;
			return;
		}
		if (batch.count != 0 && batch.frames[0].endSampleIndex - pickScanSample > batch.sampleRate / 100)
		{
			LOG_INFO("(NBN PICK BUFFER) Raw attack scan overrun; clearing uncertain attacks." << std::endl);
			pickedAttacks.Clear();
			pickScanSample = batch.endSampleIndex;
			return;
		}
		const unsigned expired = pickedAttacks.Expire(now);
		if (expired != 0) LOG_INFO("(NBN PICK BUFFER) Expired " << expired << " old attacks." << std::endl);
		for (uint32_t index = 0; index < batch.count; ++index)
		{
			const auto& frame = batch.frames[index];
			pickScanSample = frame.endSampleIndex;
			const uint64_t attackSample = frame.attackSampleIndex;
			if (attackSample == 0) continue;
			bool changed = false;
			const int observedMidi = QueryNativeLoudestPlayedNote();
			const int candidates[] = { expectedMidi, observedMidi };
			for (int midi : candidates)
			{
				if (midi < 0 || midi > 127) continue;
				NoteByNoteProtocol::RawNoteConfirmation change;
				const double frequency = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
				NoteByNoteRuntime::QueryRawNoteConfirmation(frequency, change, attackSample,
					attackSample + batch.sampleRate / (frequency < 330.0 ? 20 : 50));
				if (change.attackChange >= 0.2f) { changed = true; break; }
			}
			if (!changed)
			{
				LOG_INFO("(NBN RAW ATTACK) Rejected sustain fluctuation sample=" << attackSample << std::endl);
				continue;
			}
			NoteByNote::PickedAttack attack;
			attack.minimumSample = attackSample;
			attack.sampleRate = batch.sampleRate;
			const double time = static_cast<double>(attackSample) / batch.sampleRate;
			attack.time = time;
			ConfirmLatestPickedAttack(attack.minimumSample);
			attack.minimumMlSample = NoteByNoteRuntime::GetMlAudioSampleIndex();
			attack.candidateMidi = expectedMidi;
			float baseline = 0.0f;
			ReadPickedAttackBaseline(attack, attack.candidateMidi, baseline);
			ReadPickedAttackBaseline(attack, QueryNativeLoudestPlayedNote(), baseline);
			const auto* pending = pickedAttacks.Latest();
			if (pending != nullptr && pending->confirmedMidi < 0)
			{
				LOG_INFO("(NBN PICK BUFFER) Unresolved attack=" << pending->time
					<< " ended by next attack=" << time << std::endl);
			}
			if (!pickedAttacks.Push(attack))
			{
				LOG_INFO("(NBN PICK BUFFER) Attack queue full or timestamp repeated; rejected attack=" << time << std::endl);
			}
			else LOG_INFO("(NBN PICK BUFFER) Captured attack=" << time << " phase=" << static_cast<int>(gatePhase)
				<< " sample=" << attack.minimumSample << std::endl);
		}
		ConfirmLatestPickedAttack();
	}

	bool TakeBufferedPick()
	{
		if (acceptedPickRecord == selectedRecord) return true;
		const unsigned before = pickedAttacks.Count();
		const bool taken = pickedAttacks.Take(expectedMidi, acceptedPick);
		const unsigned mismatched = before - pickedAttacks.Count() - (taken ? 1 : 0);
		if (mismatched != 0)
		{
			LOG_INFO("(NBN PICK BUFFER) Discarded " << mismatched << " attacks with a different pitch; expected="
				<< expectedMidi << std::endl);
		}
		if (!taken) return false;
		acceptedPickRecord = selectedRecord;
		LOG_INFO("(NBN PICK BUFFER) Consumed attack=" << acceptedPick.time
			<< " midi=" << acceptedPick.confirmedMidi << " record=" << selectedRecord << std::endl);
		return true;
	}

	// The onset-flag half of the attack evidence (the spike gate is the other
	// half; see sawSpikeDuringHold). Walks every ring frame written since the last
	// scan and latches evidence when a post-latch frame carries the ring's own onset
	// flag at a credible level. Also runs while a repeated picked note waits to arm.
	void TickOnsetEvidence()
	{
		uintptr_t ring = 0;
		int32_t writeIndex = 0;
		int32_t capacity = 0;
		if (!TryResolveAnalysisRing(ring, writeIndex, capacity)) return;
		if (!hasOnsetScanAnchor)
		{
			onsetScanRingIndex = writeIndex;
			hasOnsetScanAnchor = true;
			return;
		}
		int32_t advanced = writeIndex - onsetScanRingIndex;
		if (advanced < 0) advanced += capacity;
		if (advanced <= 0) return;
		if (advanced > ONSET_SCAN_MAX_FRAMES_PER_TICK)
		{
			onsetScanFramesSinceLatch += static_cast<uint32_t>(
				advanced - ONSET_SCAN_MAX_FRAMES_PER_TICK);
			onsetScanRingIndex += advanced - ONSET_SCAN_MAX_FRAMES_PER_TICK;
			if (onsetScanRingIndex >= capacity) onsetScanRingIndex -= capacity;
			advanced = ONSET_SCAN_MAX_FRAMES_PER_TICK;
		}
		bool loggedRingRejectThisTick = false;
		for (int32_t step = 1; step <= advanced; ++step)
		{
			int32_t index = onsetScanRingIndex + step;
			if (index >= capacity) index -= capacity;
			++onsetScanFramesSinceLatch;
			if (onsetScanFramesSinceLatch < ONSET_EVIDENCE_MIN_FRAMES) continue;
			const uintptr_t frame = ring
				+ static_cast<uintptr_t>(index) * DETECTOR_RING_STRIDE;
			uint8_t onsetFlag = 0;
			float level = 0.0f;
			if (!TryRead(frame + RING_FRAME_ONSET_FLAG, onsetFlag) || onsetFlag == 0) continue;

			if (!TryRead(frame + RING_FRAME_LEVEL, level) || !std::isfinite(level)
				|| level < ONSET_EVIDENCE_LEVEL_FLOOR_DB)
			{
				continue;
			}
			float rise = 0.0f;
			const bool haveBaseline = TryMeasureOnsetRise(ring, index, capacity, level, rise);
			if (!haveBaseline || rise < ONSET_EVIDENCE_RISE_DB)
			{
				// One line per tick so a loud ring's many onset frames cannot flood the
				// log; the measured rise is what calibrates ONSET_EVIDENCE_RISE_DB.
				if (!loggedRingRejectThisTick)
				{
					loggedRingRejectThisTick = true;
					float history[ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES] = {};
					for (int32_t offset = 1; offset <= ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES; ++offset)
					{
						const int32_t priorIndex = (index - offset + capacity) % capacity;
						if (!TryRead(ring + static_cast<uintptr_t>(priorIndex) * DETECTOR_RING_STRIDE
							+ RING_FRAME_LEVEL, history[offset - 1])) history[offset - 1] = std::nanf("");
					}
					LOG_INFO("(NBN LAS ONSET EVIDENCE) Rejected onset flag as a ring (no"
						<< " rising edge): level " << std::fixed << std::setprecision(1)
						<< level << " dB, rise " << std::showpos << rise << std::noshowpos
						<< " dB; baseline=" << (IsPlainPickedTarget() ? "local-valley" : "four-frames-back")
						<< " (need >= " << ONSET_EVIDENCE_RISE_DB << ") previousLevels=["
						<< history[0] << "," << history[1] << "," << history[2] << "," << history[3] << "], "
						<< onsetScanFramesSinceLatch << " frames after latch." << std::endl);
				}
				continue;
			}
			sawSpikeDuringHold = true;
			LOG_INFO("(NBN LAS ONSET EVIDENCE) Fresh attack frame accepted as attack"
				<< " evidence: onset flag set at level " << std::fixed
				<< std::setprecision(1) << level << " dB (rise " << std::showpos << rise
				<< std::noshowpos << " dB; baseline="
				<< (IsPlainPickedTarget() ? "local-valley" : "four-frames-back") << "), " << onsetScanFramesSinceLatch << " ring frames after the hold"
				<< " latched. The level-spike gate stayed silent (a re-strum over a loud"
				<< " ring moves the meter less than " << DETECTOR_SPIKE_JUMP_DB << " dB)."
				<< std::endl);
			break;
		}
		onsetScanRingIndex = writeIndex;
	}


	bool TryReadDetectorClock(double& clock)
	{
		uintptr_t root = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return false;
		uintptr_t arrangement = 0;
		if (!TryRead(root + DETECTION_ARRANGEMENT_GUITAR, arrangement) || arrangement == 0)
		{
			return false;
		}
		uintptr_t engine = 0;
		if (!TryRead(arrangement + DETECTION_ENGINE, engine) || engine == 0) return false;
		uintptr_t detector = 0;
		if (!TryRead(engine + DETECTION_DETECTOR, detector) || detector == 0) return false;
		return TryRead(detector + DETECTOR_ANALYSIS_CLOCK, clock);
	}

	// Which condition is refusing, named in the accessor's own order of tests.
	//
	// "none-visible" is deliberately not "none". Three further conditions live inside
	// 0x004E4B60 and 0x004DD420 (locating the onset frame, requiring it to be at least two
	// frames old, and comparing two extracted analysis frames) and none of them is
	// observable without calling into the game, so everything visible passing does not prove
	// the query would return a note.
	const char* DescribeDetectorRefusal(const DetectorGateSample& sample)
	{
		if (!sample.passesLevel && !sample.passesQuality) return "level+quality";
		if (!sample.passesLevel) return "level";
		if (!sample.passesQuality) return "quality";
		if (!sample.hasRing) return "ring-unreadable";
		if (sample.pitchNow <= -1) return "no-pitch";
		if (sample.pitchNow != sample.pitchPrevious) return "unsettled";
		if (sample.pitchNow == sample.dedupe) return "already-consumed";
		return "none-visible";
	}

	// Defined later in this TU; declared here so the hold-phase shadow sampler in
	// LogDetectorGates can reach them.
	void LogTier0ShadowEvidence(int expectedMidi, const char* context);
	void LogTier1ShadowPitch(int expectedMidi);
	// Tier-0 positive raw confirmation (defined with the rest of tier-0 below);
	// declared here so the reattack tick can consult it - see the flow-through fix.
	bool Tier0ConfirmsExpected(int expectedMidi);
	extern volatile bool g_tier0Enforcement;

	ULONGLONG g_lastAttackSpikeTick = 0;
	ULONGLONG g_flowAdoptTick = 0;
	ULONGLONG g_flowArmedTick = 0;

	// One line per second while a hold is unsatisfied, naming the refusing condition first
	// because read-process-console truncates at the console buffer width.
	//
	// `force` bypasses the interval so the moment of failure (a safety release) is always
	// captured, whatever the interval happened to be doing.
	void LogDetectorGates(const char* context, bool force)
	{
		const auto now = std::chrono::steady_clock::now();

		// The sample is read on EVERY call (the reads are a handful of SEH-guarded
		// peeks), because spike detection needs the per-tick level even when the
		// interval logger would stay silent.
		DetectorGateSample sample;
		const bool sampleReadable = TryReadDetectorGates(sample);
		if (sampleReadable)
		{
			lastStateGateSample = sample;
			hasLastStateGateSample = true;
			lastStateGateSampleAt = now;
		}

		{
			static long consumedStreakTicks = 0;
			static bool streakSawSpike = false;
			const bool stuckOnExpected = sampleReadable
				&& gatePhase == GatePhase::Holding
				&& sample.passesLevel
				&& sample.passesQuality
				&& sample.hasRing
				&& sample.pitchNow >= 0
				&& sample.pitchNow == sample.dedupe
				&& sample.pitchNow == expectedMidi;
			if (stuckOnExpected)
			{
				++consumedStreakTicks;
				// spikeBurstTicksRemaining is refreshed by the spike detection below;
				// reading last tick's burst here only shifts the evidence by one tick.
				if (spikeBurstTicksRemaining > 0) streakSawSpike = true;
			}
			else
			{
				consumedStreakTicks = 0;
				streakSawSpike = false;
			}
			if (consumedStreakTicks >= 1 && streakSawSpike)
			{
				consumedStreakTicks = 0;
				streakSawSpike = false;
				if (TryWriteDedupeGlobal(-1))
				{
					LOG_INFO("(NBN LAS DEDUPE) reset: expected " << expectedMidi
						<< " was consumed without a commit, repluck reports were"
						<< " suppressed, and attack energy during the stall says the"
						<< " player is replucking; the next onset query re-reports it."
						<< std::endl);
				}
			}
		}

		if (sampleReadable)
		{
			const bool spiked = hasLastTickDetectorLevel
				&& std::isfinite(sample.level)
				&& sample.level - lastTickDetectorLevel >= DETECTOR_SPIKE_JUMP_DB;
			if (spiked)
			{
				if (gatePhase == GatePhase::Holding && holdTickCount >= 8)
				{
					sawSpikeDuringHold = true;
					// The strict level-only flag the chord matcher accept gates on - set
					// here (a genuine level jump) but NOT in the onset-flag scan, so a
					// ring's onset stamps cannot fake it (issue #58 cascade).
					sawLevelSpikeDuringHold = true;
				}
				spikeBurstTicksRemaining = DETECTOR_SPIKE_BURST_TICKS;
				g_lastAttackSpikeTick = GetTickCount64();
				LOG_INFO("(NBN LAS SPIKE) jump=" << std::fixed << std::setprecision(1)
					<< (sample.level - lastTickDetectorLevel)
					<< " exp=" << expectedMidi
					<< " now=" << sample.pitchNow
					<< " prev=" << sample.pitchPrevious
					<< " dedupe=" << sample.dedupe
					<< " lvl=" << sample.level
					<< " q=" << std::setprecision(0) << sample.quality
					<< " loud=" << sample.currentNote
					<< std::endl);
			}
			else if (spikeBurstTicksRemaining > 0)
			{
				--spikeBurstTicksRemaining;
				LOG_INFO("(NBN LAS SPIKE+) exp=" << expectedMidi
					<< " now=" << sample.pitchNow
					<< " prev=" << sample.pitchPrevious
					<< " dedupe=" << sample.dedupe
					<< " lvl=" << std::fixed << std::setprecision(1) << sample.level
					<< " q=" << std::setprecision(0) << sample.quality
					<< " loud=" << sample.currentNote
					<< std::endl);
			}
			lastTickDetectorLevel = sample.level;
			hasLastTickDetectorLevel = std::isfinite(sample.level);
		}
		else
		{
			hasLastTickDetectorLevel = false;
		}

		if (gatePhase == GatePhase::Holding && !IsPlainPickedTarget() && !sawSpikeDuringHold)
		{
			TickOnsetEvidence();
		}

#if defined(_DEBUG)
		if (gatePhase == GatePhase::Holding && NbnInputPresent())
		{
			static ULONGLONG lastHoldShadowTick = 0;
			const ULONGLONG nowShadowTick = GetTickCount64();
			if (nowShadowTick - lastHoldShadowTick >= 300)
			{
				lastHoldShadowTick = nowShadowTick;
				LogTier0ShadowEvidence(expectedMidi, "HOLD");
				LogTier1ShadowPitch(expectedMidi);
			}
		}
#endif

		if (gatePhase == GatePhase::Holding && NbnInputPresent())
		{
			static ULONGLONG lastCompareTick = 0;
			const ULONGLONG nowCompareTick = GetTickCount64();
			if (nowCompareTick - lastCompareTick >= 300)
			{
				lastCompareTick = nowCompareTick;
				SampleDetectionComparison();
			}
		}

		if (!force)
		{
			if (hasDetectorSampleAnchor
				&& std::chrono::duration<double>(now - detectorSampleAnchor).count()
					< DETECTOR_SAMPLE_INTERVAL_SECONDS)
			{
				return;
			}
			// Verbose-gated (#log-noise, default OFF for FPS): the refusal line is per-tick
			// diagnostic spam. Off, it never prints. On, it is additionally input-gated so a
			// guitar set down mid-session (idle ~ -80 dB) does not flood "refusing=level" and
			// scroll real events out of the ~9000-row console buffer. The sample read above
			// still runs every call, so spike detection is unaffected; only this emit
			// withdraws. A forced call (an event site) always logs regardless.
			if (!verboseTrace || !NbnInputPresent()) return;
		}
		detectorSampleAnchor = now;
		hasDetectorSampleAnchor = true;

		if (!sampleReadable)
		{
			LOG_INFO("(NBN LAS DETECT) refusing=chain-unreadable context=" << context
				<< " expected=" << expectedMidi
				<< ". The detection engine could not be resolved from 0x0135F57C, so no"
				<< " statement can be made about the input gates." << std::endl);
			hasLastSampledRingIndex = false;
			return;
		}

		const char* ringMotion = "unknown";
		if (sample.hasRing)
		{
			if (!hasLastSampledRingIndex) ringMotion = "first-sample";
			else if (sample.ringIndex == lastSampledRingIndex) ringMotion = "STALLED";
			else ringMotion = "advancing";
		}
		lastSampledRingIndex = sample.ringIndex;
		hasLastSampledRingIndex = sample.hasRing;

		LOG_INFO("(NBN LAS DETECT) refusing=" << DescribeDetectorRefusal(sample)
			<< " ring=" << ringMotion
			<< " context=" << context
			<< " expected=" << expectedMidi
			<< " | level=" << std::fixed << std::setprecision(2) << sample.level
			<< " (needs >=" << sample.levelMinimum << ")"
			<< " quality=" << sample.quality
			<< " (needs >=" << sample.qualityMinimum << ")"
			<< " loudest=" << sample.currentNote
			<< " pitchNow=" << sample.pitchNow
			<< " pitchPrev=" << sample.pitchPrevious
			<< " dedupe=" << sample.dedupe
			<< " ringIndex=" << sample.ringIndex << "/" << sample.ringCapacity
			<< " seq=" << sample.ringSequence
			<< " pitchMode=" << sample.pitchMode
			<< " detector=0x" << std::hex << sample.detector << std::dec
			<< "." << std::endl);

		// Compact companion line. The full line above wraps at the console width, which
		// truncates exactly the fields the same-pitch re-attack investigation needs
		// (dedupe, pitchNow, pitchPrev). This one stays under ~90 characters so every
		// field survives; the refusal tag is repeated so the line stands alone.
		LOG_INFO("(NBN LAS DETECT2) ref=" << DescribeDetectorRefusal(sample)
			<< " exp=" << expectedMidi
			<< " now=" << sample.pitchNow
			<< " prev=" << sample.pitchPrevious
			<< " dedupe=" << sample.dedupe
			<< " lvl=" << std::fixed << std::setprecision(1) << sample.level
			<< " q=" << std::setprecision(0) << sample.quality
			<< " loud=" << sample.currentNote
			<< std::endl);
	}

	// The spike re-attack acceptance tick (see REATTACK_WINDOW_TICKS for the capture
	// evidence). Called once per tick in the holding and input-release-wait phases;
	// returns expectedMidi when the pick is accepted, -1 otherwise. Reads the
	// detector directly and never calls either native query, so it cannot consume
	// the edge or disturb the dedupe global. The window is opened by a level spike
	// here, or externally by an onset whose pitch was attack-transient garbage.
	//
	// allowAccept=false keeps the level/window/streak tracking ticking but never
	// accepts: the input-release wait uses it for a same-pitch successor, where the
	// previous note's own ring reads the expected pitch with passing gates and only
	// the normal release confirmation can tell the two apart.
	bool MlConfirmsHeldNote()
	{
		NoteByNoteProtocol::MlNoteEvidence evidence;
		if (expectedMidi == previousExpectedMidi && !sawSpikeDuringHold)
		{
			mlConfirmationState.Observe(evidence);
			return false;
		}
		NoteByNoteRuntime::QueryMlNoteEvidence(expectedMidi, ML_STUCK_RESCUE_CONF,
			mlConfirmationState.GetMinimumSampleIndex(), evidence);
		return mlConfirmationState.Observe(evidence);
	}
	int TickSpikeReattackAcceptance(bool allowAccept)
	{
		DetectorGateSample sample;
		if (!TryReadDetectorGates(sample))
		{
			hasReattackLastLevel = false;
			reattackWindowTicks = 0;
			reattackStreak = 0;
			return -1;
		}

		if (isMlStuckRescueEnabled && MlMayRescueNow() && allowAccept && MlConfirmsHeldNote())
		{
			reattackWindowTicks = 0;
			reattackStreak = 0;
			LOG_INFO("(NBN LAS ML RESCUE) Stuck hold accepted: two distinct post-target"
				<< " audio predictions confirm expected " << expectedMidi << "." << std::endl);
			return expectedMidi;
		}
		const bool spiked = hasReattackLastLevel
			&& std::isfinite(sample.level)
			&& sample.level - reattackLastLevel >= DETECTOR_SPIKE_JUMP_DB;
		reattackLastLevel = sample.level;
		hasReattackLastLevel = std::isfinite(sample.level);

		const bool spikeAttributable = gatePhase != GatePhase::Holding
			|| holdTickCount >= 6;
		if (spiked && spikeAttributable)
		{
			reattackWindowTicks = REATTACK_WINDOW_TICKS;
			reattackStreak = 0;
			spikeRecencyTicks = REATTACK_WINDOW_TICKS;
		}
		else
		{
			if (reattackWindowTicks > 0) --reattackWindowTicks;
			if (spikeRecencyTicks > 0) --spikeRecencyTicks;
		}
		if (reattackWindowTicks <= 0)
		{
			reattackStreak = 0;
			return -1;
		}

		const bool detectorMatches = sample.passesQuality
			&& MatchesPickPitch(sample.currentNote);
		const bool rawConfirms = !detectorMatches
			&& g_tier0Enforcement
			&& Tier0ConfirmsExpected(expectedMidi);
		if (!detectorMatches && !rawConfirms)
		{
			reattackStreak = 0;
			return -1;
		}
		// A raw confirmation needs a 2-tick streak, not 3: Tier0ConfirmsExpected is a
		// single strong measurement (>=1e-5 target, 5x over neighbours, sub-harmonic
		// guards) where the detector match is a wobbly per-tick read; the streak there
		// exists for that wobble. One tick shaved is one tick less lag per flowed note.
		const int requiredStreak = rawConfirms ? 2 : REATTACK_STREAK_TICKS;
		if (++reattackStreak < requiredStreak) return -1;
		if (!allowAccept) return -1;

		reattackWindowTicks = 0;
		reattackStreak = 0;
		LOG_INFO("(NBN LAS REATTACK) Pick accepted from the level spike: "
			<< (detectorMatches ? "raw pitch matched" : "tier-0 raw route confirmed")
			<< " expected " << expectedMidi
			<< " (detector read " << sample.currentNote
			<< ", level=" << std::fixed << std::setprecision(1) << sample.level
			<< ", quality=" << std::setprecision(0) << sample.quality
			<< ") for " << REATTACK_STREAK_TICKS << " ticks after a spike; the native"
			<< " onset edge never latched." << std::endl);
		return expectedMidi;
	}

	struct LiveNote
	{
		uintptr_t note = 0;
		uintptr_t record = 0;
		uint32_t mask = 0;
		float recordTime = 0.0f;
		float eventTime = 0.0f;
		float windowEntry = 0.0f;
		float windowExit = 0.0f;
		uint8_t stateC0 = 0;
		uint8_t stateC1 = 0;
		uint8_t stateC2 = 0;
		uint8_t stateC3 = 0;
	};

	bool IsBeatVfxPromptActive(uintptr_t fork)
	{
		uint8_t active = 0;
		uintptr_t entity = 0;
		uint8_t request = 0;
		return TryRead(fork + BEAT_VFX_ACTIVE, active)
			&& TryRead(fork + BEAT_VFX_ENTITY, entity)
			&& TryRead(fork + BEAT_VFX_PROMPT_REQUEST, request)
			&& (active != 0 || entity != 0 || request != 0);
	}

	bool ValidateBeatVfxFork(uintptr_t fork)
	{
		uintptr_t vtable = 0;
		return fork != 0 && TryRead(fork, vtable) && vtable == BEAT_VFX_VTABLE;
	}

	bool ArmSelectedNativePrompt(const LiveNote& selected)
	{
		uintptr_t wrapper = 0;
		uintptr_t controller = 0;
		uintptr_t controllerVtable = 0;
		if (!TryRead(selected.note + NOTE_VFX_WRAPPER, wrapper) || wrapper == 0
			|| !TryRead(wrapper, controller) || controller == 0
			|| !TryRead(controller, controllerVtable))
		{
			return false;
		}

		if (controllerVtable == NOTE_VFX_VTABLE)
		{
			uintptr_t armSlot = 0;
			uintptr_t clearSlot = 0;
			uintptr_t promptFork = 0;
			if (!TryRead(controllerVtable + 0x20, armSlot) || armSlot != ARM_NOTE_VFX_PROMPT
				|| !TryRead(controllerVtable + 0x24, clearSlot) || clearSlot != CLEAR_NOTE_VFX_PROMPT
				|| !TryRead(controller + NOTE_VFX_PROMPT_FORK, promptFork)
				|| !ValidateBeatVfxFork(promptFork))
			{
				return false;
			}
			if (IsBeatVfxPromptActive(promptFork)) return true;

			reinterpret_cast<ThiscallVoidFn>(armSlot)(reinterpret_cast<void*>(controller), nullptr);
			uint8_t request = 0;
			return TryRead(promptFork + BEAT_VFX_PROMPT_REQUEST, request) && request == 1;
		}

		if (controllerVtable != SPECIALIZED_NOTE_VFX_VTABLE) return false;

		uintptr_t armBcSlot = 0;
		uintptr_t armC0Slot = 0;
		uintptr_t clearSlot = 0;
		uintptr_t bcFork = 0;
		uintptr_t c0Fork = 0;
		uint8_t promptState = 0;
		uint8_t suppressPrompt = 0;
		if (!TryRead(controllerVtable + 0x14, armBcSlot) || armBcSlot != ARM_SPECIALIZED_PROMPT_BC
			|| !TryRead(controllerVtable + 0x18, armC0Slot) || armC0Slot != ARM_SPECIALIZED_PROMPT_C0
			|| !TryRead(controllerVtable + 0x1C, clearSlot) || clearSlot != CLEAR_SPECIALIZED_PROMPT
			|| !TryRead(controller + SPECIALIZED_PROMPT_BC_FORK, bcFork)
			|| !TryRead(controller + SPECIALIZED_PROMPT_C0_FORK, c0Fork)
			|| !ValidateBeatVfxFork(bcFork) || !ValidateBeatVfxFork(c0Fork)
			|| !TryRead(selected.note + NOTE_SPECIALIZED_PROMPT_STATE, promptState)
			|| !TryRead(selected.note + NOTE_SPECIALIZED_PROMPT_SUPPRESS, suppressPrompt))
		{
			return false;
		}

		const bool useBcFork = promptState != 0 && suppressPrompt == 0;
		const uintptr_t selectedFork = useBcFork ? bcFork : c0Fork;
		const uintptr_t otherFork = useBcFork ? c0Fork : bcFork;
		uint8_t otherRequest = 0;
		if (IsBeatVfxPromptActive(selectedFork)
			&& TryRead(otherFork + BEAT_VFX_PROMPT_REQUEST, otherRequest) && otherRequest == 0)
		{
			return true;
		}

		const uintptr_t armSlot = useBcFork ? armBcSlot : armC0Slot;
		reinterpret_cast<ThiscallVoidFn>(armSlot)(reinterpret_cast<void*>(controller), nullptr);
		uint8_t selectedRequest = 0;
		return TryRead(selectedFork + BEAT_VFX_PROMPT_REQUEST, selectedRequest) && selectedRequest == 1
			&& TryRead(otherFork + BEAT_VFX_PROMPT_REQUEST, otherRequest) && otherRequest == 0;
	}

	bool ReadLiveNote(uintptr_t noteAddress, LiveNote& note)
	{
		note = {};
		note.note = noteAddress;
		if (!TryRead(noteAddress + NOTE_RECORD, note.record) || note.record == 0) return false;
		if (!TryRead(noteAddress + NOTE_EVENT_TIME, note.eventTime)) return false;
		if (!TryRead(noteAddress + NOTE_WINDOW_ENTRY, note.windowEntry)) return false;
		if (!TryRead(noteAddress + NOTE_WINDOW_EXIT, note.windowExit)) return false;
		uint32_t packedStates = 0;
		if (!TryRead(noteAddress + NOTE_STATE_C0, packedStates)) return false;
		note.stateC0 = static_cast<uint8_t>(packedStates & 0xFF);
		note.stateC1 = static_cast<uint8_t>((packedStates >> 8) & 0xFF);
		note.stateC2 = static_cast<uint8_t>((packedStates >> 16) & 0xFF);
		note.stateC3 = static_cast<uint8_t>((packedStates >> 24) & 0xFF);
		if (!TryRead(note.record + RECORD_MASK, note.mask)) return false;
		if (!TryRead(note.record + RECORD_TIME, note.recordTime)) return false;
		return true;
	}

	void ObserveBendDecision(uintptr_t noteAddress, bool originalResult)
	{
		LiveNote note;
		if (!ReadLiveNote(noteAddress, note))
		{
			return;
		}
		float observedBendAmount = 0.0f;
		if (!TryRead(note.record + RECORD_BEND_AMOUNT, observedBendAmount)
			|| !std::isfinite(observedBendAmount)
			|| observedBendAmount < BEND_AMOUNT_MIN_SEMITONES)
		{
			return;
		}

		const uint32_t packedStates = static_cast<uint32_t>(note.stateC0)
			| (static_cast<uint32_t>(note.stateC1) << 8)
			| (static_cast<uint32_t>(note.stateC2) << 16)
			| (static_cast<uint32_t>(note.stateC3) << 24);
		const int loudestMidi = QueryNativeLoudestPlayedNote();
		auto observation = std::find_if(
			bendDecisionObservations.begin(),
			bendDecisionObservations.begin() + bendDecisionObservationCount,
			[&note](const BendDecisionObservation& candidate)
			{
				return candidate.record == note.record;
			});
		if (observation == bendDecisionObservations.begin() + bendDecisionObservationCount)
		{
			if (bendDecisionObservationCount == bendDecisionObservations.size()) return;
			observation = bendDecisionObservations.begin() + bendDecisionObservationCount;
			++bendDecisionObservationCount;
			observation->record = note.record;
		}
		if (observation->hasObservation
			&& observation->packedStates == packedStates
			&& observation->loudestMidi == loudestMidi
			&& observation->originalResult == originalResult)
		{
			return;
		}

		std::array<uint8_t, 0x48> recordBytes = {};
		const bool hasRecordBytes = TryRead(note.record, recordBytes);
		std::ostringstream rawRecord;
		if (hasRecordBytes)
		{
			rawRecord << std::hex << std::setfill('0');
			for (const auto value : recordBytes)
			{
				rawRecord << std::setw(2) << static_cast<unsigned int>(value);
			}
		}

		uint32_t flags = 0;
		TryRead(note.record + RECORD_FLAGS, flags);
		LOG_INFO("(NBN BEND DIAGNOSTIC) original-decision"
			<< " updateTime=" << std::fixed << std::setprecision(6) << observedScoringUpdateTime
			<< " note=0x" << std::hex << note.note
			<< " record=0x" << note.record
			<< " mask=0x" << note.mask
			<< " flags=0x" << flags
			<< " states=0x" << packedStates << std::dec
			<< " authored=" << std::fixed << std::setprecision(6) << note.recordTime
			<< " event=" << note.eventTime
			<< " window=" << note.windowEntry << ".." << note.windowExit
			<< " loudestMidi=" << loudestMidi
			<< " originalResult=" << std::boolalpha << originalResult
			<< " nbnEnabled=" << NoteByNoteRuntime::IsNoteByNoteEnabled()
			<< " recordBytes=" << (hasRecordBytes ? rawRecord.str() : "unavailable")
			<< "." << std::endl);

		observation->packedStates = packedStates;
		observation->loudestMidi = loudestMidi;
		observation->originalResult = originalResult;
		observation->hasObservation = true;
	}

	bool ForEachLiveNote(void* owner, const std::function<bool(const LiveNote&)>& visitor)
	{
		uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		uintptr_t begin = 0;
		uintptr_t end = 0;
		if (!TryRead(ownerAddress + OWNER_NOTES_BEGIN, begin)) return false;
		if (!TryRead(ownerAddress + OWNER_NOTES_END, end)) return false;
		if (begin == 0 || end < begin || end - begin > 0x4000) return false;
		for (uintptr_t slot = begin; slot < end; slot += sizeof(uintptr_t))
		{
			uintptr_t noteAddress = 0;
			if (!TryRead(slot, noteAddress) || noteAddress == 0) continue;
			LiveNote note;
			if (!ReadLiveNote(noteAddress, note)) continue;
			if (!visitor(note)) return true;
		}
		return true;
	}

	bool FindSelectedNote(void* owner, LiveNote& found)
	{
		bool wasFound = false;
		ForEachLiveNote(owner, [&](const LiveNote& note)
		{
			if (note.record != selectedRecord) return true;
			found = note;
			wasFound = true;
			return false;
		});
		return wasFound;
	}

	void* ResolvePlayerSong(void* owner)
	{
		uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		uintptr_t collection = 0;
		if (!TryRead(ownerAddress + OWNER_COMPONENTS, collection) || collection == 0) return nullptr;
		uintptr_t begin = 0;
		uintptr_t end = 0;
		if (!TryRead(collection + 0x4, begin) || !TryRead(collection + 0x8, end)) return nullptr;
		if (begin == 0 || end < begin || end - begin > 0x800) return nullptr;
		void* resolved = nullptr;
		size_t matchCount = 0;
		for (uintptr_t entry = begin; entry + 8 <= end; entry += 8)
		{
			uintptr_t component = 0;
			if (!TryRead(entry + 0x4, component) || component == 0) continue;
			uintptr_t vtable = 0;
			if (!TryRead(component, vtable)) continue;
			if (vtable != PLAYER_SONG_VTABLE) continue;
			resolved = reinterpret_cast<void*>(component);
			++matchCount;
		}
		return matchCount == 1 ? resolved : nullptr;
	}

	void EmitCandidateEvent(void* owner, float updateTime, const LiveNote& note,
		NoteByNoteProtocol::ExpectedAttackEventKind kind)
	{
		NoteByNoteProtocol::ExpectedAttackEvent event;
		event.kind = kind;
		event.ownerAddress = reinterpret_cast<uintptr_t>(owner);
		event.epoch = epochIndex;
		event.updateTime = updateTime;
		event.isAfterUpdate = 1;
		event.isNotePresent = 1;
		event.note.nativeNoteAddress = note.note;
		event.note.recordAddress = note.record;
		event.note.noteMask = note.mask;
		event.note.authoredTime = note.recordTime;
		event.note.nativeEventTime = note.eventTime;
		event.note.rangeA4 = note.windowEntry;
		event.note.rangeA8 = note.windowExit;
		event.note.stateC0 = note.stateC0;
		event.note.stateC1 = note.stateC1;
		event.note.stateC2 = note.stateC2;
		event.note.stateC3 = note.stateC3;
		uint8_t stringIndex = 0;
		uint8_t fret = 0;
		uint32_t flags = 0;
		uint32_t hash = 0;
		int32_t chordId = -1;
		int32_t chordNotesId = -1;
		int32_t phraseIteration = -1;
		if (TryRead(note.record + RECORD_STRING, stringIndex)) event.note.stringIndex = stringIndex;
		if (TryRead(note.record + RECORD_FRET, fret)) event.note.fret = fret;
		if (TryRead(note.record + RECORD_FLAGS, flags)) event.note.noteFlags = flags;
		if (TryRead(note.record + RECORD_HASH, hash)) event.note.noteHash = hash;
		if (TryRead(note.record + RECORD_CHORD_ID, chordId)) event.note.chordId = chordId;
		if (TryRead(note.record + RECORD_CHORD_NOTES_ID, chordNotesId)) event.note.chordNotesId = chordNotesId;
		if (TryRead(note.record + RECORD_PHRASE_ITERATION, phraseIteration)) event.note.phraseIterationId = phraseIteration;
		// The bend amount rides every candidate event, read fresh from the record so it
		// is correct even for CandidateChanged events emitted before ResolveBendAcceptance
		// runs. The cue needs it because the frozen hold repaints the bend visual as a
		// plain note, leaving the player no way to see how far to bend.
		if ((event.note.noteMask & NOTE_MASK_BEND) != 0)
		{
			float semitones = 0.0f;
			if (TryRead(note.record + RECORD_BEND_AMOUNT, semitones)
				&& std::isfinite(semitones)
				&& semitones >= BEND_AMOUNT_MIN_SEMITONES
				&& semitones <= BEND_AMOUNT_MAX_SEMITONES)
			{
				event.note.bendSemitones = static_cast<int32_t>(std::lround(semitones));
			}
			else
			{
				event.note.bendSemitones = -1;
			}
		}
		NoteByNoteRuntime::PublishExpectedAttackEvent(event);
	}

	void ClearSelection()
	{
		acceptedPickRecord = 0;
		gatePhase = GatePhase::Idle;
		selectedRecord = 0;
		selectedRecordTime = 0.0f;
		selectedString = -1;
		selectedFret = -1;
		selectedChordId = -1;
		selectedChordNotesId = -1;
		selectedHoldTime = 0.0f;
		selectedCompensation = 0.0;
		expectedMidi = -1;
		previousExpectedMidi = -1;
		previousWasBend = false;
		visualGroupCount = 0;
		lastVisualGroupRecord = 0;
		isBendTarget = false;
		bendAcceptMidi = -1;
		isBendChildTarget = false;
		isLegatoTarget = false;
		legatoConfirmTickCount = 0;
		hasLegatoPitchDeparted = false;
		legatoRunCount = 0;
		for (auto& isBend : legatoRunIsBend) isBend = false;
		legatoRunIndex = 0;
		isConfirmingLegatoRun = false;
		isBendRunConfirmation = false;
		wasBendTrackerPitchLogged = false;
		isRawBendAcceptArmed = false;
		rawBendAcceptStreak = 0;
		ndBendSightingStreak = 0;
		// A spike window opened by the PREVIOUS note's own accepted pick must not
		// survive into this hold: a same-pitch successor would accept itself from
		// its predecessor's still-ringing attack. The level tracker itself persists
		// (a pick across the boundary is still a real pick).
		reattackWindowTicks = 0;
		reattackStreak = 0;
		spikeRecencyTicks = 0;
		pendingPrimedOnset = -1;
		hasPickPitchDeparted = false;
		pickPitchConfirmTicks = 0;
		isHoldSuppressed = false;
		holdTickCount = 0;
		postReleaseTickCount = 0;
		commitTickCount = 0;
		inputReleaseTickCount = 0;
		wasCommitOverrideLogged = false;
		heldPlayerSong = nullptr;
		renderFramesWhileHeld = 0;
		denseSuccessor = {};
	}

	bool OwnsNativeHold()
	{
		return gatePhase == GatePhase::WaitingForInputRelease
			|| gatePhase == GatePhase::Holding
			|| gatePhase == GatePhase::CommitBeforeRelease
			|| gatePhase == GatePhase::DenseRebuildPending
			|| gatePhase == GatePhase::DenseRecommitAfterRebuild
			|| gatePhase == GatePhase::DensePlayerSongStartPending;
	}

	constexpr uintptr_t OWNER_FROZEN_ON_TAG = 0x5E3;
	volatile bool isNativeFreezeFlagEnabled = false;

	constexpr uintptr_t NATIVE_SCHEDULER_GLOBAL = 0x0135F5AC;
	constexpr uintptr_t OWNER_FREEZE_TAG_STRING = 0x5C8;
	constexpr uintptr_t OWNER_FREEZE_TAG_BEGIN = 0x5D8;
	constexpr uintptr_t OWNER_FREEZE_TAG_END = 0x5DC;
	constexpr uintptr_t OWNER_RESUME_FROM_TAG = 0x5E4;
	constexpr uintptr_t OWNER_FREEZE_START_TIME = 0x618;

	constexpr uintptr_t CHORD_DISPLAY_GLOBAL = 0x0135F54C;
	constexpr uintptr_t CHORD_DISPLAY_GLOBAL_HOP = 0x10;
	constexpr uintptr_t CHORD_DISPLAY_WRAPPER_SLOT = 0x50;
	constexpr uintptr_t OWNER_CHORD_DISPLAY_COMPONENT = 0x78;
	constexpr uintptr_t CHORD_DISPLAY_TEMPLATE_BEGIN = 0x94;
	constexpr uintptr_t CHORD_DISPLAY_TEMPLATE_END = 0x98;
	constexpr uintptr_t CHORD_DISPLAY_SLOT = 0x1F4;
	constexpr uintptr_t CHORD_DISPLAY_VISIBLE = 0x1F0;
	volatile bool isNativeChordPanelEnabled = false;
	uintptr_t shownChordPanelComponent = 0;

	volatile bool isScheduleShiftEnabled = false;
	uint64_t scheduleShiftCount = 0;
	// When the transport was last stopped (Stop_TMusic latch at establish or a
	// dense relatch); the release-side shift compensates by now-minus-this.
	std::chrono::steady_clock::time_point transportFrozenAt{};
	bool hasTransportFrozenAt = false;
	constexpr uintptr_t NATIVE_SCHEDULE_SHIFT_FN = 0x57EB00;
	constexpr int NATIVE_SCHEDULE_SHIFT_ENTRY = 4;
	constexpr int NATIVE_SCHEDULE_SHIFT_OP_ADD = 3;

	void CallNativeScheduleShift(double elapsedSeconds)
	{
		uintptr_t scheduler = 0;
		if (!TryRead(NATIVE_SCHEDULER_GLOBAL, scheduler) || scheduler == 0)
		{
			LOG_INFO("(NBN LAS SHIFT) Scheduler global empty; shift skipped." << std::endl);
			return;
		}
		if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0
			|| elapsedSeconds > 120.0)
		{
			LOG_INFO("(NBN LAS SHIFT) Elapsed span " << elapsedSeconds
				<< "s out of bounds; shift skipped." << std::endl);
			return;
		}
		const uintptr_t fn = NATIVE_SCHEDULE_SHIFT_FN;
		// Constants inlined as literals (inline asm cannot see constexpr symbols):
		// eax = 4 = NATIVE_SCHEDULE_SHIFT_ENTRY, push 3 = OP_ADD.
		__asm {
			push edi
			mov eax, 4
			xor edi, edi
			sub esp, 8
			fld qword ptr elapsedSeconds
			fstp qword ptr [esp]
			push 3
			push scheduler
			mov ecx, fn
			call ecx
			pop edi
		}
		++scheduleShiftCount;
		LOG_INFO("(NBN LAS SHIFT) Native schedule shift applied: entry "
			<< NATIVE_SCHEDULE_SHIFT_ENTRY << " += " << std::fixed
			<< std::setprecision(3) << elapsedSeconds
			<< "s (call #" << scheduleShiftCount << ")." << std::endl);
	}

	constexpr uintptr_t NATIVE_CHORD_MATCHER_FN = 0x4E6E90;
	constexpr uintptr_t MATCHER_ARRANGEMENT_SLOT = 0x10;
	constexpr uintptr_t MATCHER_DET_SLOT = 0x08;
	constexpr uintptr_t MATCHER_STATE_SLOT = 0x04;
	constexpr uintptr_t MATCHER_GATE_A_OFFSET = 0xDD8;
	constexpr uintptr_t MATCHER_GATE_B_OFFSET = 0xD38;
	constexpr uintptr_t MATCHER_GATE_A_THRESHOLD = 0x1224418; // double global
	constexpr uintptr_t MATCHER_GATE_B_THRESHOLD = 0x12243A0; // double global

	int CallNativeChordMatcher(const int* midis, int count, int frameOffset)
	{
		if (midis == nullptr || count <= 0 || count > 6 || frameOffset < 0) return -1;
		uintptr_t root = 0, arrangement = 0, det = 0, state = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return -1;
		if (!TryRead(root + MATCHER_ARRANGEMENT_SLOT, arrangement) || arrangement == 0) return -1;
		if (!TryRead(arrangement + MATCHER_DET_SLOT, det) || det == 0) return -1;
		if (!TryRead(det + MATCHER_STATE_SLOT, state) || state == 0) return -1;
		// The game's own precondition: only match when real input energy is present. The
		// fields are floats; the thresholds are doubles (fcomp qword). Promote to compare.
		float gateA = 0.0f, gateB = 0.0f;
		double thrA = 0.0, thrB = 0.0;
		if (!TryRead(state + MATCHER_GATE_A_OFFSET, gateA)
			|| !TryRead(state + MATCHER_GATE_B_OFFSET, gateB)
			|| !TryRead(MATCHER_GATE_A_THRESHOLD, thrA)
			|| !TryRead(MATCHER_GATE_B_THRESHOLD, thrB))
		{
			return -1;
		}
		if (!std::isfinite(gateA) || !std::isfinite(gateB)
			|| static_cast<double>(gateA) < thrA
			|| static_cast<double>(gateB) < thrB)
		{
			return -2; // input too quiet - the game's own precondition failed; matcher not run
		}
		int arr[6] = { 0, 0, 0, 0, 0, 0 };
		for (int i = 0; i < count && i < 6; ++i) arr[i] = midis[i];
		const uintptr_t fn = NATIVE_CHORD_MATCHER_FN;
		int* arrPtr = arr;
		int cnt = count;
		int frameArg = frameOffset;
		uintptr_t detArg = det;
		unsigned char resultAl = 0;
		__asm {
			// __stdcall, args right-to-left: flag, frameOffset, count, midiArray, det.
			// ret 0x14 cleans all five - do NOT adjust esp afterward.
			push 0
			push frameArg
			push cnt
			push arrPtr
			push detArg
			mov ecx, fn
			call ecx
			mov resultAl, al
		}
		return (resultAl & 1) ? 1 : 0;
	}

	constexpr uintptr_t NATIVE_SINGLE_NOTE_MATCHER_FN = 0x4E7B30;
	int CallNativeSingleNoteMatcher(int expectedMidi, int frameOffset)
	{
		if (expectedMidi < 0 || frameOffset < 0) return -1;
		uintptr_t root = 0, arrangement = 0, det = 0, state = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return -1;
		if (!TryRead(root + MATCHER_ARRANGEMENT_SLOT, arrangement) || arrangement == 0) return -1;
		if (!TryRead(arrangement + MATCHER_DET_SLOT, det) || det == 0) return -1;
		if (!TryRead(det + MATCHER_STATE_SLOT, state) || state == 0) return -1;
		int expectedBuf[6] = { expectedMidi, -1, -1, -1, -1, -1 };
		const uintptr_t fn = NATIVE_SINGLE_NOTE_MATCHER_FN;
		int* bufPtr = expectedBuf;
		uintptr_t detArg = det;
		int flagArg = 0;
		int frameArg = frameOffset;
		unsigned char resultAl = 0;
		__asm {
			// __stdcall, args right-to-left: frameOffset, flag, expectedBuf, det.
			// ret 0x10 cleans all four - do NOT adjust esp afterward.
			push frameArg
			push flagArg
			push bufPtr
			push detArg
			mov ecx, fn
			call ecx
			mov resultAl, al
		}
		return (resultAl & 1) ? 1 : 0;
	}

	// Reads the newest analysis-ring frame's capture timestamp (frame+0x730, live audio
	// seconds - it keeps advancing while the transport is frozen). Used to stamp the hold
	// latch and as the "now" the native scan walks back from.
	bool TryReadCurrentRingTime(double& outTs)
	{
		uintptr_t root = 0, arrangement = 0, det = 0, state = 0, ring = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return false;
		if (!TryRead(root + MATCHER_ARRANGEMENT_SLOT, arrangement) || arrangement == 0) return false;
		if (!TryRead(arrangement + MATCHER_DET_SLOT, det) || det == 0) return false;
		if (!TryRead(det + MATCHER_STATE_SLOT, state) || state == 0) return false;
		if (!TryRead(state + DETECTOR_RING_BUFFER, ring) || ring == 0) return false;
		int32_t writeCursor = 0, capacity = 0;
		if (!TryRead(state + DETECTOR_RING_INDEX, writeCursor)
			|| !TryRead(state + DETECTOR_RING_CAPACITY, capacity)
			|| capacity <= 0 || capacity > DETECTOR_RING_MAX_CAPACITY
			|| writeCursor < 0 || writeCursor >= capacity)
		{
			return false;
		}
		return TryRead(ring + static_cast<uintptr_t>(writeCursor) * DETECTOR_RING_STRIDE
			+ RING_FRAME_TIMESTAMP, outTs) && std::isfinite(outTs);
	}

	// Cap on how many recent frames the port scans, so a long-held note does not walk
	// thousands of frames. A fresh onset+match is always recent (a strum's transient lasts
	// a handful of frames), so ~0.5s of ring history is ample; the sinceRingTime bound
	// stops the walk earlier for a freshly latched successor.
	constexpr int32_t NATIVE_SCAN_MAX_FRAMES = 48;

	bool NativeChordHitSinceLatch(const int* tones, int count, double sinceRingTime)
	{
		if (tones == nullptr || count <= 0) return false;
		uintptr_t root = 0, arrangement = 0, det = 0, state = 0, ring = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return false;
		if (!TryRead(root + MATCHER_ARRANGEMENT_SLOT, arrangement) || arrangement == 0) return false;
		if (!TryRead(arrangement + MATCHER_DET_SLOT, det) || det == 0) return false;
		if (!TryRead(det + MATCHER_STATE_SLOT, state) || state == 0) return false;
		if (!TryRead(state + DETECTOR_RING_BUFFER, ring) || ring == 0) return false;
		int32_t writeCursor = 0, capacity = 0, validCount = 0;
		if (!TryRead(state + DETECTOR_RING_INDEX, writeCursor)
			|| !TryRead(state + DETECTOR_RING_CAPACITY, capacity)
			|| !TryRead(state + DETECTOR_RING_VALID_COUNT, validCount)
			|| capacity <= 0 || capacity > DETECTOR_RING_MAX_CAPACITY
			|| writeCursor < 0 || writeCursor >= capacity || validCount <= 0)
		{
			return false;
		}
		if (validCount > capacity) validCount = capacity;
		int32_t scanLimit = validCount < NATIVE_SCAN_MAX_FRAMES ? validCount : NATIVE_SCAN_MAX_FRAMES;

		auto frameTimeAt = [&](int32_t frameOffset, double& ts) -> bool
		{
			int32_t idx = writeCursor - frameOffset;
			if (idx < 0) idx += capacity;
			return TryRead(ring + static_cast<uintptr_t>(idx) * DETECTOR_RING_STRIDE
				+ RING_FRAME_TIMESTAMP, ts) && std::isfinite(ts);
		};

		bool onsetFound = false;
		for (int32_t frameOffset = 0; frameOffset < scanLimit; ++frameOffset)
		{
			double frameTs = 0.0;
			if (!frameTimeAt(frameOffset, frameTs) || frameTs <= sinceRingTime) break;
			int32_t idx = writeCursor - frameOffset;
			if (idx < 0) idx += capacity;
			uint8_t onsetFlag = 0;
			if (TryRead(ring + static_cast<uintptr_t>(idx) * DETECTOR_RING_STRIDE
				+ RING_FRAME_ONSET_FLAG, onsetFlag) && onsetFlag != 0)
			{
				onsetFound = true;
				break;
			}
		}
		if (!onsetFound) return false;

		for (int32_t frameOffset = 0; frameOffset < scanLimit; ++frameOffset)
		{
			double frameTs = 0.0;
			if (!frameTimeAt(frameOffset, frameTs) || frameTs <= sinceRingTime) return false;
			if (CallNativeChordMatcher(tones, count, frameOffset) == 1) return true;
		}
		return false;
	}

	int32_t lastCapturedOnsetSeq = -1;
	void CaptureOnsetSpectrum(int expected)
	{
		uintptr_t root = 0, arrangement = 0, det = 0, state = 0, ring = 0;
		if (!TryRead(DETECTION_ROOT, root) || root == 0) return;
		if (!TryRead(root + MATCHER_ARRANGEMENT_SLOT, arrangement) || arrangement == 0) return;
		if (!TryRead(arrangement + MATCHER_DET_SLOT, det) || det == 0) return;
		if (!TryRead(det + MATCHER_STATE_SLOT, state) || state == 0) return;
		if (!TryRead(state + DETECTOR_RING_BUFFER, ring) || ring == 0) return;
		int32_t cursor = 0, capacity = 0;
		if (!TryRead(state + DETECTOR_RING_INDEX, cursor)
			|| !TryRead(state + DETECTOR_RING_CAPACITY, capacity)
			|| capacity <= 0 || capacity > DETECTOR_RING_MAX_CAPACITY
			|| cursor < 0 || cursor >= capacity)
		{
			return;
		}
		const uintptr_t frame = ring + static_cast<uintptr_t>(cursor) * DETECTOR_RING_STRIDE;
		uint8_t onsetFlag = 0;
		if (!TryRead(frame + RING_FRAME_ONSET_FLAG, onsetFlag) || onsetFlag == 0) return;
		int32_t seq = -1;
		TryRead(frame + RING_FRAME_SEQUENCE, seq);
		if (seq == lastCapturedOnsetSeq) return; // one capture per distinct onset frame
		lastCapturedOnsetSeq = seq;

		float level = 0.0f;
		TryRead(frame + RING_FRAME_LEVEL, level);
		int32_t count = 0;
		TryRead(frame + RING_FRAME_PAIR_COUNT, count);
		if (count < 0) count = 0;
		if (count > 48) count = 48;
		struct PeakEnergy { int midi; float energy; };
		PeakEnergy peaks[48];
		int found = 0;
		for (int32_t i = 0; i < count; ++i)
		{
			int32_t midi = -1;
			float energy = 0.0f;
			if (TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8, midi)
				&& TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8 + 4, energy)
				&& midi >= 0 && midi < 128 && std::isfinite(energy) && energy > 0.0f)
			{
				peaks[found].midi = midi;
				peaks[found].energy = energy;
				++found;
			}
		}
		for (int a = 0; a < found; ++a)
		{
			for (int b = a + 1; b < found; ++b)
			{
				if (peaks[b].energy > peaks[a].energy)
				{
					PeakEnergy tmp = peaks[a];
					peaks[a] = peaks[b];
					peaks[b] = tmp;
				}
			}
		}
		std::ostringstream ss;
		const int top = found < 14 ? found : 14;
		for (int i = 0; i < top; ++i)
		{
			if (i) ss << ' ';
			ss << peaks[i].midi << ':' << std::fixed << std::setprecision(1) << peaks[i].energy;
		}
		const int ndLoudest = QueryNativeLoudestPlayedNote();
		LOG_INFO("(NBN CAPTURE) exp=" << expected << " ndLoudest=" << ndLoudest
			<< " domPeak=" << (found > 0 ? peaks[0].midi : -1) << " lvl=" << std::fixed << std::setprecision(1)
			<< level << " peaks=[" << ss.str() << "]" << std::endl);
	}

	void LogTier0ShadowEvidence(int expectedMidi, const char* context)
	{
		if (expectedMidi < 0) return;
		NoteByNoteProtocol::RawToneEvidence evidence;
		const double frequency =
			440.0 * std::pow(2.0, (static_cast<double>(expectedMidi) - 69.0) / 12.0);
		if (!NoteByNoteRuntime::QueryRawToneEvidence(frequency, 0.15f, evidence)) return;
		LOG_INFO("(NBN TIER0) " << context << " exp=" << expectedMidi
			<< std::fixed << std::setprecision(1) << " f=" << frequency
			<< std::setprecision(4)
			<< " tgt=" << evidence.targetPower
			<< " -1=" << evidence.minusOnePower
			<< " +1=" << evidence.plusOnePower
			<< " -2=" << evidence.minusTwoPower
			<< " +2=" << evidence.plusTwoPower
			<< " rms=" << evidence.totalRms
			<< " win=" << evidence.windowSampleCount << "@" << evidence.sampleRate
			<< std::endl);
	}

	void LogTier1ShadowPitch(int expectedMidi)
	{
		if (expectedMidi < 0) return;
		if (!NoteByNoteRuntime::IsMlPitchServiceAlive()) return;
		// No internal throttle: every call site paces itself (accept is a rare event,
		// reject and the hold-phase sampler are throttled where they fire), and a shared
		// throttle here let the 300ms HOLD sampler starve the accept-moment lines - the
		// exact pairs the offline comparison needs most.
		float mlMidi = 0.0f;
		float mlConfidence = 0.0f;
		double mlAgeSeconds = 0.0;
		if (!NoteByNoteRuntime::QueryMlPitch(mlMidi, mlConfidence, mlAgeSeconds)) return;
		LOG_INFO("(NBN TIER1) shadow exp=" << expectedMidi
			<< std::fixed << std::setprecision(2)
			<< " ml=" << mlMidi
			<< " conf=" << mlConfidence
			<< std::setprecision(3) << " age=" << mlAgeSeconds
			<< std::endl);
	}



	void SampleDetectionComparison()
	{
		if (expectedMidi < 0) return;
		const DetectionTechnique technique = CurrentDetectionTechnique();

		const bool haveChordTones = technique == DetectionTechnique::Chord
			&& researchChordToneCount > 0 && researchChordTonesRecord == selectedRecord;
		auto pitchIsTarget = [&](int midi) -> bool
		{
			if (midi < 0) return false;
			if (haveChordTones)
			{
				for (int i = 0; i < researchChordToneCount; ++i)
					if (researchChordTones[i] == midi) return true;
				return false;
			}
			if (midi == expectedMidi) return true;
			if (technique == DetectionTechnique::Bend && bendAcceptMidi >= 0 && midi == bendAcceptMidi)
				return true;
			return false;
		};

		const int nativeMidi = QueryNativeLoudestPlayedNote();
		const bool nativeMatch = pitchIsTarget(nativeMidi);

		bool mlHadOpinion = false;
		bool mlMatch = false;
		int mlMidiInt = -1;
		if (NoteByNoteRuntime::IsMlPitchServiceAlive())
		{
			float mlMidi = 0.0f, mlConfidence = 0.0f;
			double mlAgeSeconds = 0.0;
			if (NoteByNoteRuntime::QueryMlPitch(mlMidi, mlConfidence, mlAgeSeconds)
				&& mlConfidence >= 0.30f && mlAgeSeconds <= 0.75)
			{
				mlHadOpinion = true;
				mlMidiInt = static_cast<int>(std::lround(mlMidi));
				mlMatch = pitchIsTarget(mlMidiInt);
			}
		}

		const int code = !mlHadOpinion ? 2 : (nativeMatch == mlMatch ? 1 : 0);
		auto& c = g_detectionComparison;
		if (mlHadOpinion) { if (code == 1) ++c.agree; else ++c.disagree; }
		c.lastTechnique = static_cast<int>(technique);
		c.lastNativeMatch = nativeMatch;
		c.lastMlMatch = mlMatch;
		c.lastMlHadOpinion = mlHadOpinion;
		c.lastValid = true;
		if (c.historyCount < DETECTION_COMPARE_HISTORY)
		{
			c.history[c.historyCount++] = static_cast<uint8_t>(code);
		}
		else
		{
			for (uint32_t i = 1; i < DETECTION_COMPARE_HISTORY; ++i) c.history[i - 1] = c.history[i];
			c.history[DETECTION_COMPARE_HISTORY - 1] = static_cast<uint8_t>(code);
		}
	}

	volatile bool g_tier0Enforcement = true;

	constexpr double TIER0_TARGET_DETUNE_RATIO = 1.0145453349375237; // 2^(25/1200)

	float Tier0WindowSecondsForFrequency(double frequencyHz)
	{
		if (frequencyHz < 200.0) return 0.30f;    // midi < ~55: lobe ~3.3 Hz
		if (frequencyHz < 330.0) return 0.20f;    // midi < ~64: lobe ~5 Hz
		return 0.15f;
	}

	bool QueryTier0EvidenceWideTarget(int expectedMidi, NoteByNoteProtocol::RawToneEvidence& evidence)
	{
		const double frequency =
			440.0 * std::pow(2.0, (static_cast<double>(expectedMidi) - 69.0) / 12.0);
		const float windowSeconds = Tier0WindowSecondsForFrequency(frequency);
		if (!NoteByNoteRuntime::QueryRawToneEvidence(frequency, windowSeconds, evidence))
		{
			return false;
		}
		NoteByNoteProtocol::RawToneEvidence detuned;
		if (NoteByNoteRuntime::QueryRawToneEvidence(
				frequency * TIER0_TARGET_DETUNE_RATIO, windowSeconds, detuned)
			&& detuned.targetPower > evidence.targetPower)
		{
			evidence.targetPower = detuned.targetPower;
		}
		if (NoteByNoteRuntime::QueryRawToneEvidence(
				frequency / TIER0_TARGET_DETUNE_RATIO, windowSeconds, detuned)
			&& detuned.targetPower > evidence.targetPower)
		{
			evidence.targetPower = detuned.targetPower;
		}
		return true;
	}

	// Log-only: compare window lengths on one snapshot before evaluating attack-pitch hypotheses.
	volatile bool g_sharpCorridorLog = true;
	void LogCentsComb(int expectedMidi)
	{
		const double expectedFrequency = 440.0 * std::pow(2.0, (expectedMidi - 69.0) / 12.0);
		NoteByNoteProtocol::RawToneComb comb;
		if (!NoteByNoteRuntime::QueryRawToneComb(expectedFrequency, comb))
		{
			LOG_INFO("(NBN TIER0 COMB) exp=" << expectedMidi << " snapshot=unavailable" << std::endl);
			return;
		}
		constexpr int WINDOW_MS[] = { 50, 100, 150 };
		for (int window = 0; window < 3; ++window)
		{
			int peakBin = 0;
			float corridorMax = 0.0f;
			float outsideMax = 0.0f;
			std::ostringstream bins;
			bins << std::scientific << std::setprecision(6);
			for (int bin = 0; bin < 17; ++bin)
			{
				const float power = comb.powers[window][bin];
				if (power > comb.powers[window][peakBin]) peakBin = bin;
				const float cents = -50.0f + bin * 12.5f;
				if (cents >= 25.0f && cents <= 80.0f)
				{
					if (power > corridorMax) corridorMax = power;
				}
				else if (power > outsideMax)
				{
					outsideMax = power;
				}
				if (bin != 0) bins << ',';
				bins << power;
			}
			LOG_INFO("(NBN TIER0 COMB) exp=" << expectedMidi
				<< " endSample=" << comb.endSampleIndex << " rate=" << comb.sampleRate
				<< " windowMs=" << WINDOW_MS[window] << " samples=" << comb.sampleCounts[window]
				<< " peakBinCents=" << (-50.0f + peakBin * 12.5f)
				<< std::scientific << std::setprecision(6)
				<< " rms=" << comb.rms[window] << " peakPow=" << comb.powers[window][peakBin]
				<< " corridorMax=" << corridorMax << " outsideMax=" << outsideMax
				<< " bins=" << bins.str() << std::defaultfloat << std::endl);
		}
	}


	bool QueryTier0WideProbePower(double centerHz, float& outPower)
	{
		const float windowSeconds = Tier0WindowSecondsForFrequency(centerHz);
		NoteByNoteProtocol::RawToneEvidence probe;
		if (!NoteByNoteRuntime::QueryRawToneEvidence(centerHz, windowSeconds, probe)) return false;
		outPower = probe.targetPower;
		if (NoteByNoteRuntime::QueryRawToneEvidence(
				centerHz * TIER0_TARGET_DETUNE_RATIO, windowSeconds, probe)
			&& probe.targetPower > outPower)
		{
			outPower = probe.targetPower;
		}
		if (NoteByNoteRuntime::QueryRawToneEvidence(
				centerHz / TIER0_TARGET_DETUNE_RATIO, windowSeconds, probe)
			&& probe.targetPower > outPower)
		{
			outPower = probe.targetPower;
		}
		return true;
	}

	bool TryEstimateRawBendPitch(double baseMidi, double targetMidi,
		float& outMidi, float& outConfidence)
	{
		outMidi = -1.0f;
		outConfidence = 0.0f;
		if (!std::isfinite(baseMidi) || !std::isfinite(targetMidi) || targetMidi <= baseMidi)
			return false;

		// Throttle: recompute at most every ~40 ms and reuse the last estimate between, so the
		// needle updates ~25x/s (the host low-pass smooths it) at a fraction of the per-frame
		// cost. The earlier per-frame scan (10 raw probes) jagged the bend; this is one probe,
		// skipped most frames.
		static std::chrono::steady_clock::time_point lastAt{};
		static double lastTarget = -1.0;
		static float lastMidi = -1.0f;
		static float lastConf = 0.0f;
		static bool lastValid = false;
		const auto now = std::chrono::steady_clock::now();
		if (targetMidi == lastTarget
			&& std::chrono::duration<double>(now - lastAt).count() < 0.040)
		{
			outMidi = lastMidi;
			outConfidence = lastConf;
			return lastValid;
		}
		lastAt = now;
		lastTarget = targetMidi;
		lastValid = false;

		// ONE probe centered at mid-bend: its built-in target / +-1 / +-2 powers span the whole
		// bend for depths up to 3 semitones, so a single QueryRawToneEvidence covers it. Parabola-
		// refine the power peak for a sub-semitone pitch.
		const double center = std::floor((baseMidi + targetMidi) * 0.5 + 0.5);
		const double freq = 440.0 * std::pow(2.0, (center - 69.0) / 12.0);
		const float windowSeconds = Tier0WindowSecondsForFrequency(freq);
		NoteByNoteProtocol::RawToneEvidence e;
		if (!NoteByNoteRuntime::QueryRawToneEvidence(freq, windowSeconds, e)) return false;
		if (!std::isfinite(e.totalRms) || e.totalRms < 0.015f) return false;

		const double p[5] = { e.minusTwoPower, e.minusOnePower, e.targetPower,
			e.plusOnePower, e.plusTwoPower };
		const double m[5] = { center - 2.0, center - 1.0, center, center + 1.0, center + 2.0 };
		int peak = 0;
		double mean = 0.0;
		for (int i = 0; i < 5; ++i)
		{
			if (!std::isfinite(p[i])) return false;
			mean += p[i];
			if (p[i] > p[peak]) peak = i;
		}
		mean /= 5.0;
		if (!(p[peak] > 0.0) || mean <= 0.0) return false;

		double refined = m[peak];
		if (peak > 0 && peak < 4)
		{
			const double y0 = p[peak - 1];
			const double y1 = p[peak];
			const double y2 = p[peak + 1];
			const double denom = y0 - 2.0 * y1 + y2;
			if (denom < 0.0)
			{
				const double delta = 0.5 * (y0 - y2) / denom; // +-0.5 semitone about the peak bin
				if (delta > -1.0 && delta < 1.0) refined = m[peak] + delta;
			}
		}
		if (refined < baseMidi - 0.5) refined = baseMidi - 0.5;
		if (refined > targetMidi + 1.0) refined = targetMidi + 1.0;

		const double contrast = p[peak] / mean;
		double confidence = (contrast - 1.5) * 40.0;
		if (confidence < 0.0) confidence = 0.0;
		if (confidence > 100.0) confidence = 100.0;

		lastMidi = static_cast<float>(refined);
		lastConf = static_cast<float>(confidence);
		lastValid = true;
		outMidi = lastMidi;
		outConfidence = lastConf;
		return true;
	}

	bool Tier0ConfirmsExpected(int expectedMidi)
	{
		if (expectedMidi < 0) return false;
		NoteByNoteProtocol::RawToneEvidence evidence;
		const double frequency =
			440.0 * std::pow(2.0, (static_cast<double>(expectedMidi) - 69.0) / 12.0);
		if (!QueryTier0EvidenceWideTarget(expectedMidi, evidence)) return false;
		if (evidence.targetPower < 1e-5f) return false;
		float worstNeighbour = evidence.minusOnePower;
		if (evidence.plusOnePower > worstNeighbour) worstNeighbour = evidence.plusOnePower;
		if (evidence.minusTwoPower > worstNeighbour) worstNeighbour = evidence.minusTwoPower;
		if (evidence.plusTwoPower > worstNeighbour) worstNeighbour = evidence.plusTwoPower;
		if (evidence.targetPower < worstNeighbour * 5.0f) return false;

		// The sub-harmonic guards get the same +/-25-cent widening as the target (shared
		// helper QueryTier0WideProbePower, hoisted so the veto runs the same guards).
		auto wideTargetPower = [](double centerHz, float& outPower) -> bool
		{
			return QueryTier0WideProbePower(centerHz, outPower);
		};
		NoteByNoteProtocol::RawToneEvidence subOctave = {};
		NoteByNoteProtocol::RawToneEvidence subTwelfth = {};
		float fifthAbovePower = 0.0f;
		if (!wideTargetPower(frequency * 0.5, subOctave.targetPower)
			|| !wideTargetPower(frequency / 3.0, subTwelfth.targetPower)
			|| !wideTargetPower(frequency * 1.5, fifthAbovePower))
		{
			return false;
		}
		if (subOctave.targetPower >= evidence.targetPower * 0.4f
			|| subTwelfth.targetPower >= evidence.targetPower * 0.4f)
		{
			return false;                  // a lower note's harmonic, not the note itself
		}
		if (fifthAbovePower >= evidence.targetPower * 0.1f && fifthAbovePower >= 1e-5f)
		{
			static ULONGLONG lastPhantomLogTick = 0;
			const ULONGLONG nowTick = GetTickCount64();
			if (nowTick - lastPhantomLogTick >= 250)
			{
				lastPhantomLogTick = nowTick;
				LOG_INFO("(NBN TIER0) RESCUE-REFUSED exp=" << expectedMidi
					<< std::fixed << std::setprecision(6)
					<< " tgt=" << evidence.targetPower
					<< " fifthAbove=" << fifthAbovePower
					<< " - sub-octave note betrayed by its 3rd harmonic at 1.5f."
					<< std::endl);
			}
			return false;
		}

		LOG_INFO("(NBN TIER0) RESCUE exp=" << expectedMidi
			<< std::fixed << std::setprecision(6)
			<< " tgt=" << evidence.targetPower
			<< " worstNeighbour=" << worstNeighbour
			<< " subOct=" << subOctave.targetPower
			<< " subTwelfth=" << subTwelfth.targetPower
			<< " fifthAbove=" << fifthAbovePower
			<< " rms=" << std::setprecision(4) << evidence.totalRms
			<< " - raw fundamental confirmed despite the bin verdict." << std::endl);
		return true;
	}

	volatile bool isChordTier0RescueEnabled = false;

	bool Tier0ConfirmsChord(const int* tones, int count)
	{
		if (tones == nullptr || count < 2) return false;   // singles use Tier0ConfirmsExpected
		const int n = count < 6 ? count : 6;
		double toneHz[6] = { 0 };
		float tonePower[6] = { 0 };
		for (int i = 0; i < n; ++i)
		{
			if (tones[i] < 0) return false;
			toneHz[i] = 440.0 * std::pow(2.0, (static_cast<double>(tones[i]) - 69.0) / 12.0);
			if (!QueryTier0WideProbePower(toneHz[i], tonePower[i])) return false;
			if (tonePower[i] < 1e-4f) return false;         // every tone must carry real energy
		}
		// Chord-aware dominance: each tone must beat its +/-1 semitone neighbours, EXCEPT a
		// neighbour that is (near) another chord tone - those bins are legitimately lit by the
		// chord itself and must not veto a present tone.
		const double semi = std::pow(2.0, 1.0 / 12.0);
		for (int i = 0; i < n; ++i)
		{
			for (int side = 0; side < 2; ++side)
			{
				const int nbMidi = tones[i] + (side == 0 ? 1 : -1);
				bool nearOtherTone = false;
				for (int j = 0; j < n; ++j)
					if (j != i && std::abs(nbMidi - tones[j]) <= 1) { nearOtherTone = true; break; }
				if (nearOtherTone) continue;
				float nbPower = 0.0f;
				if (!QueryTier0WideProbePower(side == 0 ? toneHz[i] * semi : toneHz[i] / semi, nbPower))
					continue;
				if (tonePower[i] < nbPower * 2.0f) return false;   // a neighbour rivals the tone
			}
		}
		return true;   // every expected tone present and dominant
	}

	// Native-drive phase A: the StartAt core (0x4749C0), the game's own
	// resume-and-seek, callable against our owner. Convention pinned from raw
	// disassembly (0x405F85..8B and the core's own prologue/RET):
	//   EAX = owner (also copied to ECX internally for the thiscall chain),
	//   one stack arg = pointer to a game tag-string object, RET 0x4 (callee
	//   cleans). Body: owner+0x5E3 word <- 0x100 (frozen-on-tag off,
	//   resuming-from-tag on), unfreeze core 0x474890, seek 0x7E8A50(tag),
	//   RTPC 0x3F1 <- 1.0 on owner+0x37C (speed), 0x7E98A0(1.0), and
	//   [owner+0x348]+0x9C <- 1 (start pending).
	//
	// The tag-string layout, decoded from the seek's own reads (0x7E8AB4..C3)
	// and the lesson constructor's init (0x471A30): +0x00 inline buffer (or
	// heap pointer), +0x10 end pointer, +0x14 the inline marker - it holds the
	// ADDRESS of the +0x10 field while the string is inline. An EMPTY tag is a
	// zeroed buffer with end pointing at the buffer - the shipped
	// GE_StartAt("") path, which seeks to the current position.
	//
	// Test entry: `native-seek-test` over the bridge sets the request flag; the
	// scoring detour (main thread - the same thread the GE handlers run on)
	// performs ONE call while the controller is idle, so the experiment never
	// races the music service from the pipe thread or lands mid-hold.
	constexpr uintptr_t NATIVE_STARTAT_CORE = 0x4749C0;
	volatile bool isNativeSeekTestRequested = false;

	struct GameTagString
	{
		char inlineBuffer[0x10];
		char* endPointer;
		void* inlineMarker;
	};
	static_assert(sizeof(GameTagString) == 0x18, "tag string layout is 0x18 bytes");

	void CallNativeStartAtNow(void* owner)
	{
		const uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		uintptr_t ownerVtable = 0;
		if (TryRead(ownerAddress, ownerVtable) && ownerVtable == LAS_OWNER_VTABLE)
		{
			LOG_INFO("(NBN NATIVE SEEK) REFUSED StartAt core on the LAS owner 0x"
				<< std::hex << ownerAddress << std::dec
				<< ": 0x4749C0 writes the frozen-object field owner+0x5E3, which is"
				<< " out of bounds on the 0x5D0-byte LAS object (heap smash). The"
				<< " coordinated rebuild carries the resume." << std::endl);
			return;
		}
		GameTagString emptyTag = {};
		emptyTag.endPointer = emptyTag.inlineBuffer;
		emptyTag.inlineMarker = &emptyTag.endPointer;
		GameTagString* tagPtr = &emptyTag;
		const uintptr_t fn = NATIVE_STARTAT_CORE;
		LOG_INFO("(NBN NATIVE SEEK) Calling the StartAt core with an empty tag"
			<< " (resume + seek-to-now + speed reset) on owner=0x" << std::hex
			<< ownerAddress << std::dec << "." << std::endl);
		__asm {
			push esi
			push edi
			mov eax, ownerAddress
			push tagPtr
			mov ecx, fn
			call ecx
			pop edi
			pop esi
		}
		LOG_INFO("(NBN NATIVE SEEK) StartAt core returned." << std::endl);
	}

	volatile bool isNativeReleaseEnabled = true;
	constexpr uintptr_t OWNER_PRESENTATION_COMPONENT = 0x348;
	constexpr uintptr_t PRESENTATION_ACTIVE_FLAG = 0x9C;

	constexpr uintptr_t NATIVE_FREEZESONG_CORE = 0x474840;
	constexpr uintptr_t NATIVE_UNFREEZESONG_CORE = 0x474890;
	volatile bool isFreezeModeTestArmed = false;
	bool didFreezeModeEnter = false;

	void CallNativeFreezeSongCore(void* owner)
	{
		const uintptr_t fn = NATIVE_FREEZESONG_CORE;
		const uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		__asm {
			push esi
			push edi
			mov eax, ownerAddress
			mov ecx, fn
			call ecx
			pop edi
			pop esi
		}
		LOG_INFO("(NBN MODE) FreezeSong core returned." << std::endl);
	}

	void CallNativeUnfreezeSongCore(void* owner)
	{
		const uintptr_t fn = NATIVE_UNFREEZESONG_CORE;
		const uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		__asm {
			push esi
			push edi
			mov ecx, ownerAddress
			mov edx, fn
			call edx
			pop edi
			pop esi
		}
		LOG_INFO("(NBN MODE) UnfreezeSong core returned." << std::endl);
	}


	// Native-drive step 1: the ground-truth line (pure reads; see the constants
	// block above OWNER_FROZEN_ON_TAG for what each field answers).
	void LogNativeFreezeGroundTruth(void* owner, const char* moment)
	{
		const auto ownerAddress = reinterpret_cast<uintptr_t>(owner);
		// The GE resolve chain, exactly as the handlers walk it.
		uintptr_t chainRoot = 0, chainHop = 0, resolved = 0, resolvedVtable = 0;
		const bool chainAlive = TryRead(CHORD_DISPLAY_GLOBAL, chainRoot) && chainRoot != 0
			&& TryRead(chainRoot + CHORD_DISPLAY_GLOBAL_HOP, chainHop) && chainHop != 0
			&& TryRead(chainHop + CHORD_DISPLAY_WRAPPER_SLOT, resolved) && resolved != 0;
		if (chainAlive) TryRead(resolved, resolvedVtable);
		// The scheduler the frozen-span destructor shifts.
		uintptr_t scheduler = 0;
		TryRead(NATIVE_SCHEDULER_GLOBAL, scheduler);
		// The owner's freeze-family state.
		uintptr_t tagBegin = 0, tagEnd = 0;
		uint8_t resumeFlag = 0xFF;
		double freezeStart = 0.0;
		TryRead(ownerAddress + OWNER_FREEZE_TAG_BEGIN, tagBegin);
		TryRead(ownerAddress + OWNER_FREEZE_TAG_END, tagEnd);
		TryRead(ownerAddress + OWNER_RESUME_FROM_TAG, resumeFlag);
		TryRead(ownerAddress + OWNER_FREEZE_START_TIME, freezeStart);
		LOG_INFO("(NBN NATIVE GROUND) moment=" << moment
			<< " geChain=" << (chainAlive ? "alive" : "dead")
			<< " resolved=0x" << std::hex << resolved
			<< " resolvedVtable=0x" << resolvedVtable
			<< " (owner=0x" << ownerAddress
			<< " ownerVtable=0x" << LAS_OWNER_VTABLE << ")"
			<< " scheduler=0x" << scheduler << std::dec
			<< " tagLen=" << (tagEnd >= tagBegin ? tagEnd - tagBegin : 0)
			<< " resumeFlag=" << static_cast<int>(resumeFlag)
			<< " freezeStart=" << std::fixed << std::setprecision(3) << freezeStart
			<< std::endl);
	}
	// Defined with the freeze-flag block below.
	bool TryWriteGameByte(uintptr_t address, uint8_t value) noexcept;

	bool TryWriteGameBytes(uintptr_t destination, const void* source, size_t length) noexcept
	{
		__try
		{
			memcpy(reinterpret_cast<void*>(destination), source, length);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void ShowNativeChordPanel(void* owner, int chordId)
	{
		(void)owner; // the scoring owner's +0x78 is NOT the chord display; see above.
		if (!isNativeChordPanelEnabled || chordId < 0) return;
		uintptr_t globalHead = 0;
		uintptr_t hop = 0;
		uintptr_t wrapper = 0;
		uintptr_t component = 0;
		uintptr_t base = 0;
		uintptr_t end = 0;
		if (!TryRead(CHORD_DISPLAY_GLOBAL, globalHead) || globalHead == 0
			|| !TryRead(globalHead + CHORD_DISPLAY_GLOBAL_HOP, hop) || hop == 0
			|| !TryRead(hop + CHORD_DISPLAY_WRAPPER_SLOT, wrapper) || wrapper == 0
			|| !TryRead(wrapper + OWNER_CHORD_DISPLAY_COMPONENT, component)
			|| component == 0
			|| !TryRead(component + CHORD_DISPLAY_TEMPLATE_BEGIN, base) || base == 0
			|| !TryRead(component + CHORD_DISPLAY_TEMPLATE_END, end)
			|| end <= base
			|| static_cast<uintptr_t>(chordId) >= (end - base) / sizeof(ChordTemplateView))
		{
			LOG_INFO("(NBN LAS CHORD PANEL) Chord display component unavailable for chordId="
				<< chordId << "; panel not driven." << std::endl);
			return;
		}
		ChordTemplateView view = {};
		if (!TryRead(base + static_cast<uintptr_t>(chordId) * sizeof(ChordTemplateView), view))
		{
			return;
		}
		const bool wroteTemplate = TryWriteGameBytes(
			component + CHORD_DISPLAY_SLOT, &view, sizeof(view));
		const bool wroteVisible = TryWriteGameByte(component + CHORD_DISPLAY_VISIBLE, 1);
		if (wroteTemplate && wroteVisible) shownChordPanelComponent = component;
		char safeName[sizeof(view.name) + 1] = {};
		memcpy(safeName, view.name, sizeof(view.name));
		LOG_INFO("(NBN LAS CHORD PANEL) Native chord display driven for chordId=" << chordId
			<< " name=" << (safeName[0] != '\0' ? safeName : "?")
			<< " template=" << wroteTemplate << " visible=" << wroteVisible
			<< "." << std::endl);
	}

	void HideNativeChordPanel()
	{
		if (shownChordPanelComponent == 0) return;
		// Re-resolve through the same chain Show used; only clear the visible
		// byte if the live component still is the one we drove, so a component
		// freed by a rebuild between Show and Hide is never written through.
		uintptr_t globalHead = 0;
		uintptr_t hop = 0;
		uintptr_t wrapper = 0;
		uintptr_t component = 0;
		const bool stillLive =
			TryRead(CHORD_DISPLAY_GLOBAL, globalHead) && globalHead != 0
			&& TryRead(globalHead + CHORD_DISPLAY_GLOBAL_HOP, hop) && hop != 0
			&& TryRead(hop + CHORD_DISPLAY_WRAPPER_SLOT, wrapper) && wrapper != 0
			&& TryRead(wrapper + OWNER_CHORD_DISPLAY_COMPONENT, component)
			&& component == shownChordPanelComponent;
		if (stillLive) TryWriteGameByte(shownChordPanelComponent + CHORD_DISPLAY_VISIBLE, 0);
		shownChordPanelComponent = 0;
		LOG_INFO("(NBN LAS CHORD PANEL) Native chord display hidden (live=" << stillLive
			<< ")." << std::endl);
	}

	bool TryWriteGameByte(uintptr_t address, uint8_t value) noexcept
	{
		__try
		{
			*reinterpret_cast<volatile uint8_t*>(address) = value;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void* freezeFlagSetOwner = nullptr;

	void SetNativeFreezeFlag(void* owner, bool frozen)
	{
		if (owner == nullptr) return;
		if (frozen && !isNativeFreezeFlagEnabled) return;
		if (!frozen && owner != freezeFlagSetOwner) return;
		if (frozen)
		{
			uintptr_t tagBegin = 0, tagEnd = 0;
			uint8_t resumeFlag = 0xFF;
			const bool readable =
				TryRead(reinterpret_cast<uintptr_t>(owner) + OWNER_FREEZE_TAG_BEGIN, tagBegin)
				&& TryRead(reinterpret_cast<uintptr_t>(owner) + OWNER_FREEZE_TAG_END, tagEnd)
				&& TryRead(reinterpret_cast<uintptr_t>(owner) + OWNER_RESUME_FROM_TAG, resumeFlag);
			constexpr uintptr_t TAG_LENGTH_SANITY_LIMIT = 64u * 1024u * 1024u;
			const uintptr_t tagLength = tagEnd >= tagBegin ? tagEnd - tagBegin : 0;
			const bool tagSane = readable
				&& tagBegin != 0
				&& tagEnd > tagBegin
				&& tagLength <= TAG_LENGTH_SANITY_LIMIT
				&& (resumeFlag == 0 || resumeFlag == 1);
			if (!tagSane)
			{
				LOG_INFO("(NBN LAS FREEZE FLAG) freeze-family fields read uninitialized"
					<< " (tagBegin=0x" << std::hex << tagBegin
					<< " tagEnd=0x" << tagEnd << std::dec
					<< " resumeFlag=" << static_cast<int>(resumeFlag)
					<< "); no write performed." << std::endl);
				return;
			}
			LOG_INFO("(NBN LAS FREEZE FLAG) tag sane at write time: tagLen=" << tagLength
				<< " resumeFlag=" << static_cast<int>(resumeFlag) << "." << std::endl);
		}
		const bool didWrite = TryWriteGameByte(
			reinterpret_cast<uintptr_t>(owner) + OWNER_FROZEN_ON_TAG, frozen ? 1 : 0);
		freezeFlagSetOwner = (frozen && didWrite) ? owner : nullptr;
		LOG_INFO("(NBN LAS FREEZE FLAG) owner+0x5E3 <- " << (frozen ? 1 : 0)
			<< " written=" << didWrite << "." << std::endl);
	}

	uintptr_t sanitizedFreezeOwners[8] = {};
	size_t sanitizedFreezeOwnerNext = 0;

	void SanitizeOwnerFreezeStateOnce(void* owner)
	{
		const auto base = reinterpret_cast<uintptr_t>(owner);
		if (base == 0) return;
		for (const auto known : sanitizedFreezeOwners)
		{
			if (known == base) return;
		}
		sanitizedFreezeOwners[sanitizedFreezeOwnerNext] = base;
		sanitizedFreezeOwnerNext = (sanitizedFreezeOwnerNext + 1)
			% (sizeof(sanitizedFreezeOwners) / sizeof(sanitizedFreezeOwners[0]));

		uintptr_t tagBegin = 0, tagEnd = 0;
		TryRead(base + OWNER_FREEZE_TAG_BEGIN, tagBegin);
		TryRead(base + OWNER_FREEZE_TAG_END, tagEnd);
		const uintptr_t tagBase = base + OWNER_FREEZE_TAG_STRING;
		const uint32_t bufferAddress = static_cast<uint32_t>(tagBase);
		const uint32_t endFieldAddress = static_cast<uint32_t>(base + OWNER_FREEZE_TAG_BEGIN);
		const double zeroTime = 0.0;
		const bool wroteBegin = TryWriteGameBytes(
			base + OWNER_FREEZE_TAG_BEGIN, &bufferAddress, sizeof(bufferAddress));
		const bool wroteEnd = TryWriteGameBytes(
			base + OWNER_FREEZE_TAG_END, &endFieldAddress, sizeof(endFieldAddress));
		const bool wroteNul = TryWriteGameByte(tagBase, 0);
		const bool wroteFrozenSong = TryWriteGameByte(base + 0x5E2, 0);
		const bool wroteResume = TryWriteGameByte(base + OWNER_RESUME_FROM_TAG, 0);
		const bool wroteFrozen = TryWriteGameByte(base + OWNER_FROZEN_ON_TAG, 0);
		const bool wroteStart = TryWriteGameBytes(
			base + OWNER_FREEZE_START_TIME, &zeroTime, sizeof(zeroTime));
		LOG_INFO("(NBN LAS FREEZE INIT) Owner 0x" << std::hex << base
			<< " freeze state initialized to the valid empty form at bootstrap"
			<< " (was tagBegin=0x" << tagBegin << " tagEnd=0x" << tagEnd << std::dec
			<< "); wrote begin/end/nul/frozenSong/resume/frozen/start="
			<< wroteBegin << wroteEnd << wroteNul << wroteFrozenSong
			<< wroteResume << wroteFrozen << wroteStart << "." << std::endl);
	}

	void PerformOwnedReleaseAtEpoch(void* owner, float releaseEpoch, const char* reason)
	{
		LogNativeFreezeGroundTruth(owner, "release");
		// Step 5 first flight: exit the native frozen mode BEFORE our own
		// flag clear, so the UnfreezeSong core still sees +0x5E3 set and runs
		// the full native ResumeFromTag unwind, then mode (0,2). The
		// mechanical restart below still runs afterward - belt and braces
		// while the observation is presentation behavior.
		if (didFreezeModeEnter)
		{
			didFreezeModeEnter = false;
			LogNativeFreezeGroundTruth(owner, "mode-unfreeze-before");
			LOG_INFO("(NBN MODE) Exiting the native frozen mode: UnfreezeSong core"
				<< " (ResumeFromTag unwind if tagged, then mode call 0,2)." << std::endl);
			CallNativeUnfreezeSongCore(owner);
			LogNativeFreezeGroundTruth(owner, "mode-unfreeze-after");
		}
		SetNativeFreezeFlag(owner, false);
		// Native schedule-shift experiment (step 2): compensate the engine for
		// the frozen span exactly as the frozen-span destructor does, right as
		// the transport is about to resume. Toggle-gated, default off.
		if (isScheduleShiftEnabled && hasTransportFrozenAt)
		{
			const double frozenSeconds = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - transportFrozenAt).count();
			CallNativeScheduleShift(frozenSeconds);
		}
		hasTransportFrozenAt = false;
		HideNativeChordPanel();
		if (isNativeReleaseEnabled)
		{
			LogNativeFreezeGroundTruth(owner, "native-release-before");
			LOG_INFO("(NBN NATIVE RELEASE) Running the game's own resume (StartAt core,"
				<< " empty tag) instead of the coordinated PlayerSong restart at epoch="
				<< std::fixed << std::setprecision(6) << releaseEpoch
				<< " heldEpoch=" << heldEpoch << " (" << reason << ")." << std::endl);
			reinterpret_cast<MarkNotesFn>(MARK_ACTIVE_NOTES)(nullptr, owner);
			CallNativeStartAtNow(owner);
			// The music itself: the StartAt core does not restart the PlayerSong
			// after a Stop_TMusic latch (both pure-native attempts left it
			// stopped=1), so the proven coordinated restart carries the play.
			reinterpret_cast<ThiscallFloatFn>(COORDINATED_REBUILD)(owner, nullptr, releaseEpoch);
			uintptr_t component = 0;
			uint8_t presentationActive = 0xFF;
			if (TryRead(reinterpret_cast<uintptr_t>(owner) + OWNER_PRESENTATION_COMPONENT, component)
				&& component != 0)
			{
				TryRead(component + PRESENTATION_ACTIVE_FLAG, presentationActive);
			}
			HeapCheckpoint("hybrid-release-done");
			LOG_INFO("(NBN NATIVE RELEASE) Hybrid release done: StartAt core + coordinated"
				<< " restart; presentationActive=" << static_cast<int>(presentationActive)
				<< "." << std::endl);
			LogNativeFreezeGroundTruth(owner, "native-release-after");
		}
		else
		{
			LOG_INFO("(NBN LAS RELEASE) Marking active native scoring notes and requesting the coordinated"
				<< " PlayerSong restart at epoch=" << std::fixed << std::setprecision(6) << releaseEpoch
				<< " (" << reason << ")." << std::endl);
			reinterpret_cast<MarkNotesFn>(MARK_ACTIVE_NOTES)(nullptr, owner);
			reinterpret_cast<ThiscallFloatFn>(COORDINATED_REBUILD)(owner, nullptr, releaseEpoch);
		}

		uintptr_t playerSong = reinterpret_cast<uintptr_t>(heldPlayerSong);
		uint32_t pendingState = 0;
		uint8_t pendingFlag = 0;
		uint8_t running = 0;
		uint8_t stopped = 0;
		TryRead(playerSong + PLAYER_SONG_PENDING_STATE, pendingState);
		TryRead(playerSong + PLAYER_SONG_PENDING_FLAG, pendingFlag);
		TryRead(playerSong + PLAYER_SONG_RUNNING, running);
		TryRead(playerSong + PLAYER_SONG_STOPPED, stopped);
		float clockPrimary = 0.0f;
		TryRead(reinterpret_cast<uintptr_t>(owner) + OWNER_CLOCK_PRIMARY, clockPrimary);
		LOG_INFO("(NBN LAS RELEASE) Post-release PlayerSong pendingState=" << pendingState
			<< " pendingFlag=" << static_cast<int>(pendingFlag)
			<< " running=" << static_cast<int>(running)
			<< " stopped=" << static_cast<int>(stopped)
			<< " ownerClock=" << std::fixed << std::setprecision(6) << clockPrimary
			<< "." << std::endl);
	}

	void PerformOwnedRelease(void* owner, const char* reason)
	{
		PerformOwnedReleaseAtEpoch(owner, heldEpoch, reason);
	}

	void FaultWithoutRelease(const std::string& reason)
	{
		void* owner = trackedOwner;
		LOG_ERROR("(NBN LAS FAULT) Recovering in place instead of disabling NBN ("
			<< reason << "); re-bootstrapping for the current section." << std::endl);
		ResetBootstrap(reason.c_str(), owner);
		trackedOwner = owner;
	}

	void ResetBootstrap(const char* reason, void* liveOwner = nullptr)
	{
		HeapCheckpoint("bootstrap-reset");
		if (OwnsNativeHold())
		{
			LOG_ERROR("(NBN LAS LIFECYCLE) Bootstrap reset while a native hold was owned (" << reason
				<< "); the hold is abandoned without further native calls because its owner is no longer current."
				<< std::endl);
			// First-flight safety: never leave the game in mode 2 across an
			// abandon. With a live owner the native exit runs; without one
			// nothing can, and the error line is the evidence to read.
			if (didFreezeModeEnter)
			{
				didFreezeModeEnter = false;
				if (liveOwner != nullptr)
				{
					LOG_ERROR("(NBN MODE) Hold abandoned while the native frozen mode was"
						<< " entered; exiting via the UnfreezeSong core on the live owner."
						<< std::endl);
					CallNativeUnfreezeSongCore(liveOwner);
				}
				else
				{
					LOG_ERROR("(NBN MODE) Hold abandoned while the native frozen mode was"
						<< " entered and no live owner is available; the game may remain"
						<< " in mode 2." << std::endl);
				}
			}
			if (liveOwner != nullptr && liveOwner == freezeFlagSetOwner)
			{
				SetNativeFreezeFlag(liveOwner, false);
			}
			else if (freezeFlagSetOwner != nullptr)
			{
				LOG_ERROR("(NBN LAS FREEZE FLAG) Tracking for a set owner+0x5E3 dropped without a"
					<< " clear write; the owner is not provably alive." << std::endl);
				freezeFlagSetOwner = nullptr;
			}
		}
		ClearSelection();
		// The section changed, so no string can still be ringing from the selection that
		// ClearSelection deliberately preserves it across.
		previousSelectedString = -1;
		trackedOwner = nullptr;
		hasLastUpdateTime = false;
		isEpochConfirmed = false;
		pickedAttacks.Clear();
		pickScanSample = 0;
		pickSampleRate = 0;
		confirmedViaGrid = false;
		hasNativeSectionRangeFailureLogged = false;
		hasPendingBoundary = false;
		pendingBoundaryBeforeRollback = 0.0f;
		pendingBoundaryAfterRollback = 0.0f;
		greyCutoff = 0.0f;
		sectionEndBoundary = 0.0f;
		hasSectionEndBoundary = false;
		epochIndex = 0;
		consumedRecords.clear();
	}

	void LogPhraseIterationSpread(void* owner, int32_t selectedIteration, float selectedTime)
	{
		struct Span
		{
			int32_t iteration = -1;
			uint32_t count = 0;
			float earliest = 0.0f;
			float latest = 0.0f;
		};
		std::vector<Span> spans;
		uint32_t unreadable = 0;

		ForEachLiveNote(owner, [&](const LiveNote& note)
		{
			if (note.record == 0) return true;
			int32_t iteration = -1;
			if (!TryRead(note.record + RECORD_PHRASE_ITERATION, iteration))
			{
				++unreadable;
				return true;
			}
			for (auto& span : spans)
			{
				if (span.iteration != iteration) continue;
				++span.count;
				if (note.recordTime < span.earliest) span.earliest = note.recordTime;
				if (note.recordTime > span.latest) span.latest = note.recordTime;
				return true;
			}
			Span span;
			span.iteration = iteration;
			span.count = 1;
			span.earliest = note.recordTime;
			span.latest = note.recordTime;
			spans.push_back(span);
			return true;
		});

		if (spans.empty()) return;
		std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b)
		{
			return a.earliest < b.earliest;
		});

		std::ostringstream summary;
		for (size_t index = 0; index < spans.size(); ++index)
		{
			if (index != 0) summary << "  ";
			summary << "it" << spans[index].iteration
				<< "=" << spans[index].count << "n"
				<< "[" << std::fixed << std::setprecision(3) << spans[index].earliest
				<< ".." << spans[index].latest << "]";
		}
		// Put the verdict first: the console reader truncates at the buffer width.
		LOG_INFO("(NBN LAS PHRASE) selected=it" << selectedIteration
			<< "@" << std::fixed << std::setprecision(3) << selectedTime
			<< " liveIterations=" << spans.size()
			<< (unreadable != 0 ? " unreadable=" + std::to_string(unreadable) : "")
			<< " | " << summary.str() << std::endl);
	}

#if defined(_DEBUG)
	// #69 diagnostic: log each live note's mask and per-note scoring state once, so tapped
	// notes (NOTE_MASK_TAP 0x4000) and the reason the selector drops them can be read straight
	// from the console. Deduped by record so a tapping run logs each note once, not every scan.
	std::unordered_set<uintptr_t> g_scan69LoggedRecords;
	void LogNoteScan69(const LiveNote& note)
	{
		if (note.record == 0) return;
		if (!g_scan69LoggedRecords.insert(note.record).second) return;
		LOG_INFO("(NBN SKIP69) record=0x" << std::hex << note.record
			<< " mask=0x" << note.mask << std::dec
			<< (note.mask & 0x4000 ? " TAP" : "")
			<< (note.mask & NOTE_MASK_CHILD ? " CHILD" : "")
			<< (note.mask & NOTE_MASK_IGNORE ? " IGNORE" : "")
			<< (note.mask & NOTE_MASK_BEND ? " BEND" : "")
			<< (note.mask & (NOTE_MASK_HAMMERON | NOTE_MASK_PULLOFF) ? " LEGATO" : "")
			<< " time=" << std::fixed << std::setprecision(3) << note.recordTime
			<< " states=" << (int)note.stateC0 << "," << (int)note.stateC1 << ","
			<< (int)note.stateC2 << "," << (int)note.stateC3 << std::endl);
	}
#endif

	bool FindEarliestEligibleNote(void* owner, LiveNote& earliest, int legatoReferenceString)
	{
		(void)legatoReferenceString;
		bool hasEarliest = false;
		ForEachLiveNote(owner, [&](const LiveNote& note)
		{
#if defined(_DEBUG)
			LogNoteScan69(note);
#endif
			if ((note.mask & NOTE_MASK_CHILD) != 0
				&& (note.mask & NOTE_MASK_BEND) == 0)
			{
				return true;
			}
			if ((note.mask & NOTE_MASK_IGNORE) != 0) return true;
			if (note.stateC0 != 0 && note.stateC1 != 0) return true;
			if (note.recordTime < greyCutoff - GREY_EPSILON) return true;
			if (hasSectionEndBoundary
				&& note.recordTime >= sectionEndBoundary - GREY_EPSILON)
			{
				return true;
			}
			if (note.stateC0 != 0 || note.stateC1 != 0 || note.stateC2 != 0 || note.stateC3 != 0) return true;
			if (consumedRecords.count(note.record) != 0) return true;
			// Double-strum guard: the just-committed chord stays ineligible across the
			// bootstrap reset its own release triggers at the section start. See
			// COMMITTED_CHORD_RESELECT_GUARD_SECONDS.
			if (note.record == lastCommittedChordRecord
				&& lastCommittedChordRecord != 0
				&& std::chrono::duration<double>(std::chrono::steady_clock::now()
					- lastCommittedChordAt).count() < COMMITTED_CHORD_RESELECT_GUARD_SECONDS)
			{
				return true;
			}
			if (!hasEarliest || note.recordTime < earliest.recordTime
				|| (note.recordTime == earliest.recordTime && note.record < earliest.record))
			{
				earliest = note;
				hasEarliest = true;
			}
			return true;
		});
		return hasEarliest;
	}

	void UpdateSelection(void* owner, float updateTime)
	{
		LiveNote earliest;
		// Fresh selection: the ringing string is the last note committed.
		bool hasEarliest = FindEarliestEligibleNote(owner, earliest, previousSelectedString);

		if (!hasEarliest)
		{
			if (selectedRecord != 0)
			{
				LOG_INFO("(NBN LAS SELECT) No eligible unresolved native record remains in the live vector."
					<< std::endl);
				ClearSelection();
			}
			return;
		}

		if (earliest.record == selectedRecord) return;

		selectedRecord = earliest.record;
		selectedRecordTime = earliest.recordTime;
		uint8_t stringIndex = 0;
		uint8_t fret = 0;
		int32_t chordId = -1;
		int32_t chordNotesId = -1;
		selectedString = TryRead(earliest.record + RECORD_STRING, stringIndex) ? stringIndex : -1;
		selectedFret = TryRead(earliest.record + RECORD_FRET, fret) ? fret : -1;
		selectedChordId = TryRead(earliest.record + RECORD_CHORD_ID, chordId) ? chordId : -1;
		selectedChordNotesId = TryRead(earliest.record + RECORD_CHORD_NOTES_ID, chordNotesId)
			? chordNotesId
			: -1;
		if (!TryRead(static_cast<uintptr_t>(ENGINE_COMPENSATION), selectedCompensation)
			|| selectedCompensation < 0.04 || selectedCompensation > 0.07)
		{
			FaultWithoutRelease("The native 0.053 engine compensation constant could not be validated");
			return;
		}
		if (isFlowUntilMissEnabled)
		{
			// Flow-until-miss: hold LATE so an on-time pick commits naturally while the
			// transport keeps running (see isFlowUntilMissEnabled). Clamp to just inside a
			// Riff Repeater section end so the last note of the section still freezes before
			// the loop restart jumps the transport past it.
			float lateBoundary = selectedRecordTime + flowLateGraceSeconds;
			if (hasSectionEndBoundary && lateBoundary > sectionEndBoundary - GREY_EPSILON)
			{
				lateBoundary = sectionEndBoundary - GREY_EPSILON;
			}
			// Never earlier than the pre-flow boundary: a grace clamped hard against a
			// nearby section end must not pull the freeze in front of the strike line.
			const float earlyBoundary = selectedRecordTime - static_cast<float>(selectedCompensation);
			selectedHoldTime = lateBoundary > earlyBoundary ? lateBoundary : earlyBoundary;
		}
		else
		{
			selectedHoldTime = selectedRecordTime - static_cast<float>(selectedCompensation);
		}
		expectedMidi = -1;
		isHoldSuppressed = false;
		gatePhase = GatePhase::Armed;
		int32_t selectedPhraseIteration = -1;
		TryRead(selectedRecord + RECORD_PHRASE_ITERATION, selectedPhraseIteration);
		LOG_INFO("(NBN LAS SELECT) epoch=" << epochIndex
			<< " record=0x" << std::hex << selectedRecord << std::dec
			<< " time=" << std::fixed << std::setprecision(6) << selectedRecordTime
			<< " string=" << selectedString << " fret=" << selectedFret
			<< " phraseIteration=" << selectedPhraseIteration
			<< " windowEntry=" << earliest.windowEntry
			// Flow phase 0 (docs/designs/nbn-flow-until-miss.md, Phase 2 REVISED): the native
			// detect window's EXIT bounds how late a flow-until-miss hold boundary may sit.
			<< " windowExit=" << earliest.windowExit
			<< " lateWindow=" << (earliest.windowExit - selectedRecordTime)
			<< " holdTime=" << selectedHoldTime
			<< " updateTime=" << updateTime << "." << std::endl);
		LogPhraseIterationSpread(owner, selectedPhraseIteration, selectedRecordTime);
		EmitCandidateEvent(owner, updateTime, earliest,
			NoteByNoteProtocol::ExpectedAttackEventKind::CandidateChanged);
	}

	bool AreOwnerClocksAtEpoch(void* owner, float epoch)
	{
		uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		float clocks[5] = {};
		bool clocksMatch = TryRead(ownerAddress + OWNER_CLOCK_PRIMARY, clocks[0])
			&& TryRead(ownerAddress + OWNER_CLOCK_SECONDARY, clocks[1])
			&& TryRead(ownerAddress + OWNER_CLOCK_RENDER, clocks[2])
			&& TryRead(ownerAddress + OWNER_CLOCK_EPOCH_LOW, clocks[3])
			&& TryRead(ownerAddress + OWNER_CLOCK_EPOCH_HIGH, clocks[4]);
		for (float clock : clocks)
		{
			if (clock != epoch) clocksMatch = false;
		}
		return clocksMatch;
	}

	bool SetAndVerifyHeldEpoch(void* owner, float epoch)
	{
		reinterpret_cast<ThiscallFloatFn>(SET_FIVE_CLOCKS)(owner, nullptr, epoch);
		return AreOwnerClocksAtEpoch(owner, epoch);
	}

	bool EstablishHold(void* owner, float updateTime, const LiveNote& selected)
	{
		uintptr_t ownerAddress = reinterpret_cast<uintptr_t>(owner);
		uintptr_t ownerVtable = 0;
		if (!TryRead(ownerAddress, ownerVtable) || ownerVtable != LAS_OWNER_VTABLE)
		{
			FaultWithoutRelease("The scoring owner does not expose the GamePlaysongLAS vtable required for the hold");
			return false;
		}
		uintptr_t slotSetClocks = 0;
		uintptr_t slotRebuild = 0;
		uintptr_t slotDecision = 0;
		if (!TryRead(ownerVtable + 0x50, slotSetClocks) || slotSetClocks != SET_FIVE_CLOCKS
			|| !TryRead(ownerVtable + 0x54, slotRebuild) || slotRebuild != COORDINATED_REBUILD
			|| !TryRead(ownerVtable + 0xF0, slotDecision) || slotDecision != NATIVE_HIT_DECISION)
		{
			FaultWithoutRelease("The live GamePlaysongLAS vtable slots do not match the proven +0x50/+0x54/+0xF0 methods");
			return false;
		}

		void* playerSong = ResolvePlayerSong(owner);
		if (playerSong == nullptr)
		{
			FaultWithoutRelease("Exactly one live GameComponentPlayerSong could not be resolved from the owner's component collection");
			return false;
		}
		uintptr_t playerSongAddress = reinterpret_cast<uintptr_t>(playerSong);
		uintptr_t playerSongVtable = 0;
		uintptr_t slotStop = 0;
		if (!TryRead(playerSongAddress, playerSongVtable) || playerSongVtable != PLAYER_SONG_VTABLE
			|| !TryRead(playerSongVtable + 0x24, slotStop) || slotStop != STOP_TMUSIC)
		{
			FaultWithoutRelease("The resolved PlayerSong vtable or its Stop_TMusic slot does not match the proven layout");
			return false;
		}
		uint32_t playerSongMode = 0;
		if (!TryRead(playerSongAddress + PLAYER_SONG_MODE, playerSongMode) || playerSongMode != 1)
		{
			FaultWithoutRelease("PlayerSong is not in the mode-1 music state required by Stop_TMusic");
			return false;
		}

		int16_t tuningOffset = 0;
		if (selectedChordId != -1)
		{
			// Chord hold (#43): the string index is the 0xFF chord sentinel and no single
			// expected pitch exists, so all pitch machinery is neutralized. Acceptance
			// lives in the hit-decision detour.
			expectedMidi = -1;
			isBendTarget = false;
			isBendChildTarget = false;
			bendAcceptMidi = -1;
			isLegatoTarget = false;
			legatoConfirmTickCount = 0;
			hasLegatoPitchDeparted = false;
			legatoRunCount = 0;
			isConfirmingLegatoRun = false;
			chordDecisionEvalCount = 0;
			hasChordDecisionLogAnchor = false;
			InitializeNdChordTones(selected.note, selectedRecord);
		}
		else
		{
			// The onset release needs the selected record's expected MIDI note, computed in
			// the DETECTOR's tuning frame - the same table+base the native matcher validates
			// against (TryReadDetectorOpenMidi) - so it lands in the frame the input onset is
			// detected in. Under a transpose (Speaker Mode) the authored global table
			// (0x1199D2C) diverges from this by the transpose amount, which is what made
			// single notes accept one fret low; reading the detector frame removes that.
			if (selectedString < 0 || selectedString > 5 || selectedFret < 0)
			{
				FaultWithoutRelease("The selected record's string/fret identity is outside the supported guitar range");
				return false;
			}
			const int inputShift = NoteByNoteRuntime::GetInputOnsetShiftSemitones();
			if (TryRead(TUNING_OFFSETS + static_cast<uintptr_t>(selectedString) * 2, tuningOffset)
				&& tuningOffset >= -12 && tuningOffset <= 12)
			{
				expectedMidi = GUITAR_STRING_BASE_MIDI[selectedString] + tuningOffset + selectedFret
					+ inputShift;
			}
			else
			{
				FaultWithoutRelease("The authored per-string tuning offset could not be read");
				return false;
			}
			// Authored template pitch (detector frame) for the native single-note accept.
			selectedNativeTone = -1;
			{
				uintptr_t singleTemplate = 0;
				ChordTemplateView singleView = {};
				if (TryRead(selected.note + 0x30, singleTemplate) && singleTemplate != 0
					&& TryRead(singleTemplate, singleView)
					&& singleView.notes[selectedString] >= 0)
				{
					selectedNativeTone = singleView.notes[selectedString];
				}
			}
			isBendTarget = (selected.mask & NOTE_MASK_BEND) != 0;
			isBendChildTarget = isBendTarget && (selected.mask & NOTE_MASK_CHILD) != 0;
			ResolveBendAcceptance(selectedRecord);
			isLegatoTarget = (selected.mask & (NOTE_MASK_HAMMERON | NOTE_MASK_PULLOFF | NOTE_MASK_TAP)) != 0
				&& previousSelectedString == selectedString;
			legatoConfirmTickCount = 0;
			hasLegatoPitchDeparted = false;
			BuildLegatoRun(owner, selectedRecordTime, selectedString);
		}

		if (updateTime < selectedHoldTime)
		{
			FaultWithoutRelease("The late-hold boundary is too far from the current authoritative scoring time");
			return false;
		}
		if (updateTime - selectedHoldTime > HOLD_BOUNDARY_MAX_OVERSHOOT)
		{
			isHoldSuppressed = true;
			LOG_ERROR("(NBN LAS HOLD) Hold boundary overshot by "
				<< std::fixed << std::setprecision(3) << (updateTime - selectedHoldTime)
				<< "s (max " << HOLD_BOUNDARY_MAX_OVERSHOOT << ") for record=0x"
				<< std::hex << selectedRecord << std::dec
				<< "; the record plays through natively instead of faulting."
				<< std::endl);
			return false;
		}
		float epoch = selectedHoldTime;

		uint8_t childFlag = 0;
		TryRead(ownerAddress + OWNER_CHILD_FLAG, childFlag);
		uint8_t runningBefore = 0;
		uint8_t stoppedBefore = 0;
		TryRead(playerSongAddress + PLAYER_SONG_RUNNING, runningBefore);
		TryRead(playerSongAddress + PLAYER_SONG_STOPPED, stoppedBefore);

		LOG_INFO("(NBN LAS HOLD) Requesting the lesson-derived hold: record=0x" << std::hex << selectedRecord
			<< " playerSong=0x" << playerSongAddress << std::dec
			<< " string=" << selectedString << " fret=" << selectedFret
			<< " updateTime=" << std::fixed << std::setprecision(6) << updateTime
			<< " targetHoldTime=" << selectedHoldTime
			<< " heldEpoch=" << epoch
			<< " compensation=" << selectedCompensation
			<< " childFlag=" << static_cast<int>(childFlag)
			<< " runningBefore=" << static_cast<int>(runningBefore)
			<< " stoppedBefore=" << static_cast<int>(stoppedBefore)
			<< "." << std::endl);

		// Shipped lesson order: start the freeze-note presentation, stop the
		// music/publication source, then pin the owner clocks.
		//
		// Chord holds (#43): the freeze-note prompt machinery is built around a single
		// note's NoteVfx and cannot arm for a chord record (string sentinel 0xFF) - the
		// first experiment faulted here on every chord. The prompt is presentation, not
		// transport: a chord hold proceeds without it, and the freeze itself comes from
		// Stop_TMusic and the pinned owner clocks below.
		HeapCheckpoint("hold-request-entry");
		if (!ArmSelectedNativePrompt(selected))
		{
			if (selectedChordId != -1)
			{
				uintptr_t wrapper = 0;
				uintptr_t controller = 0;
				uintptr_t vtable = 0;
				TryRead(selected.note + NOTE_VFX_WRAPPER, wrapper);
				if (wrapper != 0) TryRead(wrapper, controller);
				if (controller != 0) TryRead(controller, vtable);
				LOG_INFO("(NBN LAS CHORD) The single-note prompt fork does not arm for a"
					<< " chord record; holding without the freeze-note presentation."
					<< " wrapper=0x" << std::hex << wrapper
					<< " controller=0x" << controller
					<< " vtable=0x" << vtable << std::dec << std::endl);
			}
			else
			{
				FaultWithoutRelease("The selected native NoteVfx controller could not arm its validated prompt fork");
				return false;
			}
		}
		if (selectedChordId != -1)
		{
			LOG_INFO("(NBN LAS CHORD) Skipping Play_FreezeNoteTrack for the chord hold"
				<< " so the chord panel survives on the unswept track." << std::endl);
			ShowNativeChordPanel(owner, selectedChordId);
		}
		else
		{
			HeapCheckpoint("hold-prompt-armed");
			PlayFreezeNoteTrack();
		}
		HeapCheckpoint("hold-prompt-done");
		reinterpret_cast<ThiscallVoidFn>(STOP_TMUSIC)(playerSong, nullptr);
		HeapCheckpoint("hold-stop-tmusic");

		uint8_t runningAfter = 0xFF;
		uint8_t stoppedAfter = 0xFF;
		TryRead(playerSongAddress + PLAYER_SONG_RUNNING, runningAfter);
		TryRead(playerSongAddress + PLAYER_SONG_STOPPED, stoppedAfter);
		if (runningAfter != 0 || stoppedAfter != 1)
		{
			FaultWithoutRelease(
				"Stop_TMusic did not commit the proven +0xD9=0/+0xDA=1 latched stop state");
			return false;
		}

		if (!SetAndVerifyHeldEpoch(owner, epoch))
		{
			heldEpoch = epoch;
			heldPlayerSong = playerSong;
			PerformOwnedRelease(owner, "the five-clock hold readback failed, so the stopped PlayerSong is released");
			FaultWithoutRelease("The owner's five clocks did not all commit to the held epoch");
			return false;
		}

		HeapCheckpoint("hold-five-clocks");
		heldEpoch = epoch;
		heldPlayerSong = playerSong;
		holdTickCount = 0;
		renderFramesWhileHeld = 0;
		gatePhase = GatePhase::Holding;
		inputReleaseTickCount = 0;
		holdProgressAnchor = std::chrono::steady_clock::now();
		mlConfirmationState.Reset(NoteByNoteRuntime::GetMlAudioSampleIndex());

		{
			const ULONGLONG nowTick = GetTickCount64();
			// Flow phase 0 (docs/designs/nbn-flow-until-miss.md, Phase 2 REVISED): how long
			// before this freeze did the player last attack? A spike inside the last ~150 ms
			// means "the player was on time and the hold boundary beat the native commit";
			// that count is what moving the boundary to recordTime + grace will remove.
			// Only meaningful once the spike sampler also runs in the Armed phase (see
			// SampleArmedPhaseSpike); logged unconditionally so the blind case is visible.
			LOG_INFO("(NBN FLOW TIMING) hold established "
				<< (g_lastAttackSpikeTick != 0
					? static_cast<long long>(nowTick - g_lastAttackSpikeTick) : -1LL)
				<< " ms after the last attack spike (-1 = none seen this session)"
				<< " recordTime=" << std::fixed << std::setprecision(3) << selectedRecordTime
				<< " heldEpoch=" << heldEpoch << "." << std::endl);
			if (g_lastAttackSpikeTick != 0
				&& nowTick - g_lastAttackSpikeTick <= 700)
			{
				reattackWindowTicks = REATTACK_WINDOW_TICKS;
				reattackStreak = 0;
				spikeRecencyTicks = REATTACK_WINDOW_TICKS;
				LOG_INFO("(NBN LAS INPUT) Hold established "
					<< (nowTick - g_lastAttackSpikeTick)
					<< "ms after an attack spike: opening the raw-confirmation window so"
					<< " a pick played through the transition is judged on its sustain"
					<< " instead of discarded." << std::endl);
			}
		}
		hasHoldProgressAnchor = true;
		lastStuckWarnHeldSeconds = 0.0;
		transportFrozenAt = holdProgressAnchor;
		hasTransportFrozenAt = true;
		sawSpikeDuringHold = false;
		sawLevelSpikeDuringHold = false;
		// Stamp the native onset-scan window origin at this latch (the live ring timestamp).
		hasHoldLatchRingTime = TryReadCurrentRingTime(holdLatchRingTime);
		// The ND sounding streak is per-hold; ring carryover is handled natively
		// by gating all ND credit on fresh-attack evidence since this latch.
		hasNdSoundingStreak = false;
		reattackWindowTicks = 0;
		reattackStreak = 0;
		spikeRecencyTicks = 0;
		ResetChordOnsetEvidenceAnchor();

		// Prime the onset detector's edge-latch so a still-ringing previous note cannot
		// release this hold; only a new pluck after this point reports. A drained
		// onset that is distinguishable fresh playing is kept, not discarded.
		int primedOnset = QueryNativeOnsetNote();
		StashPrimedOnset(primedOnset);
		HeapCheckpoint("hold-onset-primed");

		SetNativeFreezeFlag(owner, true);
		HeapCheckpoint("hold-freeze-flag");
		// Step 5 first flight: one armed hold enters the game's own frozen
		// MODE on top of our mechanical freeze. One-shot; the release exits.
		if (isFreezeModeTestArmed)
		{
			isFreezeModeTestArmed = false;
			didFreezeModeEnter = true;
			LogNativeFreezeGroundTruth(owner, "mode-freeze-before");
			uint8_t frozenSongByte = 0xFF;
			TryRead(reinterpret_cast<uintptr_t>(owner) + 0x5E2, frozenSongByte);
			TryWriteGameByte(reinterpret_cast<uintptr_t>(owner) + 0x5E2, 0);
			// Round-2 crash (dump 21:02, AV in memcpy from 0xE0000000 under
			// 0x407840): the tag std::string at owner+0x5C8 is ALSO only
			// initialized by the frozen-object constructor, so the exit's
			// ResumeFromTag unwind read garbage pointers. Initialize it to a
			// valid EMPTY inline string exactly as the constructor does:
			// end (+0x5D8) -> buffer (+0x5C8), inline marker (+0x5DC) ->
			// the end field, first buffer byte NUL.
			{
				const uintptr_t tagBase = reinterpret_cast<uintptr_t>(owner) + OWNER_FREEZE_TAG_STRING;
				const uint32_t bufferAddress = static_cast<uint32_t>(tagBase);
				const uint32_t endFieldAddress = static_cast<uint32_t>(
					reinterpret_cast<uintptr_t>(owner) + OWNER_FREEZE_TAG_BEGIN);
				TryWriteGameBytes(reinterpret_cast<uintptr_t>(owner) + OWNER_FREEZE_TAG_BEGIN,
					&bufferAddress, sizeof(bufferAddress));
				TryWriteGameBytes(reinterpret_cast<uintptr_t>(owner) + OWNER_FREEZE_TAG_END,
					&endFieldAddress, sizeof(endFieldAddress));
				TryWriteGameByte(tagBase, 0);
			}
			LOG_INFO("(NBN MODE) Entering the native frozen mode for this hold:"
				<< " owner+0x5E2 was 0x" << std::hex << static_cast<int>(frozenSongByte)
				<< std::dec << " (zeroed), tag string initialized empty;"
				<< " FreezeSong core (mode call 2,0)." << std::endl);
			CallNativeFreezeSongCore(owner);
			LogNativeFreezeGroundTruth(owner, "mode-freeze-after");
		}
		static bool didMeasureOwner = false;
		if (!didMeasureOwner)
		{
			didMeasureOwner = true;
			SIZE_T ownerSize = 0;
			HANDLE owningHeap = nullptr;
			HANDLE heaps[64] = {};
			const DWORD heapCount = GetProcessHeaps(64, heaps);
			const DWORD checked = heapCount < 64 ? heapCount : 64;
			for (DWORD index = 0; index < checked; ++index)
			{
				const SIZE_T size = TryQueryHeapBlockSize(heaps[index], owner);
				if (size != 0)
				{
					ownerSize = size;
					owningHeap = heaps[index];
					break;
				}
			}
			LOG_INFO("(NBN LAS OWNER SIZE) owner=0x" << std::hex
				<< reinterpret_cast<uintptr_t>(owner)
				<< " heap=0x" << reinterpret_cast<uintptr_t>(owningHeap) << std::dec
				<< " allocationSize=" << ownerSize
				<< " freezeFamilyNeeds=" << (OWNER_FREEZE_START_TIME + 8)
				<< (ownerSize != 0 && ownerSize < OWNER_FREEZE_START_TIME + 8
					? " OUT-OF-BOUNDS: every freeze-family write smashed the heap"
					: ownerSize == 0 ? " (size unresolved - not a direct heap block?)"
					: " (fields fit inside the allocation)")
				<< std::endl);
		}
		LogNativeFreezeGroundTruth(owner, "hold-established");
		HeapCheckpoint("hold-established");
		LOG_INFO("(NBN LAS HOLD) Established. All five owner clocks hold " << std::fixed
			<< std::setprecision(6) << epoch
			<< "; expectedMidi=" << expectedMidi
			<< " (string " << selectedString << " open "
			<< (expectedMidi >= 0 ? expectedMidi - selectedFret : -1)
			<< " fret " << selectedFret << "), primedOnset=" << primedOnset
			<< "; authoredTemplateTone=" << selectedNativeTone
			<< " inputShift=" << NoteByNoteRuntime::GetInputOnsetShiftSemitones()
			<< ". Scoring and the native onset detector continue at the frozen time."
			<< std::endl);
		EmitCandidateEvent(owner, updateTime, selected,
			NoteByNoteProtocol::ExpectedAttackEventKind::HoldEstablished);
		return true;
	}

	DenseChainResult TryAdvanceDenseChain(void* owner, float /*updateTime*/)
	{
		LiveNote next;
		// Successor search: the hold is still owned, so the ringing string is the selected
		// one. A legato continuation of the held note is sounded by it and must not become
		// a target of its own.
		if (!FindEarliestEligibleNote(owner, next, selectedString))
		{
			return DenseChainResult::NotRequired;
		}

		int32_t nextChordId = -1;
		if (!TryRead(next.record + RECORD_CHORD_ID, nextChordId))
		{
			PerformOwnedRelease(owner, "the dense successor's chord identity was unreadable");
			FaultWithoutRelease("The dense successor's chord identity was unreadable");
			return DenseChainResult::Faulted;
		}
		const bool isChordSuccessor = nextChordId != -1;
		if (isChordSuccessor && !areChordHoldsEnabled) return DenseChainResult::NotRequired;
		if (isChordSuccessor && !areRepeatStrumHoldsEnabled
			&& (next.mask & 0x80000000u) == 0)
		{
			return DenseChainResult::NotRequired;
		}

		float nextHoldTime = next.recordTime - static_cast<float>(selectedCompensation);
		float spacing = nextHoldTime - heldEpoch;
		const float denseWindow = isChordSuccessor
			? CHORD_DENSE_WINDOW_SECONDS
			: PLAYER_SONG_RESTART_SECONDS;
		if (spacing <= HELD_TIME_EPSILON || spacing > denseWindow)
		{
			return DenseChainResult::NotRequired;
		}

		uint8_t nextString = 0;
		uint8_t nextFret = 0;
		if (!TryRead(next.record + RECORD_STRING, nextString)
			|| !TryRead(next.record + RECORD_FRET, nextFret)
			|| (!isChordSuccessor && nextString > 5))
		{
			PerformOwnedRelease(owner, "the dense successor's string/fret identity was invalid");
			FaultWithoutRelease("The dense successor's string/fret identity was invalid");
			return DenseChainResult::Faulted;
		}

		// The successor's expected pitch is the AUTHORED tuning + fret + input shift, exactly like
		// the single-note hold above (authored guitar frame, NOT the detector table). A chord
		// successor has no single pitch (-1).
		int denseNoteMidi = -1;
		if (!isChordSuccessor)
		{
			const int inputShift = NoteByNoteRuntime::GetInputOnsetShiftSemitones();
			int16_t tuningOffset = 0;
			// Detector frame (Speaker/Off, inputShift==0): open-string MIDI from state+0x134C +
			// fret, same restore as the single-note hold above. Authored path is the fallback / Drop
			// Pedal branch.
			// Player's physical (E-standard) frame, same as the single-note hold above.
			if (TryRead(TUNING_OFFSETS + static_cast<uintptr_t>(nextString) * 2, tuningOffset)
				&& tuningOffset >= -12 && tuningOffset <= 12)
			{
				denseNoteMidi = GUITAR_STRING_BASE_MIDI[nextString] + tuningOffset + nextFret
					+ inputShift;
			}
			else
			{
				PerformOwnedRelease(owner, "the dense successor's tuning offset was invalid");
				FaultWithoutRelease("The dense successor's tuning offset was invalid");
				return DenseChainResult::Faulted;
			}
		}

		denseSuccessor.record = next.record;
		denseSuccessor.note = next.note;
		denseSuccessor.recordTime = next.recordTime;
		denseSuccessor.holdTime = nextHoldTime;
		denseSuccessor.stringIndex = nextString;
		denseSuccessor.fret = nextFret;
		denseSuccessor.chordId = nextChordId;
		int32_t nextChordNotesId = -1;
		if (!TryRead(next.record + RECORD_CHORD_NOTES_ID, nextChordNotesId))
		{
			PerformOwnedRelease(owner, "the dense successor's chord-notes identity was unreadable");
			FaultWithoutRelease("The dense successor's chord-notes identity was unreadable");
			return DenseChainResult::Faulted;
		}
		denseSuccessor.chordNotesId = nextChordNotesId;
		// A chord has no single expected pitch; -1 routes the successor into the
		// same evaluated-decision acceptance every plain chord hold uses.
		denseSuccessor.expectedMidi = denseNoteMidi;
		denseSuccessor.isBend = (next.mask & NOTE_MASK_BEND) != 0;
		denseSuccessor.isBendChild = denseSuccessor.isBend
			&& (next.mask & NOTE_MASK_CHILD) != 0;
		denseSuccessor.isLegato = (next.mask & (NOTE_MASK_HAMMERON | NOTE_MASK_PULLOFF | NOTE_MASK_TAP)) != 0;

		PerformOwnedReleaseAtEpoch(owner, nextHoldTime,
			"the committed dense note is advancing through a coordinated PlayerSong/fretboard rebuild");
		if (!AreOwnerClocksAtEpoch(owner, nextHoldTime))
		{
			heldEpoch = nextHoldTime;
			PerformOwnedRelease(owner, "the dense coordinated rebuild failed its five-clock readback");
			FaultWithoutRelease("The dense coordinated rebuild did not set all five owner clocks to the successor epoch");
			return DenseChainResult::Faulted;
		}

		heldEpoch = nextHoldTime;
		holdTickCount = 0;
		postReleaseTickCount = 0;
		commitTickCount = 0;
		wasCommitOverrideLogged = false;
		renderFramesWhileHeld = 0;
		gatePhase = GatePhase::DenseRebuildPending;
		inputReleaseTickCount = 0;
		denseRebuildQueuedAt = std::chrono::steady_clock::now();   // phase-1 flow instrumentation
		hasDenseRebuildTiming = true;

		int currentMidi = QueryNativeLoudestPlayedNote();
		LOG_INFO("(NBN LAS CHAIN) Queued the dense successor through the coordinated PlayerSong/fretboard rebuild:"
			<< " record=0x" << std::hex << denseSuccessor.record << std::dec
			<< " string=" << denseSuccessor.stringIndex << " fret=" << denseSuccessor.fret
			<< " authoredTime=" << std::fixed << std::setprecision(6) << denseSuccessor.recordTime
			<< " requestedEpoch=" << denseSuccessor.holdTime
			<< " spacing=" << spacing
			<< " restartBoundary=" << PLAYER_SONG_RESTART_SECONDS
			<< " expectedMidi=" << denseSuccessor.expectedMidi
			<< " currentMidi=" << currentMidi
			<< ". Hit decisions remain blocked until the native play packet refreshes the visible fretboard target"
			<< " and Stop_TMusic is re-latched at the same epoch."
			<< std::endl);
		return DenseChainResult::Advanced;
	}

	// A bend sweeps upward from its fretted pitch, and NDGetOnsetNote reports each
	// crossed semitone as a distinct event. Requiring the unbent fundamental means the
	// only acceptable pitch exists for a few tens of milliseconds before the bend
	// removes it, which at a degraded tick rate is routinely missed entirely. Since
	// Rocksmith itself grades a bend on reaching the target pitch, the bent pitches are
	// correct answers, not tolerance.
	// Resolves the exact pitch a bend has to reach, from the chart's own bend amount.
	// Leaves bendAcceptMidi at -1 when the value is missing or outside a musical range, so
	// acceptance falls back to the previous tolerance rather than becoming impossible.
	void ResolveBendAcceptance(uintptr_t record)
	{
		bendAcceptMidi = -1;
		if (!isBendTarget || record == 0 || expectedMidi < 0) return;

		float semitones = 0.0f;
		if (!TryRead(record + RECORD_BEND_AMOUNT, semitones)
			|| !std::isfinite(semitones)
			|| semitones < BEND_AMOUNT_MIN_SEMITONES
			|| semitones > BEND_AMOUNT_MAX_SEMITONES)
		{
			LOG_INFO("(NBN LAS BEND) No usable bend amount at record+0x40 (read "
				<< std::fixed << std::setprecision(3) << semitones
				<< "); falling back to accepting expected+1 through expected+"
				<< MAX_BEND_SEMITONES << "." << std::endl);
			return;
		}

		bendAcceptMidi = expectedMidi + static_cast<int>(std::lround(semitones));
		LOG_INFO("(NBN LAS BEND) Chart bend amount " << std::fixed << std::setprecision(3)
			<< semitones << " semitones, so this bend is accepted only at MIDI "
			<< bendAcceptMidi << " rather than anywhere from " << (expectedMidi + 1)
			<< " to " << (expectedMidi + MAX_BEND_SEMITONES)
			<< ". Crossing the window mid-bend no longer completes it." << std::endl);
	}

	bool IsAcceptedOnset(int onset)
	{
		if (onset < 0 || expectedMidi < 0) return false;
		if (!isBendTarget && onset != expectedMidi) return false;
		if (IsPlainPickedTarget())
		{
			return acceptedPickRecord == selectedRecord && acceptedPick.confirmedMidi == expectedMidi;
		}
		if (onset == expectedMidi) return true;
		if (!isBendTarget) return false;
		// A bend with a known amount must reach its actual pitch. Accepting the whole
		// window let a bend finish part-way through, and on this chart the top of the
		// window is also the next note's pitch, which then could not be played.
		if (bendAcceptMidi >= 0) return onset == bendAcceptMidi;
		return onset > expectedMidi && onset <= expectedMidi + MAX_BEND_SEMITONES;
	}

	// What a raw-path acceptance counts as "the player picked this target". For a
	// plain note that is the exact pitch. For a bend it is the WHOLE bend window:
	// picking into a bend routinely never sounds the base at all (the 142.3s trace:
	// pick spike at quality 100 reading 70 then 71 for a 69->71 bend, base never
	// heard, seven seconds stuck). Any pitch from base to bent top proves the pick;
	// the bend confirmation still requires the top before the note commits.
	bool MatchesPickPitch(int sounding)
	{
		if (sounding < 0 || expectedMidi < 0) return false;
		if (sounding == expectedMidi) return true;
		if (!isBendTarget || sounding < expectedMidi) return false;
		const int top = bendAcceptMidi >= 0
			? bendAcceptMidi
			: expectedMidi + MAX_BEND_SEMITONES;
		return sounding <= top;
	}

	void StashPrimedOnset(int primedOnset)
	{
		pendingPrimedOnset = -1;
		if (primedOnset < 0) return;
		// previousExpectedMidi is bend-adjusted (the BENT pitch), and a released bend
		// sweeps DOWN from there toward its base; it cannot ring above the bent
		// pitch. The old upward range [previous, previous+3] double-counted the bend
		// and swallowed a genuine next note sitting just above it (the 124.58s
		// trace: picks of 74 discarded as carry-over of a bend already at 71).
		const bool mayBeCarriedBend = previousWasBend
			&& primedOnset >= previousExpectedMidi - MAX_BEND_SEMITONES
			&& primedOnset <= previousExpectedMidi;
		if (primedOnset == previousExpectedMidi || mayBeCarriedBend) return;
		if (!IsAcceptedOnset(primedOnset)) return;
		pendingPrimedOnset = primedOnset;
		LOG_INFO("(NBN LAS INPUT) Primed onset " << primedOnset
			<< " matches the new target and cannot be the previous note ("
			<< previousExpectedMidi << "); keeping the played-ahead pick for the"
			<< " first holding tick instead of discarding it." << std::endl);
	}


	void BuildVisualGroup(void* owner, uintptr_t groupRecord, float groupTime, int groupString)
	{
		(void)owner; (void)groupRecord; (void)groupTime; (void)groupString;
		visualGroupCount = 0;
	}

	// Collects the legato continuations of the selected note as expected pitches, in order.
	//
	// The run is derived from the chart alone: notes strictly later in time, on the same
	// string, each flagged hammer-on or pull-off, taken until the first note that breaks
	// either condition. Nothing assumes a link field or a fixed length, so green 15 to 13
	// to 12 collects as three, and a lone note collects as none.
	// Puts the bend at the front of the gesture and starts confirming it from the continuous
	// pitch tracker.
	//
	// The bend is the first element whether or not the chart continues it with legato notes.
	// Previously this only applied when the run was empty, so a bend that also began a legato
	// run fell through to the run branch and bendAcceptMidi was discarded: the bend requirement
	// was dropped silently and only the continuation pitches were ever checked. A live capture
	// showed exactly that, two bend amounts resolved and no bend confirmation running at all.
	//
	// Shared by the two ways a bend gesture starts. A bend parent starts when its pick is
	// accepted. A bend child has no pick to accept, so it starts as soon as the hold is
	// established.
	void BeginBendConfirmation()
	{
		if (legatoRunCount < MAX_LEGATO_RUN)
		{
			// Push the existing continuations back one slot and put the bend in front of
			// them, so the gesture is confirmed in the order it is played.
			for (uint32_t index = legatoRunCount; index > 0; --index)
			{
				legatoRunMidi[index] = legatoRunMidi[index - 1];
				legatoRunIsBend[index] = legatoRunIsBend[index - 1];
			}
			++legatoRunCount;
		}
		else
		{
			// A full run is not a reason to drop the bend, so the last continuation gives way
			// to it rather than the bend being lost.
			for (uint32_t index = legatoRunCount - 1; index > 0; --index)
			{
				legatoRunMidi[index] = legatoRunMidi[index - 1];
				legatoRunIsBend[index] = legatoRunIsBend[index - 1];
			}
		}
		legatoRunMidi[0] = bendAcceptMidi;
		legatoRunIsBend[0] = true;
		isConfirmingLegatoRun = true;
		isBendRunConfirmation = true;
		wasBendTrackerPitchLogged = false;
		isRawBendAcceptArmed = false;
		rawBendAcceptStreak = 0;
		ndBendSightingStreak = 0;
		// So the first fallback of a new bend reports immediately rather than inheriting the
		// previous bend's throttle.
		hasTrackerSampleAnchor = false;
		legatoRunIndex = 0;
		legatoConfirmTickCount = 0;
		holdProgressAnchor = std::chrono::steady_clock::now();
		mlConfirmationState.Reset(NoteByNoteRuntime::GetMlAudioSampleIndex());
	}

	void BuildLegatoRun(void* owner, float runTime, int runString)
	{
		(void)owner; (void)runTime; (void)runString;
		legatoRunCount = 0;
		for (auto& isBend : legatoRunIsBend) isBend = false;
		legatoRunIndex = 0;
		isConfirmingLegatoRun = false;
		isBendRunConfirmation = false;
		wasBendTrackerPitchLogged = false;
		isRawBendAcceptArmed = false;
		rawBendAcceptStreak = 0;
		ndBendSightingStreak = 0;
		hasBendApproachBeenObserved = false;
	}

	void AdoptDenseSuccessor()
	{
		previousExpectedMidi = (isBendTarget && bendAcceptMidi >= 0)
			? bendAcceptMidi
			: expectedMidi;
		previousSelectedString = selectedString;
		previousWasBend = isBendTarget;
		isBendTarget = denseSuccessor.isBend;
		isBendChildTarget = denseSuccessor.isBendChild;
		isLegatoTarget = denseSuccessor.isLegato
			&& denseSuccessor.stringIndex == previousSelectedString;
		legatoConfirmTickCount = 0;
		hasLegatoPitchDeparted = false;
		selectedRecord = denseSuccessor.record;
		selectedRecordTime = denseSuccessor.recordTime;
		selectedString = denseSuccessor.stringIndex;
		selectedFret = denseSuccessor.fret;
		selectedChordId = denseSuccessor.chordId;
		selectedChordNotesId = denseSuccessor.chordNotesId;
		selectedHoldTime = denseSuccessor.holdTime;
		expectedMidi = denseSuccessor.expectedMidi;
		// Re-latch the ND chord tones for the adopted record, mirroring UpdateSelection. The
		// dense-advance path reassigns selectedRecord without this, leaving ndChordForRecord on
		// the previous record so TickNdChordAcceptance's record guard rejects the new chord
		// every tick and it never commits though its tones sound at full strength.
		if (selectedChordId != -1)
		{
			InitializeNdChordTones(denseSuccessor.note, selectedRecord);
		}
		// After both the record and the expected pitch are in place, since the accepted
		// bend pitch is derived from the two together.
		ResolveBendAcceptance(selectedRecord);
		isHoldSuppressed = false;
		holdTickCount = 0;
		postReleaseTickCount = 0;
		commitTickCount = 0;
		wasCommitOverrideLogged = false;
		renderFramesWhileHeld = 0;
		inputReleaseTickCount = 0;
		sawSpikeDuringHold = false;
		sawLevelSpikeDuringHold = false;
		// New window origin for the native onset scan: this successor's own latch time, so
		// the previous strum's onset cannot carry across the dense chain.
		hasHoldLatchRingTime = TryReadCurrentRingTime(holdLatchRingTime);
		// Dense single-note successors keep the legacy onset path for now (the native
		// single-note accept is wired at EstablishHold only).
		selectedNativeTone = -1;
		hasNdSoundingStreak = false;
		reattackWindowTicks = 0;
		reattackStreak = 0;
		spikeRecencyTicks = 0;
		if (selectedChordId == -1 && !isLegatoTarget)
		{
			const ULONGLONG nowTick = GetTickCount64();
			if (g_lastAttackSpikeTick != 0
				&& nowTick - g_lastAttackSpikeTick <= 700)
			{
				reattackWindowTicks = REATTACK_WINDOW_TICKS;
				spikeRecencyTicks = REATTACK_WINDOW_TICKS;
				LOG_INFO("(NBN LAS INPUT) Dense successor adopted "
					<< (nowTick - g_lastAttackSpikeTick)
					<< "ms after an attack spike: raw-confirmation window opened for a"
					<< " pick played through the transition." << std::endl);
			}
		}
		{
			const ULONGLONG nowTick = GetTickCount64();
#if defined(_DEBUG)
			if (g_flowArmedTick != 0)
			{
				LOG_INFO("(NBN FLOW) armed->adopt " << (nowTick - g_flowArmedTick)
					<< "ms (detect+accept+commit+release+rebuild for the note just scored)."
					<< std::endl);
			}
#endif
			g_flowAdoptTick = nowTick;
		}
		ResetChordOnsetEvidenceAnchor();
		gatePhase = GatePhase::DensePlayerSongStartPending;
	}

	bool RelatchDenseSuccessor(void* owner, float updateTime, const LiveNote& selected)
	{
		uintptr_t playerSongAddress = reinterpret_cast<uintptr_t>(heldPlayerSong);
		uintptr_t playerSongVtable = 0;
		uintptr_t slotStop = 0;
		if (playerSongAddress == 0
			|| !TryRead(playerSongAddress, playerSongVtable) || playerSongVtable != PLAYER_SONG_VTABLE
			|| !TryRead(playerSongVtable + 0x24, slotStop) || slotStop != STOP_TMUSIC)
		{
			FaultWithoutRelease("The dense rebuild completed without the validated PlayerSong Stop_TMusic boundary");
			return false;
		}
		if (std::fabs(updateTime - selectedHoldTime) > HOLD_BOUNDARY_MAX_OVERSHOOT)
		{
			PerformOwnedRelease(owner, "the dense rebuild resumed too far from its requested successor epoch");
			FaultWithoutRelease("The dense rebuild did not publish the successor close enough to its requested epoch");
			return false;
		}

		if (!ArmSelectedNativePrompt(selected))
		{
			if (selectedChordId != -1)
			{
				// Same shape as EstablishHold's chord branch: the prompt is a
				// single-note NoteVfx and cannot arm for the 0xFF chord sentinel.
				// Presentation only; the freeze comes from Stop_TMusic below.
				LOG_INFO("(NBN LAS CHORD) The single-note prompt fork does not arm for a"
					<< " chord record; the dense relatch holds without the freeze-note"
					<< " presentation." << std::endl);
			}
			else
			{
				PerformOwnedRelease(owner, "the dense successor's native NoteVfx prompt could not be armed");
				FaultWithoutRelease("The dense successor did not expose a validated native prompt fork");
				return false;
			}
		}
		// Same skip as EstablishHold: the freeze event sweeps the track and only an
		// armed prompt survives it, which a chord successor does not have.
		if (selectedChordId != -1)
		{
			LOG_INFO("(NBN LAS CHORD) Skipping Play_FreezeNoteTrack for the dense chord"
				<< " relatch so the chord panel survives on the unswept track." << std::endl);
			ShowNativeChordPanel(owner, selectedChordId);
		}
		else
		{
			PlayFreezeNoteTrack();
		}
		reinterpret_cast<ThiscallVoidFn>(STOP_TMUSIC)(heldPlayerSong, nullptr);
		uint8_t runningAfter = 0xFF;
		uint8_t stoppedAfter = 0xFF;
		TryRead(playerSongAddress + PLAYER_SONG_RUNNING, runningAfter);
		TryRead(playerSongAddress + PLAYER_SONG_STOPPED, stoppedAfter);
		if (runningAfter != 0 || stoppedAfter != 1)
		{
			FaultWithoutRelease(
				"The dense rebuild's Stop_TMusic relatch did not commit +0xD9=0/+0xDA=1");
			return false;
		}
		if (!SetAndVerifyHeldEpoch(owner, selectedHoldTime))
		{
			heldEpoch = selectedHoldTime;
			PerformOwnedRelease(owner, "the dense rebuild relatch failed its five-clock readback");
			FaultWithoutRelease("The dense rebuild could not re-pin all five clocks to the successor epoch");
			return false;
		}

		heldEpoch = selectedHoldTime;
		holdTickCount = 0;
		renderFramesWhileHeld = 0;
		gatePhase = GatePhase::WaitingForInputRelease;
		inputReleaseTickCount = 0;
		bendWaitPitchFloor = -1;
		bendWaitOnTargetTicks = 0;
		transportFrozenAt = std::chrono::steady_clock::now();
		hasTransportFrozenAt = true;
		// Same reattack-window reset as EstablishHold (see the comment there): a
		// spike belonging to the previous target must not open this hold's
		// same-pitch acceptance window.
		reattackWindowTicks = 0;
		reattackStreak = 0;
		spikeRecencyTicks = 0;
		if (selectedChordId == -1 && !isLegatoTarget && !isBendChildTarget
			&& expectedMidi != previousExpectedMidi)
		{
			const ULONGLONG nowSpikeTick = GetTickCount64();
			if (g_lastAttackSpikeTick != 0
				&& nowSpikeTick - g_lastAttackSpikeTick <= 700)
			{
				reattackWindowTicks = REATTACK_WINDOW_TICKS;
				spikeRecencyTicks = REATTACK_WINDOW_TICKS;
				LOG_INFO("(NBN LAS INPUT) Dense relatch kept the raw-confirmation window"
					<< " open (attack spike " << (nowSpikeTick - g_lastAttackSpikeTick)
					<< "ms ago): a pick played through the transition is judged on its"
					<< " sustain." << std::endl);
			}
		}
		{
			const ULONGLONG nowFlowTick = GetTickCount64();
#if defined(_DEBUG)
			if (g_flowAdoptTick != 0)
			{
				LOG_INFO("(NBN FLOW) adopt->armed " << (nowFlowTick - g_flowAdoptTick)
					<< "ms (native play packet + relatch)." << std::endl);
			}
#endif
			g_flowArmedTick = nowFlowTick;
		}
		sawSpikeDuringHold = false;
		sawLevelSpikeDuringHold = false;
		lastStuckWarnHeldSeconds = 0.0;
		ResetChordOnsetEvidenceAnchor();
		SetNativeFreezeFlag(owner, true);
		holdProgressAnchor = std::chrono::steady_clock::now();
		mlConfirmationState.Reset(NoteByNoteRuntime::GetMlAudioSampleIndex());
		hasHoldProgressAnchor = true;
		lastStuckWarnHeldSeconds = 0.0;
		int primedOnset = QueryNativeOnsetNote();
		int currentMidi = QueryNativeLoudestPlayedNote();
		// The successor is now the held note, so its own legato run is what the transport
		// must wait for.
		BuildLegatoRun(owner, selectedRecordTime, selectedString);
		if (isBendChildTarget && bendAcceptMidi >= 0)
		{
			gatePhase = GatePhase::Holding;
			inputReleaseTickCount = 0;
		}
		// Chord successors skip the wait too (session 8, "strum each chord
		// multiple times"): the wait clears only after consecutive SILENT polls,
		// and the previous chord's ring keeps the detector loud for seconds, so
		// the player's first strums landed before the evaluator was even armed.
		// Chord acceptance carries its own fresh-attack evidence (the spike gate
		// plus the native onset flag), so the ring cannot auto-advance a
		// same-chord successor even without the wait.
		else if (selectedChordId != -1)
		{
			gatePhase = GatePhase::Holding;
			inputReleaseTickCount = 0;
		}
		LOG_INFO("(NBN LAS CHAIN) PlayerSong/fretboard rebuild completed and Stop_TMusic was re-latched:"
			<< " record=0x" << std::hex << selectedRecord << std::dec
			<< " string=" << selectedString << " fret=" << selectedFret
			<< " updateTime=" << std::fixed << std::setprecision(6) << updateTime
			<< " heldEpoch=" << heldEpoch
			<< " expectedMidi=" << expectedMidi
			<< " primedOnset=" << primedOnset
			<< " currentMidi=" << currentMidi
			<< ". The successor will arm after the detector confirms input release."
			<< std::endl);
		if (hasDenseRebuildTiming)   // phase-1 flow instrumentation: the heavy per-note cost
		{
			const double rebuildMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - denseRebuildQueuedAt).count();
			hasDenseRebuildTiming = false;
			LOG_INFO("(NBN FLOW TIMING) dense rebuild span " << std::fixed << std::setprecision(1)
				<< rebuildMs << " ms (Queued -> completed; this is the per-note teardown+restart"
				<< " that gaps audio and blocks the highway in a dense run)." << std::endl);
		}
		EmitCandidateEvent(owner, updateTime, selected,
			NoteByNoteProtocol::ExpectedAttackEventKind::CandidateChanged);
		EmitCandidateEvent(owner, heldEpoch, selected,
			NoteByNoteProtocol::ExpectedAttackEventKind::HoldEstablished);
		denseSuccessor = {};
		return true;
	}

	void CompleteHeldCommit(void* owner, float updateTime, const LiveNote& selected, const char* reason)
	{
		if (IsPlainPickedTarget())
		{
			LOG_INFO("(NBN PICK BUFFER) Committed record=" << selectedRecord
				<< " attack=" << acceptedPick.time << " sample=" << acceptedPick.confirmedSample
				<< " remaining=" << pickedAttacks.Count() << std::endl);
		}
		acceptedPickRecord = 0;
		consumedRecords.insert(selectedRecord);
		previousSelectedString = selectedString;
		// The ringing pitch too, with the same bend adjustment AdoptDenseSuccessor makes:
		// a bend rings at its bent pitch, not its fretted one. The ordinary commit path
		// never recorded this, so the next legato target's still-ringing guard compared
		// against a stale pitch from whenever the dense path last ran.
		previousExpectedMidi = (isBendTarget && bendAcceptMidi >= 0)
			? bendAcceptMidi
			: expectedMidi;
		previousWasBend = isBendTarget;
		// The reason is logged because an advance the player did not cause is otherwise
		// indistinguishable from one they did. Every commit now names the path that
		// produced it, so an automatic advance identifies itself instead of having to be
		// inferred from surrounding lines.
		// The reason leads the line. Console capture truncates at the buffer width, and
		// putting it at the end meant the one field worth reading was the field that got
		// cut off.
		LOG_INFO("(NBN LAS WHY) " << (reason != nullptr ? reason : "unspecified")
			<< " | string=" << selectedString << " fret=" << selectedFret
			<< " ticks=" << holdTickCount << std::endl);
		LOG_INFO("(NBN LAS COMMIT) Secured the selected native hit before transport release: record=0x"
			<< std::hex << selectedRecord << std::dec
			<< " heldTicks=" << holdTickCount << "." << std::endl);
		{   // phase-1 flow instrumentation: real-time gap between consecutive commits = cadence
			const auto nowWall = std::chrono::steady_clock::now();
			if (hasLastCommitWallClock)
			{
				const double gapMs = std::chrono::duration<double, std::milli>(
					nowWall - lastCommitWallClock).count();
				LOG_INFO("(NBN FLOW TIMING) commit-to-commit " << std::fixed << std::setprecision(1)
					<< gapMs << " ms (" << (gapMs > 0.0 ? 1000.0 / gapMs : 0.0)
					<< " notes/sec cadence)." << std::endl);
			}
			lastCommitWallClock = nowWall;
			hasLastCommitWallClock = true;
		}

		DenseChainResult chainResult = TryAdvanceDenseChain(owner, updateTime);
		if (chainResult != DenseChainResult::NotRequired) return;

		PerformOwnedRelease(owner, reason);
		gatePhase = GatePhase::PostRelease;
		postReleaseTickCount = 0;
	}

	void HandleAfterUpdate(void* owner, float updateTime)
	{
		TickPickedAttackStream();
		if (gatePhase == GatePhase::Idle) return;

		LiveNote selected;
		bool isPresent = FindSelectedNote(owner, selected);

		switch (gatePhase)
		{
			case GatePhase::Armed:
			{
				if (!isPresent)
				{
					FaultWithoutRelease("The selected native record disappeared from the live vector before its hold boundary");
					return;
				}
				if (selected.stateC0 != 0 && selected.stateC1 != 0)
				{
					consumedRecords.insert(selectedRecord);
					LOG_INFO("(NBN LAS COMMIT) Natural commit before any hold: record=0x" << std::hex
						<< selectedRecord << std::dec << " states=" << static_cast<int>(selected.stateC0)
						<< static_cast<int>(selected.stateC1) << static_cast<int>(selected.stateC2)
						<< static_cast<int>(selected.stateC3) << "." << std::endl);
					ClearSelection();
					return;
				}
				if (selected.stateC3 != 0)
				{
					if (isHoldSuppressed)
					{
						consumedRecords.insert(selectedRecord);
						LOG_INFO("(NBN LAS COMMIT) Hold-suppressed record expired natively as a miss: record=0x"
							<< std::hex << selectedRecord << std::dec << "." << std::endl);
						ClearSelection();
						return;
					}
					if (selectedChordId != -1 && areChordHoldsEnabled)
					{
						LOG_INFO("(NBN LAS CHORD) Chord record 0x" << std::hex
							<< selectedRecord << std::dec << " reads stateC3="
							<< static_cast<int>(selected.stateC3)
							<< " pre-hold; chord state semantics are unmapped, so this is"
							<< " not treated as expiry." << std::endl);
					}
					else
					{
						consumedRecords.insert(selectedRecord);
						LOG_INFO("(NBN LAS COMMIT) Selected single-note record expired before its"
							<< " hold could be established (dense-run overshoot); accepting the"
							<< " native miss and advancing rather than faulting: record=0x"
							<< std::hex << selectedRecord << std::dec
							<< " expectedMidi=" << expectedMidi
							<< " holdTime=" << std::fixed << std::setprecision(6) << selectedHoldTime
							<< " updateTime=" << updateTime << "." << std::endl);
						ClearSelection();
						return;
					}
				}
				// The original update above gets first refusal. A natural on-time hit clears
				// the selection; only a still-unresolved note can reach this hold boundary.
				if (!isHoldSuppressed && updateTime >= selectedHoldTime)
				{
					if (selectedChordId != -1 && !areChordHoldsEnabled)
					{
						// The pre-#43 fallback, kept behind the toggle: the chord plays
						// natively while successors stay gated.
						isHoldSuppressed = true;
						LOG_INFO("(NBN LAS HOLD) Chord record 0x" << std::hex << selectedRecord
							<< std::dec << " (chordId " << selectedChordId
							<< ") is not held (chord holds disabled); native play continues."
							<< std::endl);
						return;
					}
					if (selectedChordId != -1 && (selected.mask & 0x80000000u) == 0
						&& !areRepeatStrumHoldsEnabled)
					{
						isHoldSuppressed = true;
						LOG_INFO("(NBN LAS CHORD) Repeat-strum chord record 0x" << std::hex
							<< selectedRecord << std::dec << " (chordId " << selectedChordId
							<< ", mask 0x" << std::hex << selected.mask << std::dec
							<< ") plays through natively; only full chords hold."
							<< std::endl);
						return;
					}
					EstablishHold(owner, updateTime, selected);
				}
				return;
			}
			case GatePhase::DenseRebuildPending:
			{
				++postReleaseTickCount;
				if (isPresent && selected.stateC0 == 0 && selected.stateC1 == 0
					&& selected.stateC2 == 0 && selected.stateC3 == 0)
				{
					LOG_INFO("(NBN LAS COMMIT) The dense rebuild reset the committed previous state;"
						<< " forcing exactly one recommit for record=0x" << std::hex
						<< selectedRecord << std::dec << "." << std::endl);
					gatePhase = GatePhase::DenseRecommitAfterRebuild;
					commitTickCount = 0;
					wasCommitOverrideLogged = false;
					return;
				}
				if (isPresent && (selected.stateC0 == 0 || selected.stateC1 == 0))
				{
					PerformOwnedRelease(owner, "the dense rebuild left the previous record partially committed");
					FaultWithoutRelease("The dense rebuild produced a partial previous-record scoring state");
					return;
				}
				if (!isPresent || postReleaseTickCount >= 6)
				{
					AdoptDenseSuccessor();
				}
				return;
			}
			case GatePhase::DenseRecommitAfterRebuild:
			{
				++commitTickCount;
				if (!isPresent)
				{
					LOG_INFO("(NBN LAS COMMIT) The dense rebuild removed the already-committed previous record;"
						<< " no recommit identity remains." << std::endl);
					AdoptDenseSuccessor();
					return;
				}
				if (selected.stateC0 != 0 && selected.stateC1 != 0)
				{
					LOG_INFO("(NBN LAS COMMIT) Dense rebuild recommit completed for record=0x" << std::hex
						<< selectedRecord << std::dec << "." << std::endl);
					AdoptDenseSuccessor();
					return;
				}
				if (commitTickCount >= 120)
				{
					PerformOwnedRelease(owner, "the dense rebuild recommit did not complete within its bounded window");
					FaultWithoutRelease("The dense rebuild could not restore the committed previous record");
				}
				return;
			}
			case GatePhase::DensePlayerSongStartPending:
			{
				++postReleaseTickCount;
				uintptr_t playerSongAddress = reinterpret_cast<uintptr_t>(heldPlayerSong);
				uint8_t running = 0xFF;
				uint8_t stopped = 0xFF;
				uint32_t pendingState = 0;
				uint8_t pendingFlag = 0;
				if (playerSongAddress == 0
					|| !TryRead(playerSongAddress + PLAYER_SONG_RUNNING, running)
					|| !TryRead(playerSongAddress + PLAYER_SONG_STOPPED, stopped)
					|| !TryRead(playerSongAddress + PLAYER_SONG_PENDING_STATE, pendingState)
					|| !TryRead(playerSongAddress + PLAYER_SONG_PENDING_FLAG, pendingFlag))
				{
					PerformOwnedRelease(owner, "the dense rebuild's PlayerSong state became unreadable");
					FaultWithoutRelease("The dense rebuild could not observe the PlayerSong play packet");
					return;
				}
				if (stopped != 0)
				{
					if (postReleaseTickCount % 60 == 0)
					{
						LOG_INFO("(NBN LAS CHAIN) Waiting for the coordinated dense play packet:"
							<< " ticks=" << postReleaseTickCount
							<< " pendingState=" << pendingState
							<< " pendingFlag=" << static_cast<int>(pendingFlag)
							<< " running=" << static_cast<int>(running)
							<< " stopped=" << static_cast<int>(stopped) << "." << std::endl);
					}
					if (postReleaseTickCount >= DENSE_REBUILD_TIMEOUT_TICKS)
					{
						PerformOwnedRelease(owner, "the dense coordinated play packet did not clear Stop_TMusic");
						FaultWithoutRelease("The dense PlayerSong/fretboard rebuild timed out");
					}
					return;
				}
				if (!isPresent)
				{
					PerformOwnedRelease(owner, "the dense successor was absent after the PlayerSong play packet completed");
					FaultWithoutRelease("The dense successor did not survive the coordinated rebuild");
					return;
				}
				const bool successorStateDirty = selectedChordId != -1
					? (selected.stateC0 != 0 || selected.stateC1 != 0)
					: (selected.stateC0 != 0 || selected.stateC1 != 0
						|| selected.stateC2 != 0 || selected.stateC3 != 0);
				if (successorStateDirty)
				{
					PerformOwnedRelease(owner, "the dense successor changed scoring state before its visible target was armed");
					FaultWithoutRelease("The dense successor was not unresolved after the coordinated rebuild");
					return;
				}
				RelatchDenseSuccessor(owner, updateTime, selected);
				return;
			}
			case GatePhase::WaitingForInputRelease:
			{
				if (!isPresent)
				{
					PerformOwnedRelease(owner,
						"the dense successor disappeared while waiting for input release");
					FaultWithoutRelease("The dense successor disappeared while waiting for input release");
					return;
				}
				if (selected.stateC0 != 0 || selected.stateC1 != 0)
				{
					PerformOwnedRelease(owner,
						"the dense successor committed while its native hit decision was blocked");
					FaultWithoutRelease("The dense successor committed before a new input was armed");
					return;
				}

				if (IsPlainPickedTarget())
				{
					if (TakeBufferedPick())
					{
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
					}
					return;
				}

				// The raw-confirmation tick, mirroring the accepted-onset shortcut
				// below and run FIRST so this tick's spike is already recorded when
				// the onset decision needs it as same-pitch evidence. Acceptance
				// requires a distinguishable successor (for a same-pitch successor
				// the previous ring itself reads the expected pitch with passing
				// gates); allowAccept=false still keeps the tracking continuous.
				{
					// Barred whenever the PREVIOUS pitch itself would satisfy the
					// matcher: then the old ring is indistinguishable from a fresh
					// pick by pitch alone (covers both the same-pitch successor and
					// a bend window that contains the previous note).
					const int reattack = TickSpikeReattackAcceptance(
						!MatchesPickPitch(previousExpectedMidi));
					if (reattack != -1 && IsAcceptedOnset(reattack))
					{
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
						LOG_INFO("(NBN LAS INPUT) Accepted raw re-attack " << reattack
							<< " during the input-release wait: the onset edge carried a"
							<< " transient pitch, but the settled raw pitch matches the"
							<< " successor and not the previous note ("
							<< previousExpectedMidi << ")." << std::endl);
						return;
					}
				}

				int discardedOnset = QueryNativeOnsetNote();
				if (discardedOnset != -1)
				{
					const bool mayBeCarriedBend = previousWasBend
						&& discardedOnset >= previousExpectedMidi - MAX_BEND_SEMITONES
						&& discardedOnset <= previousExpectedMidi;
					if (IsAcceptedOnset(discardedOnset)
						&& discardedOnset != previousExpectedMidi
						&& !mayBeCarriedBend)
					{
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
						LOG_INFO("(NBN LAS INPUT) Accepted onset " << discardedOnset
							<< " during the input-release wait: it matches the successor and not"
							<< " the previous note (" << previousExpectedMidi
							<< "), so it is fresh playing rather than carry-over." << std::endl);
						return;
					}

					// Same-pitch successor with spike evidence: pitch alone cannot tell
					// a fresh pick of the identical pitch from the previous note's ring
					// (this chart's post-bend 71 -> fretted 71, discarded at 549.5s in
					// the trace), but a decaying ring cannot SPIKE the level meter.
					// Attack energy moments before an onset matching the target is a
					// pick, whatever the previous note was.
					if (IsAcceptedOnset(discardedOnset) && spikeRecencyTicks > 0)
					{
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
						LOG_INFO("(NBN LAS INPUT) Accepted same-pitch onset "
							<< discardedOnset << " during the input-release wait: a level"
							<< " spike accompanied it, and a decaying ring cannot spike,"
							<< " so it is a fresh pick even though the pitch matches the"
							<< " previous note (" << previousExpectedMidi << ")."
							<< std::endl);
						return;
					}

					LOG_INFO("(NBN LAS INPUT) Discarded carried onset " << discardedOnset
						<< " while waiting for the previous note to be released (expected "
						<< expectedMidi << ", previous " << previousExpectedMidi << ")."
						<< std::endl);

					// An onset that is neither the previous note's pitch nor its bend
					// range cannot be carry-over: it is a fresh attack whose transient
					// pitch was garbage. Open the raw-confirmation window so the pick
					// is not lost with its consumed edge.
					if (discardedOnset != previousExpectedMidi && !mayBeCarriedBend)
					{
						reattackWindowTicks = REATTACK_WINDOW_TICKS;
						reattackStreak = 0;
					}
				}

				if (isBendTarget && !isBendChildTarget && bendAcceptMidi >= 0)
				{
					const int rawBend = QueryNativeLoudestPlayedNote();
					// The previous note, if it was a bend, sweeps DOWN from its bent pitch toward
					// its base; a raw pitch in that decay range may be that ring, not the player.
					const bool mayBeCarriedBend = previousWasBend
						&& rawBend >= previousExpectedMidi - MAX_BEND_SEMITONES
						&& rawBend <= previousExpectedMidi;
					if (rawBend >= 0 && rawBend != previousExpectedMidi && !mayBeCarriedBend)
					{
						if (bendWaitPitchFloor < 0 || rawBend < bendWaitPitchFloor)
							bendWaitPitchFloor = rawBend;
						const bool rose = rawBend > bendWaitPitchFloor;
						const bool atTarget = rawBend >= bendAcceptMidi;
						bendWaitOnTargetTicks = atTarget ? bendWaitOnTargetTicks + 1 : 0;
						if (atTarget && ((rose && bendWaitOnTargetTicks >= BEND_ACCEPT_MIN_HOLD_TICKS)
							|| bendWaitOnTargetTicks >= BEND_WAIT_ON_TARGET_TICKS))
						{
							gatePhase = GatePhase::Holding;
							inputReleaseTickCount = 0;
							LOG_INFO("(NBN LAS INPUT) Bend confirmed by raw pitch during the"
								<< " input-release wait: sounding " << rawBend
								<< " reached the bent target " << bendAcceptMidi << " (floor "
								<< bendWaitPitchFloor << ", " << (rose ? "rose" : "steady")
								<< "), distinguishable from the previous note ("
								<< previousExpectedMidi << "), so it is the player's bend rather"
								<< " than carry-over." << std::endl);
							return;
						}
					}
					else
					{
						bendWaitOnTargetTicks = 0;
					}
				}

				if (spikeRecencyTicks > 0)
				{
					int primedOnset = QueryNativeOnsetNote();
					StashPrimedOnset(primedOnset);
					gatePhase = GatePhase::Holding;
					inputReleaseTickCount = 0;
					LOG_INFO("(NBN LAS INPUT) Level spike during the input-release wait; the"
						<< " follower is armed on the fresh attack even though its onset is masked"
						<< " by the previous note's ring (a decaying ring cannot spike). primedOnset="
						<< primedOnset << "." << std::endl);
					return;
				}

				int currentMidi = QueryNativeLoudestPlayedNote();
				if (currentMidi != -1)
				{
					inputReleaseTickCount = 0;
					return;
				}

				++inputReleaseTickCount;
				if (inputReleaseTickCount < INPUT_RELEASE_CONFIRMATION_TICKS) return;

				int primedOnset = QueryNativeOnsetNote();
				StashPrimedOnset(primedOnset);
				gatePhase = GatePhase::Holding;
				LOG_INFO("(NBN LAS INPUT) Input release confirmed across "
					<< INPUT_RELEASE_CONFIRMATION_TICKS
					<< " detector polls; dense successor is now armed, primedOnset="
					<< primedOnset << "." << std::endl);
				return;
			}
			case GatePhase::Holding:
			{
				if (!isPresent)
				{
					FaultWithoutRelease("The selected native record disappeared from the live vector during the hold; the hold is abandoned without release because its scoring identity is gone");
					return;
				}
				if (selectedChordId != -1 && holdTickCount != 0 && holdTickCount % 60 == 0)
				{
					EmitCandidateEvent(owner, updateTime, selected,
						NoteByNoteProtocol::ExpectedAttackEventKind::CandidateChanged);
				}
				if (selected.stateC0 != 0 || selected.stateC1 != 0)
				{
					PerformOwnedRelease(owner,
						"the held record committed while its native hit decision was blocked");
					FaultWithoutRelease("The held record committed before an exact onset was accepted");
					return;
				}
				if (hasHoldProgressAnchor)
				{
					const double heldSeconds = std::chrono::duration<double>(
						std::chrono::steady_clock::now() - holdProgressAnchor).count();
					const double safetyBudget = selectedChordId != -1
						? CHORD_HOLD_SAFETY_RELEASE_SECONDS
						: HOLD_SAFETY_RELEASE_SECONDS;
					if (heldSeconds >= safetyBudget && !isSafetyReleaseEnabled)
					{
						// Keep the stuck state on record at every budget interval, but
						// hold on: the refusal evidence is the point.
						if (heldSeconds - lastStuckWarnHeldSeconds >= safetyBudget)
						{
							lastStuckWarnHeldSeconds = heldSeconds;
							LogDetectorGates("stuck-hold", true);
							LOG_ERROR("(NBN LAS SAFETY) The hold on record=0x" << std::hex
								<< selectedRecord << std::dec << " (string " << selectedString
								<< " fret " << selectedFret << ", expected MIDI " << expectedMidi
								<< ") has made no progress for " << std::fixed
								<< std::setprecision(1) << heldSeconds << "s. The safety release"
								<< " is disabled, so it stays held; the refusing state above is"
								<< " the evidence to read. N releases it." << std::endl);
						}
					}
					else if (heldSeconds >= safetyBudget)
					{
						// The state at the moment of failure, unthrottled: this is the one
						// sample that is always worth having.
						LogDetectorGates("safety-release", true);
						LOG_ERROR("(NBN LAS SAFETY) The hold on record=0x" << std::hex
							<< selectedRecord << std::dec << " (string " << selectedString
							<< " fret " << selectedFret << ", expected MIDI " << expectedMidi
							<< ") made no progress for " << std::fixed << std::setprecision(1)
							<< heldSeconds << "s across " << holdTickCount
							<< " ticks on this note, so it cannot be satisfied. Releasing the"
							<< " transport without accepting the note rather than leaving the"
							<< " game frozen." << std::endl);
						consumedRecords.insert(selectedRecord);
						PerformOwnedRelease(owner,
							"the hold made no progress and was released by the safety timeout");
						ClearSelection();
						return;
					}
				}

				// A bend child has no pick of its own, so it enters bend confirmation
				// immediately instead of waiting for an onset that cannot arrive.
				//
				// The parent of this record has already been picked and accepted, and the
				// player is holding one continuous gesture across both. Waiting for an onset
				// at the unbent pitch asks them to release the bend and pluck again, which
				// nothing on screen suggests and which the frozen transport gives no cue for,
				// so the hold ran to the 60-second safety release instead.
				//
				// The child is still a target rather than skipped: skipping bend children was
				// observed to make a bend accept itself the moment its parent was played. What
				// changes here is only what satisfies it. The tracker still has to see the
				// string at the bend pitch, so a child cannot pass while the bend is not being
				// held.
				// Chord hold (#43): no single expected pitch exists, so none of the pitch
				// machinery below can accept a strum. Acceptance lives in the hit-decision
				// detour, which evaluates the game's own decision while this hold lasts;
				// this branch only keeps the diagnostics alive while waiting.
				if (selectedChordId != -1)
				{
					LogDetectorGates("chord-holding", false);
					return;
				}

				if (isBendChildTarget && bendAcceptMidi >= 0 && !isConfirmingLegatoRun)
				{
					BeginBendConfirmation();
					LOG_INFO("(NBN LAS BEND) Bend child target, so no pick is required: this"
						<< " record continues the bend its parent started. Holding until the"
						<< " tracker reads " << bendAcceptMidi
						<< " (string " << selectedString << " fret " << selectedFret << ")."
						<< (legatoRunCount > 1 ? " The legato continuation(s) follow it." : "")
						<< std::endl);
					return;
				}

				// The windowed decision cannot release a long hold (onset timestamps live
				// on the advancing input-stream clock), so the release trigger is the
				// lesson's own un-windowed onset query.
				{
					CaptureOnsetSpectrum(expectedMidi);
					// While a run is being played the pick has already been accepted, so a
					// further onset is the player picking into the run rather than a new
					// gesture, and must not restart or complete it.
					int onset = -1;
					if (IsPlainPickedTarget())
					{
						if (!TakeBufferedPick()) return;
						onset = expectedMidi;
					}
					else if (!isConfirmingLegatoRun)
					{
						// Bend and legato targets. A bend needs the detected onset PITCH here so
						// the bend-band entry below can see it (the gesture is proven by the
						// continuous-tracker confirmation, not by an attack==pitch match). A
						// legato note (hammer-on/pull-off) produces NO fresh attack, so the
						// picked-note attack scan can never accept it; native routes legato
						// through its own path (0x4E9670), not ported yet, so its sounding-based
						// departure-then-lock rule stays until that lands.
						onset = QueryNativeOnsetNote();
						if (onset == -1)
						{
							const int sounding = QueryNativeLoudestPlayedNote();
							if (sounding != -1 && !MatchesPickPitch(sounding))
							{
								hasPickPitchDeparted = true;
								pickPitchConfirmTicks = 0;
							}
							else if (sounding != -1 && MatchesPickPitch(sounding)
								&& hasPickPitchDeparted)
							{
								if (++pickPitchConfirmTicks >= LEGATO_CONFIRMATION_TICKS)
								{
									onset = expectedMidi;
									LOG_INFO("(NBN LAS REATTACK) Legato/bend accepted from"
										<< " departure-then-lock: the gated query reported a"
										<< " different pitch during this hold and then held "
										<< expectedMidi << " for " << pickPitchConfirmTicks
										<< " polls, the native legato-sustain rule (0x4E9670,"
										<< " pending a faithful port)." << std::endl);
								}
							}
							else if (sounding == -1)
							{
								pickPitchConfirmTicks = 0;
							}
						}
						if (onset == -1 && isBendTarget && bendAcceptMidi >= 0
							&& g_tier0Enforcement && Tier0ConfirmsExpected(bendAcceptMidi))
						{
							if (++bendRescueStreak >= BEND_RESCUE_STREAK)
							{
								bendRescueStreak = 0;
								onset = expectedMidi;
								LOG_INFO("(NBN LAS BEND RESCUE) Bend accepted: tier-0 confirms the bend"
									<< " settled at its target " << bendAcceptMidi << " (expected "
									<< expectedMidi << ") for " << BEND_RESCUE_STREAK
									<< " ticks where the detector read wobbled." << std::endl);
							}
						}
						else if (isBendTarget)
						{
							bendRescueStreak = 0;
						}
					}
					pendingPrimedOnset = -1;
					if (onset != -1)
					{
						LOG_INFO("(NBN LAS INPUT) Native onset " << onset << " while holding; expected "
						<< expectedMidi << (isBendTarget ? " (bend, accepts up to +3)" : "")
						<< " ndStreak=" << std::fixed << std::setprecision(3)
						<< CurrentNdStreakSeconds() << "s." << std::endl);
						if (expectedMidi >= 0 && onset != expectedMidi
							&& onset >= expectedMidi - 2 && onset <= expectedMidi + 2)
						{
							char frame[192];
							if (TryDescribeCurrentAnalysisFrame(frame, sizeof(frame)))
							{
								LOG_INFO("(NBN LAS NEAR MISS) onset=" << onset << " expected="
									<< expectedMidi << " | frame " << frame << std::endl);
							}
						}
						// #52 bend-release cascade guard: a bend's decaying ring sweeps DOWN
						// through the followers' pitches and the onset detector fires on the
						// loud ring, skipping the next notes with no pluck. For the release
						// window, a follower must show attack energy (sawSpikeDuringHold, set by
						// the level spike OR a ring-frame onset stamp) - a decaying ring shows
						// none, a real repluck does. Applies to any follower, picked or bend
						// (a ring cannot legitimately START a bend either).
						if (onset >= 0 && hasLastBendCommit && !sawSpikeDuringHold
							&& std::chrono::duration<double>(
								std::chrono::steady_clock::now() - lastBendCommitAt).count()
								< BEND_RELEASE_GUARD_SECONDS)
						{
							LOG_INFO("(NBN LAS BEND) Follower onset " << onset << " rejected: within "
								<< BEND_RELEASE_GUARD_SECONDS << "s of a bend commit and no attack"
								<< " spike - the decaying release ring, not a pluck (#52 cascade"
								<< " guard)." << std::endl);
							onset = -1;
						}
						if (IsAcceptedOnset(onset) || (isBendTarget && MatchesPickPitch(onset)))
						{
							if (bendAcceptMidi >= 0)
							{
								BeginBendConfirmation();
								// The pick landed at or below the bend target - that IS the below-target
								// approach (you pick the base, then bend up), so pre-arm the raw accept.
								// BeginBendConfirmation clears the arm; without re-arming here, a bend that
								// is already at the target pitch when confirmation starts sampling never
								// sees a non-credible read to arm, so a perfect bend (raw detector reading
								// the target at high quality) never accepts. An inherited ring picked AT the
								// target (onset == bendAcceptMidi) is excluded, keeping the anti-cascade guard.
								if (onset < bendAcceptMidi)
								{
									isRawBendAcceptArmed = true;
								}
								LOG_INFO("(NBN LAS BEND) Pick accepted at " << onset
									<< (onset != expectedMidi
										? " (in the bend band above the base; the gesture"
										  " must still be proven)"
										: "")
									<< "; now holding until the bend reaches "
									<< bendAcceptMidi
									<< ". The bent pitch never arrives as an onset, so it is"
									<< " confirmed from the continuous pitch tracker."
									<< (legatoRunCount > 1
										? " The legato continuation(s) follow it."
										: "")
									<< std::endl);
								return;
							}

							// The pick is only the first note of the gesture. When the chart
							// continues it with legato notes, the transport keeps holding
							// until every one of them has been played.
							if (legatoRunCount != 0)
							{
								isConfirmingLegatoRun = true;
								legatoRunIndex = 0;
								legatoConfirmTickCount = 0;
								LOG_INFO("(NBN LAS LEGATO) Pick accepted; holding for "
									<< legatoRunCount << " legato continuation(s) before the run"
									<< " commits. Next expected pitch " << legatoRunMidi[0]
									<< "." << std::endl);
								return;
							}
							gatePhase = GatePhase::CommitBeforeRelease;
							commitTickCount = 0;
							wasCommitOverrideLogged = false;
							LOG_INFO("(NBN LAS COMMIT) Matching onset accepted; transport remains held until"
								<< " the selected native record commits." << std::endl);
							return;
						}
						else
						{
							// A rejected onset is still an attack. The transient
							// regularly reports garbage for a correct pick (the 189.9s
							// trace: onsets 79 then 55 from a real pick of 74), so it
							// opens the raw-confirmation window and the settled pitch
							// decides over the next few ticks.
							reattackWindowTicks = REATTACK_WINDOW_TICKS;
							reattackStreak = 0;
						}
					}

					// Run confirmation. Each continuation is verified by pitch, in order,
					// and only the last one commits the whole gesture.
					if (isConfirmingLegatoRun && legatoRunIndex < legatoRunCount)
					{
						const int expectedRunMidi = legatoRunMidi[legatoRunIndex];
						const int currentMidi = QueryNativeLoudestPlayedNote();

						// A bend only has to reach its pitch, because the sweep is
						// continuous and the top wobbles; a fretted legato note is exact.
						//
						// The integer query cannot judge that well. It reports whole
						// semitones, so a bend resting just under its target reads as a
						// full semitone short and only registers once it overshoots, and
						// `>=` then accepts any higher pitch from anywhere, including a
						// different string. Rocksmith's own continuous trackers report
						// fractional pitch, so prefer them and keep the integer query as
						// the fallback for when they are unreadable.
						// Judged per element, not per run: a run can mix a bend with fretted
						// legato notes and the two need different tests.
						const bool isBendElement = legatoRunIsBend[legatoRunIndex];
						bool hasReachedRunPitch = false;
						if (isBendChildTarget)
						{
							hasBendApproachBeenObserved = true;
						}
						if (isBendElement)
						{
							const float wanted = static_cast<float>(expectedRunMidi);
							float sounding = 0.0f;
							if (TryGetSoundingPitchNear(
									wanted, BEND_OVERBEND_ALLOWANCE_SEMITONES, sounding))
							{
								// The lesson band, asymmetrically widened upward (see
								// BEND_OVERBEND_ALLOWANCE_SEMITONES): reached once the pitch
								// is within sUnderbend below the target, and overshoot up to
								// the allowance counts as reached - the old sVariance ceiling
								// refused a fast bend sitting past its target until the
								// player relaxed back into the half-semitone window.
								hasReachedRunPitch =
									sounding >= (wanted - BEND_UNDERBEND_SEMITONES)
									&& sounding < (wanted + BEND_OVERBEND_ALLOWANCE_SEMITONES);
								if (hasReachedRunPitch && !wasBendTrackerPitchLogged)
								{
									wasBendTrackerPitchLogged = true;
									LOG_INFO("(NBN LAS BEND) Tracked pitch " << std::fixed
										<< std::setprecision(3) << sounding
										<< " reached the target " << expectedRunMidi
										<< " within the widened bend band ("
										<< BEND_UNDERBEND_SEMITONES << " under, "
										<< BEND_OVERBEND_ALLOWANCE_SEMITONES << " over)."
										<< std::endl);
								}
							}
							else
							{
								// No tracker is sounding near the target. That is the
								// normal state before the bend arrives, so it is not an
								// error; fall back to the previous integer behaviour so a
								// missing tracker can never make a bend unplayable.
								// Off-target-first: the gesture must be observed BELOW the
								// accept pitch before >= can complete it, or a carried
								// in-band ring already at the target confirms a bend that
								// was never bent (see hasBendApproachBeenObserved).
								if (currentMidi >= 0 && currentMidi < expectedRunMidi)
								{
									hasBendApproachBeenObserved = true;
								}
								hasReachedRunPitch = hasBendApproachBeenObserved
									&& (currentMidi >= expectedRunMidi);

								// Second fallback, the one that carries a bend while the
								// hold is frozen (see DETECTOR_RAW_BEND_QUALITY_FLOOR for
								// the capture evidence): the ungated current-note field,
								// accepted only when armed and only on a streak.
								if (!hasReachedRunPitch)
								{
									DetectorGateSample raw;
									if (TryReadDetectorGates(raw))
									{
										const float rawPitch =
											static_cast<float>(raw.currentNote);
										const bool credible = raw.currentNote >= 0
											&& std::isfinite(raw.quality)
											&& raw.quality >= DETECTOR_RAW_BEND_QUALITY_FLOOR
											&& rawPitch >= (wanted - BEND_UNDERBEND_SEMITONES)
											// Overshoot counts as reached; see
											// BEND_OVERBEND_ALLOWANCE_SEMITONES.
											&& rawPitch < (wanted + BEND_OVERBEND_ALLOWANCE_SEMITONES);
										if (!credible)
										{
											// Only a genuine BELOW-target read is the approach:
											// (re)arm and restart the count, so an inherited
											// in-band ring can never confirm a bend that was
											// never bent (the anti-cascade guard stays).
											// ARM on ANY non-credible read - silence before the bend,
											// the approach, or a wobble at the top. This makes a fast
											// bend (silence -> bent, no clean below-target frame) still
											// arm; below-target-only arming missed those and a perfect
											// bend did not proceed. Anti-cascade kept: an inherited ring
											// already AT target is credible from tick one, never reaches
											// this branch, so it never arms.
											isRawBendAcceptArmed = true;
											ndBendSightingStreak = 0;
											float streakResetBelow = wanted - BEND_UNDERBEND_SEMITONES;
											if (bendAcceptMidi > expectedMidi && expectedMidi >= 0)
											{
												const float baseRelative = static_cast<float>(expectedMidi) + BEND_UNDERBEND_SEMITONES;
												if (baseRelative < streakResetBelow) streakResetBelow = baseRelative;
											}
											const bool belowTarget = rawPitch >= 0.0f && rawPitch < streakResetBelow;
											if (belowTarget)
											{
												rawBendAcceptStreak = 0;
											}
											// A non-below dip is the WOBBLE at the top of the bend
											// (or a momentary quality drop), not a failure: hold
											// the streak through it. Zeroing it here was the
											// green-but-no-accept - a reached bend wobbled and had
											// to restart the 3-tick count while the visualizer
											// stayed green. The below-target re-arm above still
											// resets a genuinely released bend.
										}
										else
										{
											// A credible read is the bend AT its target - the same
											// on-pitch, high-quality signal that turns the on-screen
											// bend meter GREEN. Arm on it directly (not only after a
											// clean below-target approach) so a fast or HALF bend,
											// which flashes the target only briefly, still confirms.
											isRawBendAcceptArmed = true;
											++rawBendAcceptStreak;
											// A decaying bend-release ring is excluded by the #52
											// guard (within the release window with no attack
											// spike), so trusting a strong green cannot cascade a
											// follower.
											const bool inBendReleaseGuard = hasLastBendCommit
												&& !sawSpikeDuringHold
												&& std::chrono::duration<double>(
													std::chrono::steady_clock::now()
														- lastBendCommitAt).count()
													< BEND_RELEASE_GUARD_SECONDS;
											const bool strongGreen =
												raw.quality >= DETECTOR_RAW_BEND_STRONG_QUALITY
												&& !inBendReleaseGuard
												// Require the strong read to actually be HELD, not just
												// flash for one tick (the "activated without the full
												// bend" case). rawBendAcceptStreak was ++'d above.
												&& rawBendAcceptStreak >= BEND_ACCEPT_MIN_HOLD_TICKS;
											if (strongGreen
												|| rawBendAcceptStreak >= DETECTOR_RAW_BEND_STREAK_TICKS)
											{
												hasReachedRunPitch = true;
												LOG_INFO("(NBN LAS BEND) Raw detector pitch "
													<< raw.currentNote << " (quality "
													<< std::fixed << std::setprecision(0)
													<< raw.quality << ") "
													<< (strongGreen
														? "is a strong green; accepted immediately"
														: "held the target for a streak")
													<< " for target " << expectedRunMidi
													<< "; accepted from the ungated field because"
													<< " the tracker is unregistered while frozen"
													<< " and the gated queries are blind during a"
													<< " bend." << std::endl);
											}
										}
									}
								}

								// Third source, the native sounding table (the step-4 seam;
								// see ndBendSightingStreak): the engine lists the bent
								// pitch as integer MIDI once it sounds. Below-target
								// sightings arm the approach; a target-or-over sighting
								// while armed reaches, on a 2-tick streak.
								if (!hasReachedRunPitch)
								{
									for (int midi = expectedRunMidi - 2; midi < expectedRunMidi; ++midi)
									{
										if (midi >= 0 && ReadNdSoundingStrength(midi) >= 0.0f)
										{
											hasBendApproachBeenObserved = true;
											break;
										}
									}
									bool targetSighted = false;
									const int overAllowance =
										static_cast<int>(BEND_OVERBEND_ALLOWANCE_SEMITONES);
									for (int midi = expectedRunMidi;
										midi <= expectedRunMidi + overAllowance; ++midi)
									{
										if (ReadNdSoundingStrength(midi) >= 0.0f)
										{
											targetSighted = true;
											break;
										}
									}
									if (targetSighted && hasBendApproachBeenObserved)
									{
										if (++ndBendSightingStreak >= 2)
										{
											hasReachedRunPitch = true;
											LOG_INFO("(NBN LAS BEND) Native sounding table"
												<< " reached the bend target " << expectedRunMidi
												<< " (approach observed, " << ndBendSightingStreak
												<< " sighting ticks)." << std::endl);
										}
									}
									else if (!targetSighted)
									{
										ndBendSightingStreak = 0;
									}
								}

								// But never silently. The fallback is the open-ended
								// comparison the tracker was adopted to replace, so every
								// time it carries a bend, the reason the tracker declined
								// is reported. Throttled, because this runs every tick.
								const auto now = std::chrono::steady_clock::now();
								if (!hasTrackerSampleAnchor
									|| std::chrono::duration<double>(
										now - trackerSampleAnchor).count() >= 1.0)
								{
									trackerSampleAnchor = now;
									hasTrackerSampleAnchor = true;
									LogMotionTrackers("bend-fallback", expectedRunMidi);
								}
							}
						}
						else
						{
							hasReachedRunPitch = (currentMidi == expectedRunMidi);
						}
						if (isBendElement && hasReachedRunPitch)
						{
							float vetoEstMidi = -1.0f;
							float vetoEstConfidence = 0.0f;
							if (TryEstimateRawBendPitch(
									static_cast<double>(expectedMidi),
									static_cast<double>(expectedRunMidi),
									vetoEstMidi, vetoEstConfidence)
								&& vetoEstConfidence >= 40.0f
								&& vetoEstMidi < static_cast<float>(expectedRunMidi)
									- BEND_UNDERBEND_SEMITONES)
							{
								hasReachedRunPitch = false;
							}
						}
						const uint32_t requiredPolls = isBendElement
							? 1u
							: LEGATO_CONFIRMATION_TICKS;
						if (hasReachedRunPitch)
						{
							++legatoConfirmTickCount;
							if (legatoConfirmTickCount >= requiredPolls)
							{
								legatoConfirmTickCount = 0;
								++legatoRunIndex;
								// Ticket #52: a tracker-confirmed bend never consumes an
								// onset, so unlike every picked commit it leaves the dedupe
								// global holding a stale value while the string rings at the
								// BENT pitch. The next hold's onset query then reported the
								// ring as a fresh onset (live: a 3-tick auto-commit right
								// after the tracked acceptance). Seeding the dedupe with the
								// bent pitch restores the symmetry: the ring is consumed
								// exactly as a picked note's onset would have been, and a
								// same-pitch follower goes through the departure-then-lock
								// and spike paths like any repeated note.
								if (isBendElement)
								{
									TryWriteDedupeGlobal(expectedRunMidi);
									// #52: open the release-decay guard so the bend's ring,
									// sweeping down through the followers' pitches, cannot
									// auto-commit them without a pluck.
									lastBendCommitAt = std::chrono::steady_clock::now();
									hasLastBendCommit = true;
								}
								// The next element starts its own raw-acceptance arming; a
								// carried streak would let the element it belonged to leak
								// into its successor.
								isRawBendAcceptArmed = false;
								rawBendAcceptStreak = 0;
								ndBendSightingStreak = 0;
								hasBendApproachBeenObserved = false;
								// Real progress within the run, so the safety timeout
								// restarts and a long but advancing run is never cut short.
								holdProgressAnchor = std::chrono::steady_clock::now();
								mlConfirmationState.Reset(NoteByNoteRuntime::GetMlAudioSampleIndex());
								if (legatoRunIndex >= legatoRunCount)
								{
									isConfirmingLegatoRun = false;
									gatePhase = GatePhase::CommitBeforeRelease;
									commitTickCount = 0;
									wasCommitOverrideLogged = false;
									LOG_INFO("(NBN LAS LEGATO) Run complete; all " << legatoRunCount
										<< " continuation(s) played. The gesture commits as one note."
										<< std::endl);
									consumedRecords.insert(selectedRecord);
									lastCommittedChordRecord = selectedRecord;
									lastCommittedChordAt = std::chrono::steady_clock::now();
								}
								else
								{
									LOG_INFO("(NBN LAS LEGATO) Continuation " << legatoRunIndex
										<< " of " << legatoRunCount << " confirmed at pitch "
										<< expectedRunMidi << "; next expected "
										<< legatoRunMidi[legatoRunIndex] << "." << std::endl);
								}
							}
						}
						else
						{
							legatoConfirmTickCount = 0;
						}
						return;
					}

					if (isLegatoTarget && expectedMidi != previousExpectedMidi)
					{
						const int currentPitch = QueryNativeLoudestPlayedNote();
						if (currentPitch != expectedMidi)
						{
							hasLegatoPitchDeparted = true;
							legatoConfirmTickCount = 0;
						}
						else if (hasLegatoPitchDeparted)
						{
							++legatoConfirmTickCount;
							if (legatoConfirmTickCount >= LEGATO_CONFIRMATION_TICKS)
							{
								gatePhase = GatePhase::CommitBeforeRelease;
								commitTickCount = 0;
								wasCommitOverrideLogged = false;
								legatoConfirmTickCount = 0;
								LOG_INFO("(NBN LAS COMMIT) Legato pitch " << expectedMidi
									<< " confirmed from the detector's current-pitch query across "
									<< LEGATO_CONFIRMATION_TICKS
									<< " polls, after first departing from it; a hammer-on or"
									<< " pull-off produces no pick attack for the onset query to"
									<< " latch." << std::endl);
							}
						}
					}
				}
				return;
			}
			case GatePhase::CommitBeforeRelease:
			{
				++commitTickCount;
				if (!isPresent)
				{
					PerformOwnedRelease(owner,
						"the selected record disappeared before its held commit completed");
					FaultWithoutRelease("The selected record disappeared before its held commit completed");
					return;
				}
				if (selected.stateC0 != 0 && selected.stateC1 != 0)
				{
					CompleteHeldCommit(owner, updateTime, selected,
						"the selected onset was committed before transport release");
					return;
				}
				if (commitTickCount >= 120)
				{
					PerformOwnedRelease(owner,
						"the held pre-release commit did not complete within its bounded window");
					FaultWithoutRelease("The held pre-release commit did not complete within its bounded window");
				}
				return;
			}
			case GatePhase::PostRelease:
			{
				++postReleaseTickCount;
				if (isPresent && selected.stateC0 == 0 && selected.stateC1 == 0
					&& selected.stateC2 == 0 && selected.stateC3 == 0)
				{
					LOG_INFO("(NBN LAS COMMIT) The release rebuild reset the committed selected state;"
						<< " forcing exactly one native recommit for record=0x" << std::hex
						<< selectedRecord << std::dec << "." << std::endl);
					gatePhase = GatePhase::RecommitAfterRelease;
					commitTickCount = 0;
					wasCommitOverrideLogged = false;
					return;
				}
				if (postReleaseTickCount >= 6)
				{
					ClearSelection();
				}
				return;
			}
			case GatePhase::RecommitAfterRelease:
			{
				++commitTickCount;
				if (!isPresent)
				{
					LOG_INFO("(NBN LAS COMMIT) The already-committed selected record left the live vector"
						<< " after release; no post-release identity remains to recommit." << std::endl);
					ClearSelection();
					return;
				}
				if (selected.stateC0 != 0 && selected.stateC1 != 0)
				{
					LOG_INFO("(NBN LAS COMMIT) Forced recommit completed for record=0x" << std::hex
						<< selectedRecord << std::dec << "." << std::endl);
					ClearSelection();
					return;
				}
				if (commitTickCount >= 120)
				{
					FaultWithoutRelease("The forced single recommit did not complete within the bounded window");
				}
				return;
			}
			default:
				return;
		}
	}

	void __stdcall ScoringUpdateDetour(void* owner, float updateTime)
	{
		std::lock_guard<std::recursive_mutex> lock(controllerMutex);
		observedScoringUpdateTime = updateTime;

		// Native-drive phase A: the StartAt-core test call, executed HERE because
		// this is the main thread the game's own GE handlers run on (the bridge
		// pipe thread must never call into the music service). Idle-only and
		// vtable-guarded; one call per request. See CallNativeStartAtNow.
		if (isNativeSeekTestRequested)
		{
			isNativeSeekTestRequested = false;
			uintptr_t ownerVtable = 0;
			if (gatePhase == GatePhase::Idle
				&& TryRead(reinterpret_cast<uintptr_t>(owner), ownerVtable)
				&& ownerVtable == LAS_OWNER_VTABLE)
			{
				LogNativeFreezeGroundTruth(owner, "native-seek-before");
				CallNativeStartAtNow(owner);
				LogNativeFreezeGroundTruth(owner, "native-seek-after");
			}
			else
			{
				LOG_INFO("(NBN NATIVE SEEK) Test skipped: phase="
					<< DescribeGatePhase(gatePhase)
					<< " ownerVtable=0x" << std::hex << ownerVtable << std::dec
					<< " (requires Idle + GamePlaysongLAS)." << std::endl);
			}
		}

		bool isEnabled = NoteByNoteRuntime::IsNoteByNoteEnabled();

		if (isReleaseRequested)
		{
			isReleaseRequested = false;
			if (OwnsNativeHold()
				&& owner == trackedOwner)
			{
				PerformOwnedRelease(owner, "Note by Note was stopped while a native hold was owned");
			}
			ClearSelection();
		}

		if (!isEnabled)
		{
			if (trackedOwner != nullptr) ResetBootstrap("Note by Note is disabled", owner);
			originalScoringUpdate(owner, updateTime);
			return;
		}

		if (reArmRequested)
		{
			reArmRequested = false;
			ResetBootstrap("Note by Note was re-enabled; re-arming for the current section");
			trackedOwner = owner;
		}

		if (owner != trackedOwner)
		{
			ResetBootstrap("the scoring owner changed");
			trackedOwner = owner;
		}

		if (isEpochConfirmed && timelineOwner == owner && !timelineSections.empty())
		{
			uintptr_t liveContainer = 0;
			uintptr_t liveBegin = 0;
			uintptr_t liveEnd = 0;
			if (TryRead(reinterpret_cast<uintptr_t>(owner) + LAS_PHRASE_SECTION_CONTAINER, liveContainer)
				&& liveContainer != 0
				&& TryRead(liveContainer + PHRASE_SECTION_VECTOR_BEGIN, liveBegin)
				&& TryRead(liveContainer + PHRASE_SECTION_VECTOR_END, liveEnd)
				&& (liveContainer != timelineContainer
					|| liveBegin != timelineBegin
					|| liveEnd != timelineEnd))
			{
				LOG_INFO("(NBN LAS TIMELINE) The authored section grid changed under the confirmed"
					<< " controller (song reload); re-arming for the new song." << std::endl);
				ResetBootstrap("the song's authored section grid changed");
				trackedOwner = owner;
			}
		}

		if (!isEpochConfirmed)
		{
			TryLatchSectionFromGrid(owner);
		}
		else if (HasSelectionMovedToNewSection(owner))
		{
			const float previousStart = greyCutoff;
			LOG_INFO("(NBN LAS BOOTSTRAP) Riff Repeater section selection changed while enabled;"
				<< " auto-following to the newly selected section (no toggle needed)."
				<< std::endl);
			ResetBootstrap("the Riff Repeater section selection changed", owner);
			trackedOwner = owner;
			TryLatchSectionFromGrid(owner, previousStart);
		}

		// Periodic arming diagnostic. The one-time latch line is buried within a second by
		// play spam, so re-state the latched bounds vs the live transport every ~90 ticks so
		// a console snapshot always catches it. This is what tells "wrong section" from
		// "wrong note": if updateTime is outside [greyCutoff, sectionEndBoundary] the SECTION
		// is wrong; if inside, the bounds are right and the problem is elsewhere.
		if (isEpochConfirmed && (++armDiagTicks % 90u) == 0u)
		{
			LOG_INFO("(NBN LAS ARMDIAG) via=" << (confirmedViaGrid ? "grid" : "fallback")
				<< " grid=[" << std::fixed << std::setprecision(3) << greyCutoff << ".."
				<< sectionEndBoundary << "] transport=" << updateTime
				<< " inSection=" << ((updateTime >= greyCutoff && updateTime < sectionEndBoundary) ? 1 : 0)
				<< " timelineSections=" << timelineSections.size()
				<< " epoch=" << epochIndex << "." << std::endl);
		}

		float nativeSectionStart = 0.0f;
		float nativeSectionEnd = 0.0f;
		const bool hasNativeRange = TryReadNativeSectionRange(owner, nativeSectionStart, nativeSectionEnd);
		if (!hasNativeRange && !isEpochConfirmed)
		{
			// The native range is needed ONLY to latch the section end at confirmation.
			// Before that, stay inert until Rocksmith publishes a valid range. AFTER
			// confirmation the owner fields march with every restart and periodically read
			// back invalid (the mirrored pair disagrees mid-restart) - but
			// greyCutoff/sectionEndBoundary are already latched, so a failed read must NOT
			// send the confirmed controller inert. Doing so was greying every note after
			// the first loop and never re-latching (the read stayed invalid across the
			// restart churn).
			if (!hasNativeSectionRangeFailureLogged)
			{
				hasNativeSectionRangeFailureLogged = true;
				LOG_ERROR("(NBN LAS BOOTSTRAP) The GamePlaysongLAS Riff Repeater bounds"
					<< " were unreadable or their mirrored fields disagreed; Note by Note"
					<< " remains inert until Rocksmith publishes a valid native range."
					<< std::endl);
			}
			originalScoringUpdate(owner, updateTime);
			return;
		}
		if (hasNativeRange)
		{
			hasNativeSectionRangeFailureLogged = false;
		}

		// nativeSectionStart is only consumed by TryReadNativeSectionRange's own validity
		// gate (mirror agreement, end>start). greyCutoff never reads it - the +0x3C0 field
		// marches with every owned restart. Delayed-identity supplies the start instead.
		(void)nativeSectionStart;

		// Rollback (loop turnover): a backward jump in the transport clock.
		if (hasLastUpdateTime && updateTime < lastUpdateTime - ROLLBACK_THRESHOLD)
		{
			if (OwnsNativeHold())
			{
				LOG_ERROR("(NBN LAS LIFECYCLE) External rollback while a native hold was"
					<< " owned; abandoning the hold without release and re-bootstrapping"
					<< " in place." << std::endl);
				HeapCheckpoint("external-rollback");
				SetNativeFreezeFlag(owner, false);
				ResetBootstrap("an external rollback occurred while a native hold was owned");
				trackedOwner = owner;
			}
			else if (!isEpochConfirmed)
			{
				// Not armed yet: capture both endpoints of this first jump. Neither is
				// trusted as the boundary - the first rollback observed is wherever the
				// player happened to enable the feature; the identity check below decides
				// which endpoint is the real section start.
				if (!hasPendingBoundary)
				{
					hasPendingBoundary = true;
					pendingBoundaryBeforeRollback = lastUpdateTime;
					pendingBoundaryAfterRollback = updateTime;
					LOG_INFO("(NBN LAS BOOTSTRAP) Native rollback "
						<< std::fixed << std::setprecision(6) << lastUpdateTime
						<< " -> " << updateTime << "; resolving the section boundary by live"
						<< " authored/native identity across both rollback endpoints."
						<< std::endl);
				}
			}
			else
			{
				++epochIndex;
				ClearSelection();
				consumedRecords.clear();
				TryWriteDedupeGlobal(-1);
				LOG_INFO("(NBN LAS BOOTSTRAP) Native epoch restart -> epoch " << epochIndex
					<< " at updateTime=" << std::fixed << std::setprecision(6) << updateTime
					<< "." << std::endl);
			}
		}
		lastUpdateTime = updateTime;
		hasLastUpdateTime = true;

		// Delayed-identity confirmation: find the live record whose authored time AND native
		// event time both equal one endpoint of the pending jump. Exactly one endpoint must
		// resolve - a spurious rollback (e.g. 8.336 -> 7.786) matches no record and is
		// rejected, so greyCutoff is only ever written from a real section boundary and is
		// never re-read from the marching owner field.
		if (!isEpochConfirmed && hasPendingBoundary)
		{
			LiveNote beforeRollbackBoundary;
			LiveNote afterRollbackBoundary;
			bool hasBeforeRollbackBoundary = false;
			bool hasAfterRollbackBoundary = false;
			ForEachLiveNote(owner, [&](const LiveNote& note)
			{
				if (std::fabs(note.eventTime - pendingBoundaryBeforeRollback) <= BOUNDARY_EPSILON
					&& std::fabs(note.recordTime - pendingBoundaryBeforeRollback) <= BOUNDARY_EPSILON)
				{
					beforeRollbackBoundary = note;
					hasBeforeRollbackBoundary = true;
				}
				if (std::fabs(note.eventTime - pendingBoundaryAfterRollback) <= BOUNDARY_EPSILON
					&& std::fabs(note.recordTime - pendingBoundaryAfterRollback) <= BOUNDARY_EPSILON)
				{
					afterRollbackBoundary = note;
					hasAfterRollbackBoundary = true;
				}
				return true;
			});

			if (hasBeforeRollbackBoundary != hasAfterRollbackBoundary)
			{
				const bool usesBeforeRollbackEndpoint = hasBeforeRollbackBoundary;
				const LiveNote& boundaryNote = usesBeforeRollbackEndpoint
					? beforeRollbackBoundary
					: afterRollbackBoundary;
				greyCutoff = usesBeforeRollbackEndpoint
					? pendingBoundaryBeforeRollback
					: pendingBoundaryAfterRollback;
				// Latch the END once, now, while +0x3C8 still holds the real loop end: no
				// owned restart has fired yet, so slot +0x54 has not marched the field. It
				// is never re-read after this. The half-open end gate in
				// FindEarliestEligibleNote drops the next-iteration boundary note.
				sectionEndBoundary = nativeSectionEnd;
				hasSectionEndBoundary = true;
				isEpochConfirmed = true;
				confirmedViaGrid = false;
				epochIndex = 1;
				consumedRecords.clear();
				LOG_INFO("(NBN LAS BOOTSTRAP) Section boundary confirmed by record=0x"
					<< std::hex << boundaryNote.record << std::dec << " at greyCutoff="
					<< std::fixed << std::setprecision(6) << greyCutoff << " using the rollback's "
					<< (usesBeforeRollbackEndpoint ? "pre-jump" : "post-jump")
					<< " endpoint; end latched at " << sectionEndBoundary
					<< "; epoch 1 begins." << std::endl);
				HeapCheckpoint("bootstrap-confirmed");

				if (!usesBeforeRollbackEndpoint)
				{
					// The post-jump endpoint is the native section-initialization update.
					// Suppress its scoring pass so it cannot expire the boundary record;
					// selection begins on the next ordinary timeline update.
					LOG_INFO("(NBN LAS BOOTSTRAP) The post-jump endpoint is the native"
						<< " section-initialization update; its first scoring pass is"
						<< " suppressed." << std::endl);
					return;
				}
			}
		}

		if (isEpochConfirmed)
		{
			if (gatePhase == GatePhase::Idle || gatePhase == GatePhase::Armed)
			{
				UpdateSelection(owner, updateTime);
				if (gatePhase == GatePhase::Armed) LogDetectorGates("armed", false);
			}
			else if (OwnsNativeHold())
			{
				++holdTickCount;
				// First-hold corruption lattice: validate every tick for the first
				// ~15 seconds of a session's first hold, so the corrupting write
				// is bracketed to a single tick instead of "after establishment".
				static bool firstHoldLatticeDone = false;
				if (!firstHoldLatticeDone)
				{
					if (holdTickCount <= 600)
					{
						char seam[32];
						std::snprintf(seam, sizeof(seam), "early-tick-%llu", static_cast<unsigned long long>(holdTickCount));
						HeapCheckpoint(seam);
					}
					else
					{
						firstHoldLatticeDone = true;
					}
				}
				{
					const bool usesSuccessor = denseSuccessor.record != 0;
					const uintptr_t visual = usesSuccessor ? denseSuccessor.record : selectedRecord;
					if (visual != lastVisualGroupRecord)
					{
						lastVisualGroupRecord = visual;
						BuildVisualGroup(owner, visual,
							usesSuccessor ? denseSuccessor.recordTime : selectedRecordTime,
							usesSuccessor ? denseSuccessor.stringIndex : selectedString);
					}
				}
				if (holdTickCount == 1)
				{
					holdTickRateAnchor = std::chrono::steady_clock::now();
					holdTickRateAnchorTick = 0;
					hasDetectorSampleAnchor = false;
					hasLastSampledRingIndex = false;
				}
				// What the input gates are doing, for as long as the hold is unsatisfied.
				// Throttled to once a second, and read-only: nothing here calls either
				// native query, because the onset query writes the dedupe global and an
				// extra call would consume the edge the hold is waiting for.
				LogDetectorGates(DescribeGatePhase(gatePhase), false);
				if (gatePhase != GatePhase::DensePlayerSongStartPending
					&& std::fabs(updateTime - heldEpoch) > HELD_TIME_EPSILON)
				{
					std::ostringstream reason;
					reason << "The authoritative scoring time advanced to " << std::fixed
						<< std::setprecision(6) << updateTime << " while the hold owned epoch "
						<< heldEpoch << "; the stop latch failed, so no release seek is issued";
					FaultWithoutRelease(reason.str());
				}
				else if (holdTickCount % 240 == 0)
				{
					const auto now = std::chrono::steady_clock::now();
					const double elapsedSeconds =
						std::chrono::duration<double>(now - holdTickRateAnchor).count();
					const double ticksPerSecond = elapsedSeconds > 0.0
						? static_cast<double>(holdTickCount - holdTickRateAnchorTick) / elapsedSeconds
						: 0.0;
					// The rate is reported, not diagnosed. It is the frame rate, and a low
					// frame rate is worth knowing about, but the detector's own code shows
					// it is not what refuses a note: see the (NBN LAS DETECT) line for that.
					LOG_INFO("(NBN LAS HOLD) Steady at " << std::fixed << std::setprecision(6)
						<< heldEpoch << " after " << holdTickCount << " scoring ticks at "
						<< std::setprecision(1) << ticksPerSecond << " ticks/sec"
						<< (ticksPerSecond < 55.0
							? " (below the usual 60; the scoring update is frame-coupled)"
							: "")
						<< "." << std::endl);
					holdTickRateAnchor = now;
					holdTickRateAnchorTick = holdTickCount;
				}
			}
		}

		originalScoringUpdate(owner, updateTime);

		if (NoteByNoteRuntime::IsNoteByNoteEnabled() && isEpochConfirmed)
		{
			HandleAfterUpdate(owner, updateTime);
		}
	}

	bool EvaluateHeldChordDecision(void* owner, void* unusedEdx, void* note)
	{
		const auto noteAddress = reinterpret_cast<uintptr_t>(note);
		float authoredTime = 0.0f;
		float startDelta = 0.0f;
		float endDelta = 0.0f;
		double detectorClock = 0.0;
		const bool hasWindow = TryRead(noteAddress + NOTE_EVENT_TIME, authoredTime)
			&& TryRead(noteAddress + NOTE_DETECT_WINDOW_START_DELTA, startDelta)
			&& TryRead(noteAddress + NOTE_DETECT_WINDOW_END_DELTA, endDelta)
			&& std::isfinite(authoredTime)
			&& std::isfinite(startDelta)
			&& std::isfinite(endDelta)
			&& TryReadDetectorClock(detectorClock)
			&& std::isfinite(detectorClock);
		if (!hasWindow)
		{
			return originalHitDecision(owner, unusedEdx, note);
		}

		const float windowStart = authoredTime + startDelta;
		const float windowEnd = authoredTime + endDelta;
		const auto clock = static_cast<float>(detectorClock);
		const bool isClockInWindow = clock >= windowStart && clock <= windowEnd;

		bool didSlide = false;
		bool result = false;
		if (isChordWindowSlideEnabled)
		{
			const float negativeStart = -(authoredTime + 1.0f);   // windowStart == -1.0
			didSlide = TryWriteGameFloat(
				noteAddress + NOTE_DETECT_WINDOW_START_DELTA, negativeStart);
			result = originalHitDecision(owner, unusedEdx, note);
			TryWriteGameFloat(noteAddress + NOTE_DETECT_WINDOW_START_DELTA, startDelta);
			if (didSlide) ++chordWindowSlideCount;
		}
		else
		{
			result = originalHitDecision(owner, unusedEdx, note);
		}

		// Throttled to once a second, but an accepting slid evaluation always logs:
		// that single line is the fix working and must never be lost to the interval.
		// The throttled line is verbose-gated (default OFF for FPS); the accepting slid
		// line is not, so the fix-working evidence survives even with verbose off.
		const auto now = std::chrono::steady_clock::now();
		if ((result && didSlide)
			|| (verboseTrace
				&& (!hasChordWindowLogAnchor
					|| std::chrono::duration<double>(now - chordWindowLogAnchor).count() >= 1.0)))
		{
			chordWindowLogAnchor = now;
			hasChordWindowLogAnchor = true;
			char frameSummary[160] = "unreadable";
			TryDescribeCurrentAnalysisFrame(frameSummary, sizeof(frameSummary));
			LOG_INFO("(NBN LAS CHORD WINDOW) clock=" << std::fixed << std::setprecision(3)
				<< detectorClock
				<< " window=[" << windowStart << "," << windowEnd << "]"
				<< " heldEpoch=" << heldEpoch
				<< (isClockInWindow ? " in-window" : " OUT-OF-WINDOW")
				<< (didSlide ? " slid" : (isChordWindowSlideEnabled ? "" : " slide-off"))
				<< " result=" << std::boolalpha << result
				<< " slides=" << chordWindowSlideCount
				<< " | frame " << frameSummary << "." << std::endl);
		}
		return result;
	}

	bool __fastcall HitDecisionDetour(void* owner, void* unusedEdx, void* note)
	{
		std::lock_guard<std::recursive_mutex> lock(controllerMutex);

		if (!isEpochConfirmed || selectedRecord == 0 || owner != trackedOwner
			|| !NoteByNoteRuntime::IsNoteByNoteEnabled())
		{
			const bool originalResult = originalHitDecision(owner, unusedEdx, note);
			ObserveBendDecision(reinterpret_cast<uintptr_t>(note), originalResult);
			return originalResult;
		}

		uintptr_t record = 0;
		float recordTime = 0.0f;
		if (!TryRead(reinterpret_cast<uintptr_t>(note) + NOTE_RECORD, record) || record == 0
			|| !TryRead(record + RECORD_TIME, recordTime))
		{
			return originalHitDecision(owner, unusedEdx, note);
		}

		if (recordTime < greyCutoff - GREY_EPSILON)
		{
			return originalHitDecision(owner, unusedEdx, note);
		}

		if (record == selectedRecord)
		{
			if (gatePhase == GatePhase::WaitingForInputRelease
				|| gatePhase == GatePhase::Holding
				|| gatePhase == GatePhase::DenseRebuildPending
				|| gatePhase == GatePhase::DensePlayerSongStartPending)
			{
				// Chord hold (#43): instead of blocking, the game's own decision is
				// EVALUATED - a strum has no single expected pitch, so the native chord
				// detection is the release trigger. A natural true commits (this very
				// decision scores the chord) and hands the release to the commit
				// machinery. Every evaluation is counted so the frozen-transport
				// question is answered by data even when the answer is silence.
				if (selectedChordId != -1
					&& gatePhase == GatePhase::Holding
					&& areChordHoldsEnabled)
				{
					const bool naturalResult = EvaluateHeldChordDecision(owner, unusedEdx, note);
					++chordDecisionEvalCount;
					int matcherTones[6] = { 0, 0, 0, 0, 0, 0 };
					int matcherToneCount = 0;
					{
						// The authored template tones are in the guitar frame; the spectrum the matcher
						// compares against is the detected frame, so add the input shift (0 for
						// Speaker/Off, the Drop amount for Drop Pedal) - see the single-note hold.
						const int matcherInputShift = NoteByNoteRuntime::GetInputOnsetShiftSemitones();
						uintptr_t chordTemplate = 0;
						ChordTemplateView matcherView = {};
						if (TryRead(reinterpret_cast<uintptr_t>(note) + 0x30, chordTemplate)
							&& chordTemplate != 0 && TryRead(chordTemplate, matcherView))
						{
							for (int i = 0; i < 6; ++i)
							{
								if (matcherView.frets[i] < 0x1A && matcherView.notes[i] >= 0)
								{
									matcherTones[matcherToneCount++] = matcherView.notes[i] + matcherInputShift;
								}
							}
						}

						if (matcherToneCount > 0)
						{
							researchChordToneCount = matcherToneCount;
							for (int i = 0; i < matcherToneCount; ++i)
								researchChordTones[i] = matcherTones[i];
							researchChordTonesRecord = selectedRecord;
						}
					}
					const bool portRan = hasHoldLatchRingTime && matcherToneCount > 0;
					const bool nativeHit = portRan
						&& NativeChordHitSinceLatch(matcherTones, matcherToneCount, holdLatchRingTime);
					if (verboseTrace && matcherToneCount > 0)
					{
						const auto obsNow = std::chrono::steady_clock::now();
						const bool dueThrottled = !hasMatcherObserveAnchor
							|| std::chrono::duration<double>(
								obsNow - matcherObserveAnchor).count() >= 0.5;
						if (nativeHit || sawSpikeDuringHold || dueThrottled)
						{
							matcherObserveAnchor = obsNow;
							hasMatcherObserveAnchor = true;
							int soundingSeen = 0;
							for (int i = 0; i < matcherToneCount; ++i)
							{
								if (ReadNdSoundingStrength(matcherTones[i]) >= 0.0f) ++soundingSeen;
							}
							LOG_INFO("(NBN NATIVE HIT) hit=" << (nativeHit ? "YES" : "no")
								<< " tones=" << matcherToneCount
								<< " sounding=" << soundingSeen << "/" << matcherToneCount
								<< " portRan=" << (portRan ? "y" : "n")
								<< " nativeVote=" << (naturalResult ? "true" : "false")
								<< " freshStrum=" << (sawSpikeDuringHold ? "y" : "n")
								<< "." << std::endl);
						}
					}
					// PRIMARY chord accept: the native onset+harmonic scan confirms a fresh, full-chord
					// re-pick since the latch. No custom heuristics.
					if (nativeHit)
					{
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
						holdProgressAnchor = std::chrono::steady_clock::now();
						mlConfirmationState.Reset(NoteByNoteRuntime::GetMlAudioSampleIndex());
						consumedRecords.insert(selectedRecord);
						lastCommittedChordRecord = selectedRecord;
						lastCommittedChordAt = std::chrono::steady_clock::now();
						char chordIdentity[64] = "?";
						NoteByNoteNativeScoring::TryDescribeChordTarget(
							reinterpret_cast<uintptr_t>(note), chordIdentity, sizeof(chordIdentity));
						LOG_INFO("(NBN LAS CHORD) The native onset+harmonic scan accepted the held"
							<< " chord record=0x" << std::hex << selectedRecord << std::dec
							<< " (chordId " << selectedChordId << ", " << chordIdentity
							<< ") - fresh full-chord re-pick since latch after " << chordDecisionEvalCount
							<< " evaluation(s); committing." << std::endl);
						chordDecisionEvalCount = 0;
						QueryNativeOnsetNote();
						return true;
					}
										// The ND sounding-table accept is now only a FALLBACK for when the
					// native port could not run (no latch stamp / no tones). Its 0.3s per-tone memory
					// window stitches sympathetically-ringing tones into a false "all tones
					// seen" (issue #58: it committed chords while the matcher read quiet /
					// 1-of-3), so it must not fire when the matcher actually ran.
					if (!portRan && TickNdChordAcceptance(selectedRecord))
					{
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
						holdProgressAnchor = std::chrono::steady_clock::now();
						mlConfirmationState.Reset(NoteByNoteRuntime::GetMlAudioSampleIndex());
						consumedRecords.insert(selectedRecord);
						lastCommittedChordRecord = selectedRecord;
						lastCommittedChordAt = std::chrono::steady_clock::now();
						char chordIdentity[64] = "?";
						NoteByNoteNativeScoring::TryDescribeChordTarget(
							reinterpret_cast<uintptr_t>(note), chordIdentity, sizeof(chordIdentity));
						LOG_INFO("(NBN LAS CHORD) The ND sounding table accepted the held"
							<< " chord record=0x" << std::hex << selectedRecord << std::dec
							<< " (chordId " << selectedChordId << ", " << chordIdentity
							<< ") after " << chordDecisionEvalCount
							<< " evaluation(s); committing." << std::endl);
						chordDecisionEvalCount = 0;
						QueryNativeOnsetNote();
						return true;
					}
					// CHORD TIER-0 RESCUE (last resort, default OFF): native scan + ND both missed
					// a chord. If a FRESH strum happened (sawSpikeDuringHold) and EVERY expected tone
					// is energy-confirmed, accept - the correct chord was played, the matcher just did
					// not register it. Additive: only fires after both native paths declined, so it can
					// never fail a correct play; conservative (all tones) so it never accepts a wrong grab.
					if (isChordTier0RescueEnabled && portRan && sawSpikeDuringHold
						&& Tier0ConfirmsChord(matcherTones, matcherToneCount))
					{
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
						holdProgressAnchor = std::chrono::steady_clock::now();
						mlConfirmationState.Reset(NoteByNoteRuntime::GetMlAudioSampleIndex());
						consumedRecords.insert(selectedRecord);
						lastCommittedChordRecord = selectedRecord;
						lastCommittedChordAt = std::chrono::steady_clock::now();
						char chordIdentity[64] = "?";
						NoteByNoteNativeScoring::TryDescribeChordTarget(
							reinterpret_cast<uintptr_t>(note), chordIdentity, sizeof(chordIdentity));
						LOG_INFO("(NBN CHORD TIER0) RESCUE accepted the held chord record=0x"
							<< std::hex << selectedRecord << std::dec << " (chordId " << selectedChordId
							<< ", " << chordIdentity << ") - all " << matcherToneCount
							<< " expected tones energy-confirmed on a fresh strum." << std::endl);
						chordDecisionEvalCount = 0;
						QueryNativeOnsetNote();
						return true;
					}
					if (naturalResult && !sawSpikeDuringHold)
					{
						// The evaluator hears the chord, but no attack has happened since
						// this hold latched: that is the PREVIOUS strum still ringing
						// (first chord session: same-chord sequences auto-advanced on the
						// ring). A fresh strum unblocks through either evidence source: a
						// level-meter spike, or a post-latch ring frame carrying the
						// analysis onset flag (the flag is what catches a re-strum layered
						// on a still-loud ring, where the meter barely moves).
						const auto now = std::chrono::steady_clock::now();
						// Verbose-gated (#log-noise, default OFF for FPS): this eval-false line
						// plus its per-tone sounding-table dump is the chord diagnosis feed.
						// Off it never prints; verbose on to read the per-tone strengths
						// (including the raw sub-threshold values).
						if (verboseTrace
							&& (!hasChordDecisionLogAnchor
								|| std::chrono::duration<double>(
									now - chordDecisionLogAnchor).count() >= 1.0))
						{
							chordDecisionLogAnchor = now;
							hasChordDecisionLogAnchor = true;
							LOG_INFO("(NBN LAS CHORD) Held chord decision is true but no"
								<< " attack has been seen since the hold latched; treating"
								<< " it as the previous strum's ring and waiting for a"
								<< " fresh strum." << std::endl);
						}
						return false;
					}
					// The loose native window-slide vote is now only a FALLBACK for the case
					// the native port could not run at all (no latch stamp / no tones: det
					// unresolved, or ND tones not set for this record). Whenever the matcher
					// actually ran - YES, no, or quiet - it governs, so a partial strum the
					// vote used to wave through (issue #58) no longer commits here.
					if (naturalResult && !portRan)
					{
						gatePhase = GatePhase::CommitBeforeRelease;
						commitTickCount = 0;
						wasCommitOverrideLogged = false;
						holdProgressAnchor = std::chrono::steady_clock::now();
						mlConfirmationState.Reset(NoteByNoteRuntime::GetMlAudioSampleIndex());
						consumedRecords.insert(selectedRecord);
						lastCommittedChordRecord = selectedRecord;
						lastCommittedChordAt = std::chrono::steady_clock::now();
						char chordIdentity[64] = "?";
						NoteByNoteNativeScoring::TryDescribeChordTarget(
							reinterpret_cast<uintptr_t>(note), chordIdentity, sizeof(chordIdentity));
						LOG_INFO("(NBN LAS CHORD) The native hit decision accepted the held"
							<< " chord record=0x" << std::hex << selectedRecord << std::dec
							<< " (chordId " << selectedChordId << ", " << chordIdentity
							<< ") naturally after "
							<< chordDecisionEvalCount << " evaluation(s); committing."
							<< std::endl);
						chordDecisionEvalCount = 0;
						// Consumption symmetry (the #52 rule, chord form): consume the
						// strum's onset edge so it cannot commit a successor single.
						QueryNativeOnsetNote();
						return true;
					}
					const auto now = std::chrono::steady_clock::now();
					if (!hasChordDecisionLogAnchor
						|| std::chrono::duration<double>(
							now - chordDecisionLogAnchor).count() >= 1.0)
					{
						chordDecisionLogAnchor = now;
						hasChordDecisionLogAnchor = true;
						// Identity in every throttled line: some chords accept in under
						// 500 evaluations while others refuse thousands with real input
						// sounding, and whatever distinguishes them must be visible here.
						// The template identity (name + frets, read from the very note
						// object being judged) is the desync detector: if it disagrees
						// with what the screen shows, the visuals are stale; if it
						// disagrees with the record's chordId, the note object is stale.
						char chordIdentity[64] = "?";
						NoteByNoteNativeScoring::TryDescribeChordTarget(
							reinterpret_cast<uintptr_t>(note), chordIdentity, sizeof(chordIdentity));
						{
							uintptr_t templateAddress = 0;
							ChordTemplateView view = {};
							if (TryRead(reinterpret_cast<uintptr_t>(note) + 0x30, templateAddress)
								&& templateAddress != 0
								&& TryRead(templateAddress, view))
							{
								char soundingText[160];
								size_t at = 0;
								soundingText[0] = '\0';
								for (int stringIndex = 0;
									stringIndex < 6 && at < sizeof(soundingText) - 24; ++stringIndex)
								{
									if (view.frets[stringIndex] >= 0x1A
										|| view.notes[stringIndex] < 0)
									{
										continue;
									}
									const float strength =
										ReadNdSoundingStrength(view.notes[stringIndex]);
									at += std::snprintf(soundingText + at,
										sizeof(soundingText) - at,
										"%s%d:%s", at == 0 ? "" : " ",
										view.notes[stringIndex],
										strength >= 0.0f ? "YES" : "no");
									if (strength >= 0.0f)
									{
										at += std::snprintf(soundingText + at,
											sizeof(soundingText) - at, "(%.1f)", strength);
									}
									else
									{
										// Show the RAW sub-threshold strength (#open-A false
										// accept): distinguishes a tone truly absent from one
										// present-but-weak (a sympathetic ring the player never
										// picked). "raw" means below the accept bar.
										const float raw =
											ReadNdRawSoundingStrength(view.notes[stringIndex]);
										at += std::snprintf(soundingText + at,
											sizeof(soundingText) - at,
											std::isfinite(raw) ? "(raw %.1f)" : "(absent)",
											raw);
									}
								}
								LOG_INFO("(NBN ND CHORD) sounding-table per tone: "
									<< soundingText << std::endl);
							}
						}
						LOG_INFO("(NBN LAS CHORD) Held chord decision evaluated false ("
							<< chordDecisionEvalCount << " evaluation(s) so far)"
							<< " record=0x" << std::hex << selectedRecord << std::dec
							<< " chordId=" << selectedChordId
							<< " chord=" << chordIdentity
							<< " chordNotesId=" << selectedChordNotesId
							<< " time=" << std::fixed << std::setprecision(3)
							<< selectedRecordTime
							<< "; the native evaluator IS running against the frozen"
							<< " transport." << std::endl);
					}
					return false;
				}
				return false;
			}
			if (gatePhase == GatePhase::CommitBeforeRelease
				|| gatePhase == GatePhase::RecommitAfterRelease
				|| gatePhase == GatePhase::DenseRecommitAfterRebuild)
			{
				bool originalResult = originalHitDecision(owner, unusedEdx, note);
				if (!wasCommitOverrideLogged)
				{
					wasCommitOverrideLogged = true;
					const char* commitName = "post-release recommit";
					if (gatePhase == GatePhase::CommitBeforeRelease)
					{
						commitName = "pre-release commit";
					}
					else if (gatePhase == GatePhase::DenseRecommitAfterRebuild)
					{
						commitName = "dense-rebuild recommit";
					}
					LOG_INFO("(NBN LAS INPUT) Forcing the selected native "
						<< commitName
						<< "; the original decision returned "
						<< std::boolalpha << originalResult << "." << std::endl);
				}
				return true;
			}
			return originalHitDecision(owner, unusedEdx, note);
		}

		// Close-note protection: while a selected record owns the gate, no other
		// non-grey identity may consume a hit decision.
		return false;
	}
}

void NoteByNoteScoringCore::Initialize()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	if (isInitialized) return;

	isInitialized = true;
	LOG_INFO("(NBN LAS LIFECYCLE) Reloadable Note by Note controller initialized."
		<< " The lesson-derived hold (Stop_TMusic latch + five-clock epoch) and coordinated"
		<< " PlayerSong-packet release are armed but inert until Note by Note is enabled."
		<< std::endl);
}

bool NoteByNoteScoringCore::IsAvailable()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return isInitialized;
}

void NoteByNoteNativeScoring::SetChordHoldsEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	areChordHoldsEnabled = enabled;
	LOG_INFO("(NBN LAS CHORD) Chord holds " << (enabled ? "enabled" : "disabled")
		<< (enabled
			? ": held chords release on the game's own hit decision."
			: ": chords play through natively while successors stay gated.")
		<< std::endl);
}

void NoteByNoteNativeScoring::SetFreezePromptSoundEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isFreezePromptSoundEnabled = enabled;
	LOG_INFO("(NBN LAS PROMPT) " << FREEZE_NOTE_TRACK_EVENT << " before each hold "
		<< (enabled ? "enabled (the per-note percussion cue is back)"
			: "disabled (default: the per-note percussion cue is silenced)")
		<< std::endl);
}

void NoteByNoteNativeScoring::SetFlowUntilMissEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isFlowUntilMissEnabled = enabled;
	LOG_INFO("(NBN FLOW) Flow-until-miss "
		<< (enabled
			? "ENABLED: the hold boundary sits late (recordTime + grace) so on-time notes"
			  " commit naturally and the transport plays straight through; freeze only on a"
			  " real miss. Dense sections keep their audio."
			: "disabled: the pre-flow freeze-per-note boundary (recordTime - compensation)"
			  " is restored; every note freezes and restarts the transport.")
		<< std::endl);
}

bool NoteByNoteNativeScoring::GetFlowUntilMissEnabled()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return isFlowUntilMissEnabled;
}

void NoteByNoteNativeScoring::SetFlowLateGraceSeconds(float seconds)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	// Keep the grace inside the native detect window's late edge (~0.3s past record time):
	// the frozen clock must still fall within the note's window or native calls it dead.
	if (seconds < 0.0f) seconds = 0.0f;
	if (seconds > 0.29f) seconds = 0.29f;
	flowLateGraceSeconds = seconds;
	LOG_INFO("(NBN FLOW) Flow late-grace set to " << std::fixed << std::setprecision(3)
		<< flowLateGraceSeconds << "s (how far past a note's time the transport flows"
		<< " before the boundary freezes on it)." << std::endl);
}

float NoteByNoteNativeScoring::GetFlowLateGraceSeconds()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return flowLateGraceSeconds;
}

bool NoteByNoteNativeScoring::GetChordHoldsEnabled()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return areChordHoldsEnabled;
}

void NoteByNoteNativeScoring::SetVerboseTrace(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	verboseTrace = enabled;
	// This one line always prints (it is not itself gated) so the toggle is visible in
	// the log even when everything else falls quiet.
	LOG_INFO("(NBN TRACE) Verbose trace " << (enabled ? "ENABLED" : "disabled")
		<< (enabled
			? ": per-tick DETECT/BEND/CHORD-WINDOW/eval-false logs are on for diagnosis."
			: ": high-frequency logs are off; event logs still print. Higher FPS.")
		<< std::endl);
}

bool NoteByNoteNativeScoring::GetVerboseTrace()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return verboseTrace;
}

namespace
{
	const char* DescribeStrategy(NoteByNoteNativeScoring::DetectionStrategy strategy)
	{
		switch (strategy)
		{
			case NoteByNoteNativeScoring::DetectionStrategy::NativeOnly:
				return "NativeOnly (ML shadows only)";
			case NoteByNoteNativeScoring::DetectionStrategy::MlOnly:
				return "MlOnly (ML co-sign + strict veto; native shadows)";
			default:
				return "Blend (native + tier-0 + ML: mutual rescue, ML veto only when confident and not octave-off)";
		}
	}
}

void NoteByNoteNativeScoring::SetChordDetectionStrategy(DetectionStrategy strategy)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	g_chordDetectionStrategy = static_cast<int>(strategy);
	LOG_INFO("(NBN STRATEGY) Chord detection = " << DescribeStrategy(strategy)
		<< " (no ML-primary chord decider yet; ML stays shadow/rescue)" << std::endl);
}

void NoteByNoteNativeScoring::SetNoteDetectionStrategy(DetectionStrategy strategy)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	g_noteDetectionStrategy = static_cast<int>(strategy);
	LOG_INFO("(NBN STRATEGY) Single-note detection = " << DescribeStrategy(strategy) << std::endl);
}

void NoteByNoteNativeScoring::SetBendDetectionStrategy(DetectionStrategy strategy)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	g_bendDetectionStrategy = static_cast<int>(strategy);
	LOG_INFO("(NBN STRATEGY) Bend detection = " << DescribeStrategy(strategy)
		<< " (no ML-primary bend decider yet; ML stays shadow/rescue)" << std::endl);
}

bool NoteByNoteNativeScoring::TryDescribeChordTarget(
	uintptr_t noteAddress,
	char* buffer,
	size_t bufferLength)
{
	if (buffer == nullptr || bufferLength == 0) return false;
	buffer[0] = '\0';

	uintptr_t templateAddress = 0;
	if (!TryRead(noteAddress + NOTE_CHORD_RECORD, templateAddress) || templateAddress == 0)
	{
		return false;
	}
	ChordTemplateView view = {};
	if (!TryRead(templateAddress, view)) return false;

	// The name field is authored ASCII; anything unprintable means the template
	// read is not what it claims to be, so the name is dropped, not sanitized.
	char name[sizeof(view.name) + 1] = {};
	for (size_t i = 0; i < sizeof(view.name) && view.name[i] != '\0'; ++i)
	{
		if (view.name[i] < 0x20 || view.name[i] > 0x7E) { name[0] = '\0'; break; }
		name[i] = view.name[i];
	}

	char frets[24] = {};
	size_t at = 0;
	for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
	{
		const uint8_t fret = view.frets[stringIndex];
		// The evaluator's own playability test: values below 0x1A are real frets.
		at += std::snprintf(frets + at, sizeof(frets) - at, "%s%s",
			stringIndex == 0 ? "" : "/",
			fret < 0x1A ? std::to_string(fret).c_str() : "x");
		if (at >= sizeof(frets)) break;
	}

	if (name[0] != '\0')
	{
		std::snprintf(buffer, bufferLength, "%s [%s]", name, frets);
	}
	else
	{
		std::snprintf(buffer, bufferLength, "[%s]", frets);
	}
	return true;
}

void NoteByNoteNativeScoring::SetNativeChordPanelEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isNativeChordPanelEnabled = enabled;
	if (!enabled) HideNativeChordPanel();
	LOG_INFO("(NBN LAS CHORD PANEL) Native chord display driving "
		<< (enabled ? "enabled" : "disabled") << "." << std::endl);
}

void NoteByNoteNativeScoring::RequestNativeSeekTest()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isNativeSeekTestRequested = true;
	LOG_INFO("(NBN NATIVE SEEK) Test requested; the next idle scoring tick"
		<< " performs one StartAt-core call on the main thread." << std::endl);
}

void NoteByNoteNativeScoring::SetScheduleShiftEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isScheduleShiftEnabled = enabled;
	LOG_INFO("(NBN LAS SHIFT) Native schedule-shift-at-release "
		<< (enabled ? "enabled: each release compensates the engine for the frozen span (entry 4, op add)."
			: "disabled.")
		<< std::endl);
}

void NoteByNoteNativeScoring::RequestFreezeModeTest()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isFreezeModeTestArmed = true;
	LOG_INFO("(NBN MODE) Freeze-mode test armed: the next established hold enters"
		<< " the game's own frozen mode (FreezeSong core, mode 2,0) and its"
		<< " release exits (UnfreezeSong core, mode 0,2). One-shot." << std::endl);
}

void NoteByNoteNativeScoring::SetNdAcceptEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isNdAcceptEnabled = enabled;
	hasNdSoundingStreak = false;
	ndChordToneCount = 0;
	ndChordForRecord = 0;
	hasNdChordGroup = false;
	LOG_INFO("(NBN ND) Native sounding-table acceptance "
		<< (enabled ? "enabled: expected pitch above the native threshold for 0.18s accepts."
			: "disabled: acceptance falls back to the heuristic stack alone.")
		<< std::endl);
}

void NoteByNoteNativeScoring::SetNativeReleaseEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isNativeReleaseEnabled = enabled;
	LOG_INFO("(NBN NATIVE RELEASE) Native release "
		<< (enabled ? "enabled (hybrid): owned releases run the StartAt core (unfreeze +"
			" seek-to-now + speed reset) and then the coordinated PlayerSong restart"
			" for the music."
			: "disabled: owned releases use the coordinated PlayerSong restart alone.")
		<< std::endl);
}

void NoteByNoteNativeScoring::SetSafetyReleaseEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isSafetyReleaseEnabled = enabled;
	LOG_INFO("(NBN LAS SAFETY) Timed safety release "
		<< (enabled ? "enabled: a no-progress hold releases after its budget."
			: "disabled: a no-progress hold stays held and logs its refusing state; N releases it.")
		<< std::endl);
}

bool NoteByNoteNativeScoring::GetNativeChordPanelEnabled()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return isNativeChordPanelEnabled;
}

void NoteByNoteNativeScoring::SetNativeFreezeFlagEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isNativeFreezeFlagEnabled = enabled;
	LOG_INFO("(NBN LAS FREEZE FLAG) Native frozen-on-tag flag writes "
		<< (enabled ? "enabled" : "disabled")
		<< ": owner+0x5E3 " << (enabled ? "announces every owned hold" : "is left native")
		<< "." << std::endl);
}

bool NoteByNoteNativeScoring::GetNativeFreezeFlagEnabled()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return isNativeFreezeFlagEnabled;
}

void NoteByNoteNativeScoring::SetRepeatStrumHoldsEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	areRepeatStrumHoldsEnabled = enabled;
	LOG_INFO("(NBN LAS CHORD) Repeat-strum holds " << (enabled ? "enabled" : "disabled")
		<< (enabled
			? ": bare-0x2 repeat records hold and release like full chords."
			: ": bare-0x2 repeat records play through natively (the pre-2026-08-24"
			  " stopgap; skipped strums count as missed).")
		<< std::endl);
}

bool NoteByNoteNativeScoring::GetRepeatStrumHoldsEnabled()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return areRepeatStrumHoldsEnabled;
}

void NoteByNoteNativeScoring::SetChordWindowSlideEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isChordWindowSlideEnabled = enabled;
	LOG_INFO("(NBN LAS CHORD WINDOW) Window slide " << (enabled ? "enabled" : "disabled")
		<< (enabled
			? ": a held chord whose detection window the detector clock has left is"
			  " evaluated with the window slid onto the clock, authored width preserved."
			: ": held chords are evaluated against their authored windows only, so"
			  " out-of-window refusals are measured rather than repaired.")
		<< std::endl);
}

void NoteByNoteNativeScoring::SetChordTier0RescueEnabled(bool enabled)
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	isChordTier0RescueEnabled = enabled;
	LOG_INFO("(NBN CHORD TIER0) Chord tier-0 rescue " << (enabled ? "ENABLED" : "disabled")
		<< (enabled
			? ": a fresh-strummed chord the native scan + ND both miss is accepted when every"
			  " expected tone is energy-confirmed (per-tone raw dominance)."
			: ": chords rely on the native scan + ND only.")
		<< std::endl);
}

bool NoteByNoteNativeScoring::GetChordTier0RescueEnabled()
{
	return isChordTier0RescueEnabled;
}

bool NoteByNoteNativeScoring::GetChordWindowSlideEnabled()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	return isChordWindowSlideEnabled;
}

void NoteByNoteNativeScoring::Shutdown()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	ClearSelection();
	ResetBootstrap("the reloadable controller is shutting down");
	originalScoringUpdate = nullptr;
	originalHitDecision = nullptr;
	isInitialized = false;
}

void NoteByNoteNativeScoring::ProcessScoringUpdate(
	void* owner,
	float updateTime,
	NoteByNoteProtocol::ScoringUpdate original)
{
	if (original == nullptr) return;
	originalScoringUpdate = reinterpret_cast<ScoringUpdateFn>(original);
	ScoringUpdateDetour(owner, updateTime);
}

bool NoteByNoteNativeScoring::ProcessHitDecision(
	void* owner,
	void* unusedEdx,
	void* note,
	NoteByNoteProtocol::HitDecision original)
{
	if (original == nullptr) return false;
	originalHitDecision = reinterpret_cast<HitDecisionFn>(original);
	return HitDecisionDetour(owner, unusedEdx, note);
}

NoteByNoteProtocol::NoteByNoteState NoteByNoteNativeScoring::GetStateSnapshot()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	NoteByNoteProtocol::NoteByNoteState state;
	state.isInitialized = isInitialized ? 1 : 0;
	state.isEpochConfirmed = isEpochConfirmed ? 1 : 0;
	state.ownsNativeHold = OwnsNativeHold() ? 1 : 0;
	state.gatePhase = static_cast<NoteByNoteProtocol::GatePhase>(gatePhase);
	state.trackedOwner = reinterpret_cast<uintptr_t>(trackedOwner);
	state.selectedRecord = selectedRecord;
	state.epoch = epochIndex;
	state.holdTickCount = holdTickCount;
	state.visualGroupCount = visualGroupCount;
	for (uint32_t i = 0; i < visualGroupCount
		&& i < NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup; ++i)
	{
		state.visualGroupRecords[i] = visualGroupRecords[i];
		state.visualGroupStrings[i] = visualGroupStrings[i];
		state.visualGroupFrets[i] = visualGroupFrets[i];
	}
	state.lastUpdateTime = lastUpdateTime;
	state.selectedRecordTime = selectedRecordTime;
	state.selectedHoldTime = selectedHoldTime;
	state.heldEpoch = heldEpoch;
	state.selectedString = selectedString;
	state.selectedFret = selectedFret;
	state.selectedChordId = selectedChordId;
	state.selectedChordNotesId = selectedChordNotesId;
	state.expectedMidi = expectedMidi;

	if (selectedChordId >= 0 && researchChordToneCount > 0
		&& researchChordTonesRecord == selectedRecord)
	{
		state.expectedChordToneCount = (std::min)(static_cast<uint32_t>(researchChordToneCount),
			NoteByNoteProtocol::NoteByNoteState::MaxChordTones);
		for (uint32_t i = 0; i < state.expectedChordToneCount; ++i)
			state.expectedChordTones[i] = researchChordTones[i];
	}

	state.isBendTarget = isBendTarget ? 1 : 0;
	state.bendAcceptMidi = bendAcceptMidi;

	if (denseSuccessor.record != 0)
	{
		state.visualRecord = denseSuccessor.record;
		state.visualString = denseSuccessor.stringIndex;
		state.visualFret = denseSuccessor.fret;
		state.visualChordId = denseSuccessor.chordId;
		state.visualChordNotesId = denseSuccessor.chordNotesId;
	}
	else
	{
		state.visualRecord = selectedRecord;
		state.visualString = selectedString;
		state.visualFret = selectedFret;
		state.visualChordId = selectedChordId;
		state.visualChordNotesId = selectedChordNotesId;
	}

	// Bend visualizer feed. Populated whenever a bend gesture is the business at
	// hand; the sounding pitch is read fresh on every call so the overlay (which
	// polls per frame) animates like a tuner. Reads only; neither native query is
	// called, so the onset edge and dedupe global are untouched.
	//
	// The feed logs itself (throttled, into the trace file): the overlay's
	// heartbeat showed bendTarget=-1 through a sixty-second hold ON A BEND, so
	// either this branch is not taken or its values are lost in marshalling, and
	// only a probe-side record can say which.
	{
		static std::chrono::steady_clock::time_point bendFeedLogAnchor;
		static bool hasBendFeedLogAnchor = false;
		const auto now = std::chrono::steady_clock::now();
		// Verbose- and input-gated (#log-noise, default OFF for FPS): off it never prints;
		// on, a put-down guitar still stops flooding the buffer and it resumes on real input.
		if (verboseTrace && NbnInputPresent()
			&& (!hasBendFeedLogAnchor
				|| std::chrono::duration<double>(now - bendFeedLogAnchor).count() >= 2.0))
		{
			bendFeedLogAnchor = now;
			hasBendFeedLogAnchor = true;
			LOG_INFO("(NBN BEND FEED) isBendTarget=" << isBendTarget
				<< " isBendRunConfirmation=" << isBendRunConfirmation
				<< " expectedMidi=" << expectedMidi
				<< " bendAcceptMidi=" << bendAcceptMidi
				<< " phase=" << static_cast<int>(gatePhase) << std::endl);
		}
	}
	// Input-health feed, from the per-tick cache rather than a fresh read: the
	// scoring tick refreshes it 20-60 times a second, plenty for a strum meter,
	// and the fresh read (a VirtualQuery syscall per TryRead, up to three polls a
	// frame) cost real frame time on the Debug host. A sample older than two
	// seconds means scoring is not ticking (menus, idle) and the line withdraws
	// instead of showing a stale or legitimately-muted level as DEAD.
	if (hasLastStateGateSample
		&& std::isfinite(lastStateGateSample.level)
		&& std::chrono::duration<double>(
			std::chrono::steady_clock::now() - lastStateGateSampleAt).count() < 2.0)
	{
		state.detectorLevelDb = lastStateGateSample.level;
		state.detectorQuality = std::isfinite(lastStateGateSample.quality)
			? lastStateGateSample.quality
			: 0.0f;
		state.detectorLoudestMidi = lastStateGateSample.currentNote;
		state.detectorSampleValid = 1;
	}
	if ((isBendTarget || isBendRunConfirmation) && expectedMidi >= 0)
	{
		state.bendBaseMidi = expectedMidi;
		state.bendTargetMidi = bendAcceptMidi >= 0
			? bendAcceptMidi
			: (isConfirmingLegatoRun && legatoRunIndex < legatoRunCount
				&& legatoRunIsBend[legatoRunIndex]
				? legatoRunMidi[legatoRunIndex]
				: -1);
		const float wanted = static_cast<float>(
			state.bendTargetMidi >= 0 ? state.bendTargetMidi : expectedMidi);
		float trackerPitch = 0.0f;
		// A tight window around the bend (base is up to MAX_BEND_SEMITONES below the target):
		// the old 24-semitone window let a stray tracker pitch far from the bend feed the meter,
		// which read as the needle flickering to a rail. Cover base..target+overshoot only.
		float bendEstMidi = -1.0f;
		float bendEstConfidence = 0.0f;
		if (TryGetSoundingPitchNear(wanted, MAX_BEND_SEMITONES + 1.5f, trackerPitch))
		{
			state.soundingMidi = trackerPitch;
			state.soundingQuality = 100.0f;
		}
		else if (TryEstimateRawBendPitch(
					static_cast<double>(state.bendBaseMidi), static_cast<double>(wanted),
					bendEstMidi, bendEstConfidence)
				&& bendEstConfidence >= 20.0f)
		{
			// The tracker is not registered - the low/slight part of the bend, and its drop-outs.
			// The raw-tap fractional estimate keeps the needle following the bend 1:1 here instead
			// of freezing on the integer detector's out-of-band garbage.
			state.soundingMidi = bendEstMidi;
			state.soundingQuality = bendEstConfidence;
		}
		else
		{
			DetectorGateSample sample;
			if (TryReadDetectorGates(sample) && sample.currentNote >= 0
				&& std::isfinite(sample.quality)
				&& sample.quality >= DETECTOR_RAW_BEND_QUALITY_FLOOR)
			{
				state.soundingMidi = static_cast<float>(sample.currentNote);
				state.soundingQuality = sample.quality;
			}
		}
	}

	// Detection-strategy authority + live native-vs-ML agreement, for the corner HUD strip.
	// HUD flag: ML participates in the decision (rescue/veto) whenever the technique is not
	// forced NativeOnly. In Blend (the default) ML is active as rescue + guarded veto.
	state.chordAuthorityIsMl = g_chordDetectionStrategy != static_cast<int>(DetectionStrategy::NativeOnly) ? 1 : 0;
	state.noteAuthorityIsMl = g_noteDetectionStrategy != static_cast<int>(DetectionStrategy::NativeOnly) ? 1 : 0;
	state.bendAuthorityIsMl = g_bendDetectionStrategy != static_cast<int>(DetectionStrategy::NativeOnly) ? 1 : 0;
	{
		const DetectionComparison& c = g_detectionComparison;
		state.compareAgreeCount = c.agree;
		state.compareDisagreeCount = c.disagree;
		state.compareLastTechnique = c.lastTechnique;
		state.compareLastNativeMatch = c.lastNativeMatch ? 1 : 0;
		state.compareLastMlMatch = c.lastMlMatch ? 1 : 0;
		state.compareLastMlHadOpinion = c.lastMlHadOpinion ? 1 : 0;
		state.compareLastValid = c.lastValid ? 1 : 0;
		state.compareHistoryCount = (std::min)(c.historyCount,
			NoteByNoteProtocol::NoteByNoteState::CompareHistoryLength);
		for (uint32_t i = 0; i < state.compareHistoryCount; ++i)
			state.compareHistory[i] = c.history[i];
	}
	return state;
}

void NoteByNoteScoringCore::ObserveRenderedAttack(
	const NoteByNoteProbe::NativeRenderedAttack& attack)
{
	std::unique_lock<std::recursive_mutex> lock(controllerMutex, std::try_to_lock);
	if (!lock.owns_lock()) return;
	if (!OwnsNativeHold()) return;

	++renderFramesWhileHeld;
	if (renderFramesWhileHeld <= 3 || renderFramesWhileHeld % 60 == 0)
	{
		std::ostringstream notes;
		for (size_t index = 0; index < attack.notes.size(); ++index)
		{
			if (index != 0) notes << ',';
			notes << attack.notes[index].stringIndex << ':' << attack.notes[index].fret;
		}
		LOG_INFO("(NBN LAS RENDER) heldFrame=" << renderFramesWhileHeld
			<< " songTime=" << std::fixed << std::setprecision(6) << attack.songTime
			<< " incomingFront=" << notes.str()
			<< " x=" << attack.longitudinalPosition << "." << std::endl);
	}
}

void NoteByNoteNativeScoring::SetHeapCheckEnabled(bool shouldEnable)
{
	isHeapCheckEnabled.store(shouldEnable, std::memory_order_relaxed);
	LOG_INFO("(NBN HEAP CHECK) Checkpoints " << (shouldEnable ? "enabled" : "disabled")
		<< "." << std::endl);
}

void NoteByNoteScoringCore::Stop()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	HeapCheckpoint("stop");
	if (OwnsNativeHold())
	{
		isReleaseRequested = true;
		LOG_INFO("(NBN LAS STOP) A native hold is owned; its coordinated release is queued to the"
			<< " next scoring tick, which continues to run while the game is held." << std::endl);
		return;
	}
	ClearSelection();
}

void NoteByNoteScoringCore::RequestReArm()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	// Latch only; the next scoring tick runs ResetBootstrap for the live owner. Doing
	// the reset here would touch owner state off the scoring thread. Cheap and
	// idempotent: a redundant enable (no section change) just re-arms the same section.
	reArmRequested = true;
	LOG_INFO("(NBN LAS BOOTSTRAP) Re-arm requested on enable; the next scoring tick"
		<< " re-bootstraps for the current section." << std::endl);
}
