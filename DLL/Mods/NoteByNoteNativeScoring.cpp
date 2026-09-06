#include "ResearchProbeRuntime.hpp"
#include "MlConfirmationState.hpp"
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

// Every line this TU logs is mirrored, flushed per line, into a file next to the
// loaded probe. The console scrollback holds roughly forty seconds before the
// per-frame diagnostics flood it, which cost the investigation three consecutive
// captures of the exact pick moment it needed: by the time Philip has paused and
// said so, the event is gone. The game locks only ITS OWN log file; this one is
// ours to read at leisure. Timestamps are seconds since this DLL loaded, so a
// reload starts a new epoch (and appends, never truncates).
namespace
{
	void AppendNbnTraceLine(const std::string& text)
	{
		static std::mutex traceMutex;
		std::lock_guard<std::mutex> lock(traceMutex);
		static const auto traceEpoch = std::chrono::steady_clock::now();
		static std::ofstream trace = []()
		{
			// The probe loads from <game>\RSModsResearch\Loaded\, so the trace sits
			// in <game>\RSModsResearch\ regardless of the process working directory.
			char modulePath[MAX_PATH] = {};
			HMODULE module = nullptr;
			std::string path = "RSModsResearch\\nbn-trace.log";
			if (GetModuleHandleExA(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
						| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(&AppendNbnTraceLine), &module)
				&& GetModuleFileNameA(module, modulePath, MAX_PATH) != 0)
			{
				std::string full(modulePath);
				const auto lastSlash = full.find_last_of('\\');
				if (lastSlash != std::string::npos)
				{
					const auto parentSlash = full.find_last_of('\\', lastSlash - 1);
					if (parentSlash != std::string::npos)
					{
						path = full.substr(0, parentSlash + 1) + "nbn-trace.log";
					}
				}
			}
			return std::ofstream(path, std::ios::app);
		}();
		if (!trace.is_open()) return;
		const double seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - traceEpoch).count();
		trace << std::fixed << std::setprecision(3) << seconds << ' ' << text;
		if (text.empty() || text.back() != '\n') trace << '\n';
		trace.flush();
	}
}

#undef LOG_INFO
#undef LOG_ERROR
#define LOG_INFO(msg) do { std::ostringstream _log_ss; _log_ss << msg; \
	ResearchProbeRuntime::Log(ResearchProtocol::LogLevel::Info, _log_ss.str()); \
	AppendNbnTraceLine(_log_ss.str()); } while(0)
#define LOG_ERROR(msg) do { std::ostringstream _log_ss; _log_ss << msg; \
	ResearchProbeRuntime::Log(ResearchProtocol::LogLevel::Error, _log_ss.str()); \
	AppendNbnTraceLine(_log_ss.str()); } while(0)

// Authoritative Learn A Song Note by Note controller.
//
// This is a direct application of the shipped Guided Experience lesson hold to the
// GamePlaysongLAS owner, established by offline decompilation of the captured live
// .text image (see docs/investigations/note-by-note-native-sync.md):
//
// - The lesson tick and the LAS tick are near-identical and neither contains a freeze
//   gate. The lesson hold works because FreezeOnTag core 0x474A80 performs exactly two
//   operations against shared state: PlayerSong Stop_TMusic (vtable +0x24, 0x44C130)
//   and the owner's five-clock setter (vtable +0x50) at the frozen epoch.
// - Stop_TMusic writes the 16-bit value 0x100 at PlayerSong +0xD9: it clears the
//   publication gate +0xD9 AND sets +0xDA. The PlayerSong component tick 0x44C7F0,
//   the only native re-publisher of +0xD9, exits immediately while +0xDA is nonzero.
//   That is why Stop latches the hold and the rejected Pause_TMusic trial did not.
// - While held, the tick keeps running at the frozen time: slot +0x94 re-publishes the
//   held owner clock to the render-facing timer each frame and native scoring 0x7E2880
//   keeps evaluating the selected note's own hit decision 0x7E2640. Input detection
//   therefore stays live during the hold, exactly as the shipped lesson behaves.
// - Release mirrors ResumeFromTag minus the lesson-only tag seek: mark active scoring
//   notes through 0x7E8920, then invoke coordinated slot +0x54 at the requested epoch. That
//   slot rebuilds the timeline bounds, calls slot +0x50, and sends PlayerSong the same
//   native play packet (0x44BED0) that every Riff Repeater loop restart uses to restart
//   music. The lesson-only speed restore 0x472A20 is intentionally not called because
//   this controller never modifies the native rate or the +0x348 component flag.
// - Dense successors rebuild directly at the next held epoch and wait for that play packet
//   to refresh PlayerSong's transferred fretboard target. Only then is Stop_TMusic
//   re-latched at the same epoch. Advancing the five owner clocks alone moves incoming
//   note geometry but leaves the stopped PlayerSong target on the committed prior note.
// - The hold is late-only: native scoring gets the full approach and can commit an
//   on-time note without transport intervention. Only an unresolved note that reaches
//   record time minus the engine compensation is stopped, and the current authoritative
//   update time becomes the held epoch so establishing the hold does not seek backwards.

namespace
{
	// Heap-corruption tripwire (2026-08-26 section-loop crash forensics). The loop
	// reproducer dies of 0xc0000374 minutes after the corrupting write, so the log
	// could only ever show the trigger context, never the corruptor. This validates
	// every process heap at the hold-lifecycle seams, so a corrupted heap is DETECTED
	// at a named seam and the corruptor is bracketed between two seam names. First
	// failure logs loudly and latches (a corrupted heap stays corrupted; repeats
	// would bury the first detection). SEH-wrapped because HeapValidate on corrupted
	// metadata can itself fault. Cost is a few ms per checkpoint, only at hold
	// events; probe_heap_check_off is the revert if it ever shows in frame pacing.
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
	// GamePlaysongLAS stores Rocksmith's selected Riff Repeater bounds twice. Native
	// range writers 0x7E87F0/0x7E9130 populate both pairs from the selected phrases.
	// These MARCH: slot +0x54 (COORDINATED_REBUILD) recomputes them as
	// [min(heldEpoch, ref), max(heldEpoch, ref)] on every owned restart, so +0x3C0
	// (start) drifts to the held epoch while +0x3C8 (end=max=ref) stays pinned to the
	// section's authored end. Verified live 2026-08-28: +0x3C0=18.089(heldEpoch),
	// +0x3C8=18.384(entry[0].end). The stable source is the phrase-section grid below.
	constexpr uintptr_t LAS_SECTION_START = 0x3C0;
	constexpr uintptr_t LAS_SECTION_START_MIRROR = 0x3C4;
	constexpr uintptr_t LAS_SECTION_END = 0x3C8;
	constexpr uintptr_t LAS_SECTION_END_MIRROR = 0x3CC;

	// The STABLE authored phrase-section grid - Rocksmith's own Riff Repeater section
	// list, loaded once per song and never rewritten during play (unlike +0x3C0/+0x3C8).
	// owner+0x78 -> a container whose std::vector spans [+0xF4 begin, +0xF8 end), stride
	// 0x58; each entry carries authored start at +0x24 and end at +0x28 in absolute
	// seconds, as a half-open chain (end[i] == start[i+1]). Verified live 2026-08-28
	// against the running song: 56 sections, 11.714..260.938s, and the owner+0x78 root
	// matched the _DAT_0135F54C global chain (not the lesson-mode chord-display trap).
	// See docs/investigations/note-by-note-bug-tracker.md.
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

	// Rocksmith's continuous pitch trackers, the input behind the shipped bend lessons.
	//
	// Recovered in docs/investigations/note-by-note-motion-note-tracker.md by resolving
	// Technique_MotionNote_IsLocated (0x00404E10) and _GetLocation (0x00404E70) through
	// the Lua registration table, then decompiling both. They report a *fractional* MIDI
	// pitch, which is the whole reason they matter here: the loudest-played-note query is
	// an integer, so a bend sitting at 67.9 reads as a whole semitone away from its target
	// and only registers once it overshoots.
	//
	// Confirmed live: during a bend these records traced a continuous trajectory rising
	// 66.098 to 68.700, and the idle value is exactly the -1.0f sentinel the native
	// failure path loads. Confirmed again in ordinary Learn a Song play with Note by Note
	// disabled, which is what makes them usable here at all: they are not lesson-only
	// machinery, so nothing has to be registered, frozen or resumed to read them.
	//
	// Read-only. Nothing here calls into the game or writes to its memory.
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
	// Ubisoft's own acceptance band, not a tolerance invented here.
	// eseratingfunctionbends.lua defaults sUnderbend and sVariance to 0.5 each and treats
	// "we hit our target pitch" as bend_pitch_to_hit - sUnderbend <= pitch < + sVariance.
	// TIGHTER than the base game on purpose (0.3, not the Lua 0.5): Philip wants NBN bends to
	// feel BETTER than base game, and 0.5 under a 2-semitone bend accepts a ~1.5-semitone
	// half-bend (measured live: barely-bent accepted at 0.456 under). 0.3 still forgives the
	// top wobble (a real bend rests 0-0.2 under) but rejects a bend that never reached pitch.
	constexpr float BEND_UNDERBEND_SEMITONES = 0.3f;
	constexpr float BEND_VARIANCE_SEMITONES = 0.5f;
	// Overbend allowance (Philip, 2026-08-25 late: "I had to hold it at max bend
	// for a while before it would allow it"). The lesson's symmetric 0.5 band is
	// a TEACHING band - overbending is an error a bend lesson corrects. In song
	// play a player who bends past the target has reached it; the session log
	// shows a 1-semitone bend to 70 tracked at 71 refused until he relaxed back
	// into the half-semitone window. Overshoot up to this bound now counts as
	// reached (both in the tracker search radius and the accept ceiling); a
	// genuinely different note two or more semitones up stays excluded.
	constexpr float BEND_OVERBEND_ALLOWANCE_SEMITONES = 1.5f;

	// Rocksmith's note-detection engine, decoded in
	// docs/investigations/note-by-note-input-gates.md.
	//
	// This exists so a stalled hold can say which condition refused it instead of guessing.
	// Both native input queries are refused by the same two amplitude gates before either
	// looks at a pitch, and neither reports which one failed: they both just return -1. So
	// when the onset query and the integer fallback go quiet together, as they did for
	// 1440+ ticks in the 2026-08-16 run 3 capture, nothing in the log distinguishes "the
	// player is not playing", "the signal is below the engine's noise floor", "the pitch has
	// not settled across two analysis frames" and "we already consumed this value".
	//
	// The whole chain is plain memory reachable from the same root as the motion-note
	// trackers, so it is read through the guarded TryRead and costs no native call. That
	// matters here beyond tidiness: the onset query is *not* a read-only observation. It
	// writes the dedupe global 0x012F6920, so an extra diagnostic call to it would consume
	// the very edge the controller is waiting for. Nothing below calls it.
	//
	// The trackers live at arrangement + 0x04 and the detection engine at + 0x08, two
	// fields of one arrangement object.
	constexpr uintptr_t DETECTION_ROOT = 0x0135F57C;
	constexpr uintptr_t DETECTION_ARRANGEMENT_GUITAR = 0x10;   // kind 2 (bass) would be +0x14
	constexpr uintptr_t DETECTION_ENGINE = 0x08;
	constexpr uintptr_t DETECTION_DETECTOR = 0x04;
	constexpr uintptr_t DETECTOR_CURRENT_NOTE = 0x5F4;         // what 0x48E5F0 returns
	// The double the decompiled window leaves (0x4E5DB0 and 0x4E5B80) gate every
	// note-window query on: windowStart <= clock <= windowEnd or refuse without
	// reading input. Not the transport clock; whether the two diverge during a
	// frozen hold is exactly what the chord-window logs measure.
	constexpr uintptr_t DETECTOR_ANALYSIS_CLOCK = 0xD08;
	constexpr uintptr_t DETECTOR_GATE_QUALITY = 0xD38;
	constexpr uintptr_t DETECTOR_RING_BUFFER = 0xDB8;
	constexpr uintptr_t DETECTOR_RING_INDEX = 0xDBC;
	constexpr uintptr_t DETECTOR_RING_CAPACITY = 0xDC0;
	// Live count of valid ring frames (state+0xDC8) and each frame's own capture timestamp
	// (frame+0x730, double seconds). The native onset-window scan (FUN_004E6A70/004E4B60)
	// bounds its walk by these live timestamps, NOT by the frozen detector clock - which is
	// exactly why it keeps working while NBN freezes the transport (port, 2026-08-29).
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
	// Tier-0 high-fret vouch (Philip 2026-09-05). Native's own quality gate refuses a high,
	// thin-string note (B12/MIDI71, green 19, high-e 15) whose fundamental the tier-0 DSP still
	// reads cleanly - measured quality ~35-50, just under the ~50 gate, with the level fine. On a
	// sustained hold where native scored in this band, tier-0's 5x-dominant fundamental
	// confirmation is a reliable accept (the flickery ML rescue is not). The floor keeps it from
	// vouching on a dead/absent signal (quality < ~35 is not a real read). Streak-gated for the
	// documented tier-0 top-of-note wobble; the sub-harmonic guards inside Tier0ConfirmsExpected
	// stop a lower note's harmonic from confirming.
	constexpr float TIER0_VOUCH_QUALITY_FLOOR = 35.0f;
	constexpr int32_t TIER0_VOUCH_STREAK_TICKS = 2;
	// Only bounds the ring indexing against a corrupt read; the capacity is read, not assumed.
	constexpr int32_t DETECTOR_RING_MAX_CAPACITY = 4096;
	// Native-drive step 4 (decomp-nd-query-core-2026-08-25): the ND
	// "is this note being played" seam, the lesson engine's own rating
	// primitive (NDIsNoteBeingPlayed -> 0x48DE10 -> 0x4E9590 -> 0x4E6380).
	// The detector keeps a current-sounding table: count at +0x6A4, entries at
	// +0x604 with stride 8 {int32 midi, float strength}; the native predicate
	// is "expected midi present with strength above the float threshold at
	// 0x01199DD4" (reads 5.0 live). Pure reads end to end, live-verified
	// 2026-08-25 night: [arr+0x04] == [[arr+0x08]+0x04] == the detector the
	// probe already resolves, and the deaf-signature garbage (MIDI 88-92)
	// sits at strength ~2.3, naturally below the bar. The probe replicates
	// the scan instead of calling the Lua-adjacent wrapper. The duration gate
	// is the lesson recipe's ~0.18s sustained-sounding requirement.
	constexpr uintptr_t DETECTOR_SOUNDING_TABLE = 0x604;
	constexpr uintptr_t DETECTOR_SOUNDING_COUNT = 0x6A4;
	constexpr uintptr_t ND_STRENGTH_THRESHOLD_GLOBAL = 0x01199DD4;
	constexpr float ND_STRENGTH_THRESHOLD_FALLBACK = 5.0f;
	constexpr double ND_SOUNDING_HOLD_SECONDS = 0.18;
	constexpr int32_t ND_SOUNDING_MAX_ENTRIES = 64;
	// Ships ENABLED (Philip, 2026-08-25 night: features under test run
	// enabled; the toggle is an emergency revert, nd-accept-off).
	volatile bool isNdAcceptEnabled = true;

	// A stalled hold reports its detector state at this cadence. Deliberately wall-clock
	// rather than tick-counted: a tick-counted cadence would itself be frame-coupled, which
	// is the thing under suspicion.
	constexpr double DETECTOR_SAMPLE_INTERVAL_SECONDS = 1.0;
	// A pick attack jumps past this between two consecutive scoring ticks; the natural
	// decay moves ~1 dB/s, so even this low threshold is two orders of magnitude above
	// the tick-to-tick decay. It was first set at 6 dB, and the 2026-08-18 orange-14
	// capture showed an accepted pick over a string bed already ringing at -20 dB
	// producing no spike line at all: a re-pick over a loud ring adds only a few dB,
	// which is exactly the case the capture exists for.
	constexpr float DETECTOR_SPIKE_JUMP_DB = 2.5f;
	// Post-spike burst length: at the observed 30-60 ticks/s this records roughly a
	// quarter second of the attack transient, enough to see whether quality or the
	// ring pitch ever responds to the pick.
	constexpr int32_t DETECTOR_SPIKE_BURST_TICKS = 12;
	// #52 bend-release cascade guard: for this long after a bend commits, a follower must show
	// attack energy (a spike) to accept an onset. A bend release rings the string down through
	// every follower's pitch and the onset detector fires on the loud decaying ring, skipping
	// the next notes with no pluck (2026-08-27: a green bend at 81 skipped followers at 69/74/71
	// off the ring). A decaying release never spikes the meter; a real repluck does. Covers the
	// release decay; a genuinely fast repluck still spikes and passes regardless of this window.
	constexpr double BEND_RELEASE_GUARD_SECONDS = 0.4;
	// Onset-frame attack evidence for chord holds (2026-08-24 "five strums" session;
	// see the cursor state further down). The level floor rejects the dead-input
	// session's garbage onset blips (spikes at -80..-88 dB with loudest reading MIDI
	// 86-95); every real strum frame that session logged sat at -52 dB or louder.
	// The frame minimum keeps the previous target's committing strum (whose attack
	// spans a couple of ring frames around the relatch) from counting as evidence
	// for THIS hold: ~10 frames is ~100 ms at the observed ~95 ring frames/s.
	constexpr float ONSET_EVIDENCE_LEVEL_FLOOR_DB = -65.0f;
	constexpr uint32_t ONSET_EVIDENCE_MIN_FRAMES = 10;
	// Rising-edge requirement for onset-flag evidence (2026-08-29, issue #58: a chord
	// that rings out was satisfying repeat-strum sections with no re-pick). The detector
	// stamps its onset flag on a loud sustained ring too, so the flag alone is not proof
	// of a fresh attack. A real re-pick INJECTS energy - the flagged frame's level rises
	// above the ring just before it - while a decaying ring (or a prior strum's falling
	// tail) sits flat or falling. Require that rise, measured at ring-frame resolution
	// (~95/s) against the frame ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES earlier. This is the
	// finer-grained cousin of the level-spike gate: it keeps the onset-flag path's whole
	// reason for existing (a soft re-strum over a loud ring moves the tick-to-tick meter
	// less than DETECTOR_SPIKE_JUMP_DB, so the coarse gate misses it) while refusing a
	// ring, because at frame resolution a pick transient is a sharp local rise even when
	// the tick average barely moves. RISE_DB is a first, conservative value; the reject
	// path logs the measured rise so the threshold can be locked from a live pass.
	constexpr int32_t ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES = 4;
	constexpr float ONSET_EVIDENCE_RISE_DB = 1.5f;
	// Bound per-tick scan work when the cursor falls far behind (a hitch between
	// scoring ticks); the newest frames are the ones trusted.
	constexpr int32_t ONSET_SCAN_MAX_FRAMES_PER_TICK = 120;
	// Raw-detector bend acceptance. While the transport is frozen the motion-tracker
	// container never holds an active record (registration is transport-driven; proven
	// by the 2026-08-18 orange-14 capture: count=10 active=0 for the entire hold), and
	// the gated queries return -1 for a bending pitch because a continuously moving
	// pitch rarely reaches the 50-point quality gate. In that capture the ungated
	// current-note field read the exact bend target at quality 37 the moment the bend
	// arrived, while the quality-0 noise floor read arbitrary pitches (43, 45, 90-92).
	// The floor sits between those two regimes, and the streak requires the target to
	// persist across consecutive ticks so one noise sample cannot accept.
	constexpr float DETECTOR_RAW_BEND_QUALITY_FLOOR = 15.0f;
	constexpr int32_t DETECTOR_RAW_BEND_STREAK_TICKS = 3;
	// An unambiguous "green": a credible raw read this strong is accepted on the spot
	// rather than waiting for the streak, because a 1-semitone HALF bend only flashes its
	// target briefly at high quality and the streak rarely sustains through the wobble
	// (Philip: "I saw it go green, that should indicate it's correct"). The #52 bend-release
	// guard still excludes a decaying ring, so this cannot cascade.
	constexpr float DETECTOR_RAW_BEND_STRONG_QUALITY = 70.0f;
	// Minimum consecutive on-target ticks before EITHER instant-accept path (the strong-green
	// Holding accept and the rising input-release accept) may commit a bend (Philip 2026-09-05:
	// "some bends activated without me doing the full bend"). A single momentary read inside the
	// target band - detector noise, a harmonic, a brief overshoot-and-back on a partial bend -
	// no longer counts; the bent pitch must actually be present for this many ticks. Small (a
	// real held bend clears it in tens of ms, and the top-wobble streak-hold above keeps it from
	// resetting), so a genuine full bend still feels immediate while a flash-through is rejected.
	// The >= DETECTOR_RAW_BEND_STREAK_TICKS fallback is unchanged.
	constexpr int32_t BEND_ACCEPT_MIN_HOLD_TICKS = 2;
	// Spike re-attack acceptance. The 324.161s trace capture (2026-08-18, string 4
	// fret 15) is the blocker's smoking gun: the pick produced a 3.6 dB spike and
	// THIRTEEN consecutive ticks of quality 92-100 with the raw current-note reading
	// exactly the expected pitch, and the native onset never fired (its internal
	// frame comparison depends on the ring pitch fields, which read unrelated values
	// through the whole attack). So the pick is accepted from what is actually
	// visible: a level spike opens a short window, and inside it the raw pitch must
	// equal the expected note with BOTH native gates passing on consecutive ticks.
	// The spike requirement is the arming guard the handover asked for: a decaying
	// ring alone can never commit, because it never jumps.
	constexpr int32_t REATTACK_WINDOW_TICKS = 15;
	constexpr int32_t REATTACK_STREAK_TICKS = 3;

	// ML can confirm a held note from distinct post-target audio observations.
	volatile bool isMlStuckRescueEnabled = true;
	constexpr float ML_STUCK_RESCUE_CONF = 0.45f;
	// Bends want a LONGER held streak and tier-0 ONLY (no ML): the companion's 1.2s / ~5 Hz
	// window smears a dynamic bend and reads the target early (Philip: "activated before they
	// got to their pitch"), while tier-0's 5x-dominance guard will not confirm until the pitch
	// actually settles at the target. The longer streak rejects a fast glide-through the target.
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
	// The native hit decision packs these two floats into its detection query
	// (decomp-native-hit-decision-2026-08-24.txt): every window leaf receives
	// [eventTime + startDelta, eventTime + endDelta] and refuses outright unless
	// the detector clock lies inside that span, before consulting any input.
	constexpr uintptr_t NOTE_DETECT_WINDOW_END_DELTA = 0x94;
	constexpr uintptr_t NOTE_DETECT_WINDOW_START_DELTA = 0x98;
	// The chord template the evaluator judges the note against (query slot [7]).
	constexpr uintptr_t NOTE_CHORD_RECORD = 0x30;

	// SNG chord template, as the evaluator itself walks it (decomp 2026-08-24:
	// QueryNoteWindow's chord branch reads six per-string frets at +0x04 with
	// values < 0x1A playable, and the noteway indexes the template array with a
	// 0x48 stride, which is this layout including the trailing 32-byte name).
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
	// A hold that cannot be satisfied releases itself rather than locking the game.
	//
	// Several distinct causes have produced an unsatisfiable hold: a note beyond the loop
	// end on the first pass, before any turnover has revealed the boundary, and the note
	// carrying record+0x04 = 1 whose meaning is still unknown. This is a backstop for all
	// of them, including causes not yet found, and it is deliberately generous so that
	// ordinary slow or repeated attempts are never cut short. The note is not accepted:
	// the transport is released and the song continues, which is a worse outcome than
	// playing the note but a much better one than freezing.
	// Twenty seconds was far too short. It fired four times in succession while Philip was
	// simply not playing, which a practice tool must tolerate: stopping to read the
	// highway, think, or look away is normal use, not a stuck hold. This is long enough
	// that only a genuinely unsatisfiable hold reaches it.
	constexpr double HOLD_SAFETY_RELEASE_SECONDS = 60.0;
	// Chords get a shorter safety budget. A refused chord hold is freed by this
	// timeout alone (strumming never resets the progress anchor, only legato-run
	// progress does), and the 2026-08-24 beta showed a stalled chord reads as the
	// game hanging: 60 seconds of frozen transport is far worse than surrendering
	// one chord after fifteen.
	constexpr double CHORD_HOLD_SAFETY_RELEASE_SECONDS = 15.0;
	// Double-strum guard (2026-08-24 autopsy): a chord committed at the section
	// start restarts PlayerSong at an epoch the bootstrap reads as a section
	// change, which clears consumedRecords and re-selects the just-played chord,
	// forcing a second strum of an already-accepted chord. The last committed
	// chord survives that reset for this many wall-clock seconds; a real loop
	// replay arrives later than this and re-targets the chord normally.
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
	// The bend amount in semitones.
	//
	// Found by diffing a live bend record against every plain note in the same section:
	// this offset is one of only a handful that differ, and it read 2.0 on orange fret 14,
	// which is exactly the whole step from MIDI 69 to 71 that Philip played. The adjacent
	// fields look like a bend curve, a time at +0x44 inside the note's sustain window and
	// the value repeated at +0x48, rather than a single scalar.
	//
	// It matters because bend acceptance was a range, expected+1 through expected+3, so it
	// fired the moment the pitch crossed into that window and a bend could complete
	// mid-bend. Worse, 71 is also the pitch of the following note, green fret 12, so the
	// bend consumed it and the next target could never be satisfied.
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
	// Chord successors take the dense rebuild out to a wider spacing. The
	// 2026-08-24 chord chain ran at 0.28-0.34s gaps - every one just over the
	// single-note boundary - so every chord went release -> restart -> re-freeze,
	// and each restart re-runs the visual churn around the strum line. Jumping
	// straight to the next chord skips only the strum tail between two holds,
	// which is the right trade for note-by-note practice.
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

	// Bootstrap state. The section START is confirmed by the delayed-identity
	// rollback-boundary bootstrap (restored 2026-08-28): a natural loop turnover jumps
	// the transport clock backward; the endpoint that a live note record matches by BOTH
	// authored time and native event time is the true loop start, and greyCutoff is written
	// from it EXACTLY ONCE per section. It is never re-read from the owner's +0x3C0 field,
	// which the game marches to each restart epoch (that marching was the regression
	// introduced in ee3c4d0 - it drifted greyCutoff forward and greyed the section's own
	// early notes). The section END is latched once from +0x3C8 at confirmation, before any
	// owned restart marches it, and likewise never re-read. Rollbacks after confirmation
	// only advance the epoch and clear consumedRecords; they never touch either boundary.
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

	// The cached authored section timeline for the current song (Philip's "floating
	// timeline controller"). Built once per owner from the stable phrase-section grid and
	// used to latch the loop bounds directly, so arming no longer waits for a transport
	// rollback or depends on the churning +0x3C0/+0x3C8 mirror. Immutable for the song.
	struct TimelineSection { float start; float end; };
	std::vector<TimelineSection> timelineSections;
	void* timelineOwner = nullptr;
	// The grid identity the cache was built from. The owner+0x78 container (and its vector
	// begin/end) is repopulated when a NEW SONG loads - sometimes reusing the same owner
	// pointer - so keying the cache on the owner alone leaked the previous song's sections
	// into the next song (Philip: Cliffs' 56 sections cached for a 14-section Eb song ->
	// carried section / teleport, and out of range it would crash). Re-validating these each
	// call rebuilds the timeline per song lifecycle.
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
	// The chord target's tones as the native matcher last evaluated them (physical MIDI
	// frame, from note+0x30). Tagged with the record they were read for so the research
	// state never reports a previous chord's tones for the current target. Fed for the
	// fake-guitar harness; read only.
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
	// Set when the selected record is a bend CHILD, that is a continuation of a bend whose
	// parent has already been played and accepted.
	//
	// Such a record cannot be satisfied by a pick. Rocksmith authors a bend as a parent and a
	// child on the same string and fret a fraction of a second apart, the player performs one
	// continuous gesture, and by the time the child is selected the string is already bent and
	// ringing at the target pitch with nothing left to pluck. Requiring an onset at the unbent
	// pitch asks for a note that no player would produce, and the hold then runs to the safety
	// release. Philip hit exactly this on green 15: parent mask 0x08803000 accepted from a pick
	// at 74 and a bend to 76, child mask 0x10813000 selected 88 ms later, then 40 seconds with
	// no input at all (see docs/investigations/note-by-note-input-gates.md).
	//
	// The child is still a target rather than skipped, because skipping bend children was
	// observed to make a bend accept itself the moment its parent was played, reproduced on a
	// second fret. So the fix is to change what satisfies it, not whether it exists: the
	// continuous pitch tracker can see the bend being held, and a bend produces no new pick
	// attack anyway, which is the same constraint hammer-ons already live under.
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
	uintptr_t visualGroupRecords[ResearchProtocol::NoteByNoteState::MaxVisualGroup] = {};
	// The same gesture as string/fret pairs, for the neck-diagram gate, which receives
	// coordinates rather than a note pointer.
	int32_t visualGroupStrings[ResearchProtocol::NoteByNoteState::MaxVisualGroup] = {};
	int32_t visualGroupFrets[ResearchProtocol::NoteByNoteState::MaxVisualGroup] = {};
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
	// A legato run is played as one gesture, so it is accepted as one note.
	//
	// Skipping the continuations as targets stopped asking the player to pick them twice,
	// but it also stopped the transport waiting for them at all: the pick alone completed
	// the gesture, the timeline advanced past the whole run, and the continuation collapsed
	// visually into the following note, which may be on another string. Philip described
	// exactly this, having never played the continuation while the feature accepted it.
	//
	// The run is now held as a unit. The first note needs its pick attack, and each
	// continuation is then confirmed by pitch, because a hammer-on or pull-off produces no
	// pick attack for the onset query to latch. Nothing is committed until the whole run
	// has been played, so an incomplete or wrong run simply does not advance.
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

	// ---- Detection strategy per technique (Philip 2026-09-05, blend rework) ---------------
	// Note by Note ships as ONE tuned engine. Blend (default) fuses native + tier-0 + ML:
	// each can RESCUE the other where it is weak, and neither VETOES the other unless it is
	// confidently right. NativeOnly / MlOnly force a single engine for testing and isolation.
	// Default Blend; the dev swap arrives over the probe-command channel (no GUI). See
	// docs/designs/nbn-detection-blend.md.
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

	// Native single-note accept needs an ML co-sign only in the strict MlOnly test mode; in
	// Blend and NativeOnly native's own accept stands (Blend adds ML as rescue+guarded veto,
	// not a required co-sign, so ML latency or a weak ML read never stalls a correct note).
	bool NativeAcceptNeedsMlCosign() { return CurrentTechniqueStrategy() == DetectionStrategy::MlOnly; }

	// Live native-vs-ML agreement sampler + per-session CSV. Defined further down, next to the
	// shadow loggers where the passive native/ML queries live; forward-declared here so the
	// hold-phase sampler can call it.
	void SampleDetectionComparison();

	// Corner-HUD snapshot, updated by SampleDetectionComparison and read by GetResearchState.
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
	// ND sounding-table bend evidence (2026-08-25 night, Philip: "you need to
	// hold the pitch for like 43 seconds rather than a bend"). While frozen
	// the motion trackers are unregistered and the gated integer query needs
	// settled quality, so a bend resting just under target stalled until held
	// dead-on. The sounding table lists the CURRENT pitch as integer MIDI with
	// an engine strength: a below-target sighting arms the approach (the sweep
	// passing through), a target-or-over sighting while armed reaches - two
	// consecutive ticks so one stray entry cannot complete a bend.
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
	// Chord holds (#43, 2026-08-22/23). A strum has no single expected pitch, so a held
	// chord is accepted by EVALUATING the game's own hit decision instead of blocking it:
	// the frozen-transport question ("does the native chord evaluation still run against
	// a stopped clock?") is answered by the (NBN LAS CHORD) logs. Bridge-toggleable
	// (chord-holds-on/off) so a failed experiment can fall back to the old play-through
	// behavior live. History says the odds are good: chords scored natively under the
	// superseded draw-path architecture.
	volatile bool areChordHoldsEnabled = true;
	// Flow-until-miss (docs/designs/nbn-flow-until-miss.md, Phase 2 REVISED). DEFAULT OFF -
	// PARKED for the 2026-09-05 stable release (Philip). The mechanic works: moving the hold
	// boundary from BEFORE the strike line (selectedRecordTime - the ~0.053 engine
	// compensation) to selectedRecordTime + flowLateGraceSeconds lets an on-time note commit
	// NATURALLY while Armed (the "Natural commit before any hold" path, transport never
	// stops), so dense sections play straight through instead of being sliced into per-note
	// audio fragments, and the freeze fires only on a real miss. But it changes the fragile
	// freeze/epoch core and has not been hardened, and the larger speed-up/slow-down timeline
	// work it belongs with is parked, so the release ships the proven freeze-per-note
	// behavior. probe_flow_on turns it back on live for continued development; the whole flow
	// mode (this + the tempo ramp) comes back together, tested, after the release.
	volatile bool isFlowUntilMissEnabled = false;
	// How far past a note's record time the transport keeps flowing before the boundary
	// freezes on it (seconds). Bounded above by the native detect window's exit
	// (recordTime + ~0.3): the frozen clock must still sit inside the note's window or the
	// native hit decision treats it as dead, so this stays comfortably under that. Start at
	// half the late window; bridge-settable (probe_flow_grace <sec>) so tuning needs no
	// rebuild.
	volatile float flowLateGraceSeconds = 0.15f;
	// Verbose trace toggle, DEFAULT ON (Philip, 2026-08-29: "I'm debugging more than I'm
	// playing" - a default-off flag silently missing the logs when something happens
	// catches us out, which is worse than the FPS cost). The per-tick trace logs (LAS
	// DETECT, BEND FEED, CHORD WINDOW, the chord eval-false + sounding-table dump, and the
	// native-matcher observe call) are ON by default; `probe_verbose_off` is the revert
	// when playing for fun (higher FPS), flipped live over the probe-command channel with
	// no game restart. Event logs (commits, accepts, holds, onset evidence) are never
	// gated by this - only the high-frequency spam is.
	volatile bool verboseTrace = true;
	// Safety-release toggle, DEFAULT OFF (Philip, 2026-08-25): the timed release
	// read as an accept from the player's seat (#58) and masked every stuck-hold
	// refusal it fired on. Off, a stuck hold stays held and logs its refusing
	// state every budget interval; N (Stop) releases it. safety-release-on|off.
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
	// Carry-over-ring freshness for the matcher accept (Philip 2026-08-29: a repeated
	// chord's previous strum keeps ringing, so the spectral matcher reads the full chord
	// even when the player picks only one string of it). The matcher must have fallen
	// BELOW its YES at least once since this target latched before a YES may commit, so a
	// fresh strum - not a continuous ring - is the only way to arm acceptance.
	uintptr_t matcherFreshnessRecord = 0;
	bool matcherFreshnessArmed = false;
	// #53 window slide. The decompiled hit decision refuses a held chord without
	// consulting any input once the detector clock (detector +0xD08) has left the
	// note's authored-time window (note +0x38 plus the +0x98/+0x94 deltas). While a
	// hold owns the transport the song clock is frozen but the detector clock is
	// not, so a passed window refuses forever - the accept-in-~130 /
	// refuse-in-1600+ split. The slide translates the window onto the clock for
	// exactly one original-decision call, authored width preserved, deltas restored
	// immediately after. Bridge-toggleable (chord-window-on/off) for a live A/B;
	// the (NBN LAS CHORD WINDOW) log doubles as the divergence measurement.
	volatile bool isChordWindowSlideEnabled = true;
	uint64_t chordWindowSlideCount = 0;
	std::chrono::steady_clock::time_point chordWindowLogAnchor;
	bool hasChordWindowLogAnchor = false;
	// #54: bare-0x2 repeat strums hold like full chords. The play-through stopgap
	// was worse than the skipped strums it caused: two play-through records in a
	// row let the running transport overshoot the next chord's hold boundary,
	// which is what ended the 2026-08-24 beta session. The original chronic-stall
	// observation predates the spike gate and the chord safety budget, both of
	// which now bound a stalling repeat; toggle back (repeat-holds-off) if the
	// stalls return.
	volatile bool areRepeatStrumHoldsEnabled = true;
	// Double-strum guard state; see COMMITTED_CHORD_RESELECT_GUARD_SECONDS.
	uintptr_t lastCommittedChordRecord = 0;
	std::chrono::steady_clock::time_point lastCommittedChordAt{};
	// Attack evidence for chord acceptance: set by the spike detector OR the ring
	// onset-flag scan below while holding, cleared at hold establishment. Without it
	// the ringing PREVIOUS strum satisfies the evaluator when the next target is the
	// same chord (Philip's first chord session: ring-through auto-advance instead of
	// waiting for a fresh strum).
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
	// Onset-frame attack evidence cursor (2026-08-24 "five strums" session). The
	// level-spike gate alone cannot see a re-strum layered on a still-loud ring: the
	// meter sat at -34 dB from the previous chord's ring and fresh strums moved it
	// less than the 2.5 dB jump, so every true evaluator decision was discarded as
	// ring for the full 15 s safety budget (console: result=true onset=1 frames
	// refused while "decision is true but no attack" logged). The analysis ring
	// stamps an onset flag on attack frames themselves, which separates a fresh
	// attack from a ring at ANY absolute level. Frames are walked from this cursor
	// so none are missed between frame-coupled scoring ticks (~30/s against ~95
	// ring frames/s); only frames written at least ONSET_EVIDENCE_MIN_FRAMES after
	// the hold latched count, so the committing strum of the previous target cannot
	// bleed evidence into this hold.
	int32_t onsetScanRingIndex = -1;
	uint32_t onsetScanFramesSinceLatch = 0;
	bool hasOnsetScanAnchor = false;
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
	Research::MlConfirmationState mlConfirmationState;
	int32_t bendRescueStreak = 0;      // consecutive ticks tier-0/ML confirm the bend reached its target
	int32_t tier0VouchStreak = 0;      // consecutive ticks tier-0 confirms a high-fret note native under-scored

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
		return nativeMidi >= 0 ? nativeMidi + ResearchProbeRuntime::GetInputOnsetShiftSemitones()
							   : nativeMidi;
	}
	float ShiftNativePitchToDisplay(float nativePitch)
	{
		if (!kApplyInputOnsetShift) return nativePitch;
		return nativePitch >= 0.0f
			? nativePitch + static_cast<float>(ResearchProbeRuntime::GetInputOnsetShiftSemitones())
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

	// The Guided Experience "freeze" sound NBN dispatches before every single-note hold.
	// Philip (2026-09-04, first cable session): "silence and a thundering percussion noise
	// on every correct note". Two per-note sounds coincide today: this event, and the
	// music restart fragment (release play packet -> Stop_TMusic at the next hold). The
	// 2026-09-05 bridge A/B (probe_freeze_sound_off) confirmed by ear that THIS event is
	// the percussion, so it now ships silenced: default OFF removes it outright. The
	// probe_freeze_sound_on command is the revert if the prompt cue is ever wanted back.
	volatile bool isFreezePromptSoundEnabled = false;

	void PlayFreezeNoteTrack()
	{
		if (!isFreezePromptSoundEnabled)
		{
			LOG_INFO("(NBN LAS PROMPT) " << FREEZE_NOTE_TRACK_EVENT
				<< " skipped (probe_freeze_sound_off)." << std::endl);
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

		// Anchor on the section END (+0x3C8). It is the STABLE half of the marching pair -
		// the +0x54 writer sets it to max(heldEpoch, ref)=ref, so it stays pinned to the
		// loop's authored end and its own mirror (+0x3CC) agrees even while +0x3C0 churns.
		// Requiring only the end mirror (not both) is what makes this immune to the churn
		// that hung the old bootstrap.
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

		// The loop START. +0x3C0 marches during play (to the held epoch) and, crucially, is
		// NOT reliably rewritten when the player only extends the range rightward - so it
		// cannot be blindly trusted. Precedence:
		//   1. +0x3C0 when it reads clean (mirror agrees) AND snaps EXACTLY to a grid start
		//      at/before the end row. That is a fresh selection the game just wrote (a move,
		//      or an extend-left), so it is the authoritative new start.
		//   2. otherwise the caller's startHint - the previously latched start - because
		//      Philip's rule is the loop start does not change when you extend the range to
		//      the right (read left to right, the leftmost section is fixed). This keeps a
		//      5-section loop's start pinned instead of collapsing to the last phrase.
		//   3. last resort: a single-phrase loop (start = the end row's own start).
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

	// True when the stable RR section end (+0x3C8) has moved to a DIFFERENT grid section
	// than the one currently latched - i.e. the player navigated to a new Riff Repeater
	// section in Practice Selection. +0x3C8 is the ref (max) half of the marching pair, so
	// it does NOT drift during play; a change is a real re-selection, not restart churn.
	// This is what lets NBN auto-follow the selection without an N toggle (Philip: "the
	// latch should auto-update", "NBN should not turn off when enumerating RR sections").
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

	// Every tracker record, so the bend fallback stops being silent.
	//
	// The continuous trackers were adopted specifically to judge a bend as a trajectory, and
	// across twelve captures on 2026-08-17 the "Tracked pitch" line appeared twice: every
	// other bend, including both halves of a parent/child pair that otherwise worked, was
	// confirmed by the integer fallback `currentMidi >= expectedRunMidi`. That fallback is the
	// open-ended comparison the tracker was meant to replace, so the feature is running on the
	// thing it was supposed to fix and nothing says why.
	//
	// TryGetSoundingPitchNear can decline for four different reasons and reports one bool for
	// all of them: the pointer chain not resolving, a zero or implausible count, no record
	// being both active and located, or every located record sitting outside the band. Those
	// need different fixes, so they are separated here.
	//
	// Note the recorded open question this is most likely to answer. What registers a tracker
	// during ordinary play is not established, and the original confirmation that they run
	// outside lessons was captured with Note by Note *disabled*. If registration is driven by
	// the advancing transport, then a frozen transport means no tracker is ever registered for
	// the held note and the whole tracker path is unusable while holding. That would show here
	// as records present but never active or located.
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

	// Cached gate sample for the state feed. GetResearchState is polled up to three
	// times per rendered frame (highway refresh, overlay input line, bend meter), and
	// TryRead pays a VirtualQuery syscall per read - a fresh detector-chain read per
	// poll multiplies into real frame time on the already FPS-poor Debug host
	// (2026-08-24 second session: 16-21 FPS with audible ASIO crackle against the
	// prior session's 21-45). LogDetectorGates already samples every scoring tick;
	// the feed copies that sample instead of re-reading. The timestamp bounds
	// staleness: outside active scoring (menus, idle) the sample stops refreshing
	// and the overlay line withdraws rather than showing a stale or misleading
	// number (the pause menu legitimately quiets the input, which read as DEAD).
	DetectorGateSample lastStateGateSample = {};
	bool hasLastStateGateSample = false;
	std::chrono::steady_clock::time_point lastStateGateSampleAt{};

	// Input-present floor for gating the high-frequency trace logs (Philip, 2026-08-29).
	// The console buffer holds only ~9000 rows, so per-frame/per-tick idle spam (BEND
	// FEED, LAS DETECT) scrolls real events - a commit, a chord accept - out of it before
	// they can be read after the guitar is set down. Gate that spam on actual input.
	// Idle sits near -80 dB; real playing and its ring tail sit above -60, so the floor
	// keeps a decaying chord's diagnostics alive while silencing a put-down guitar.
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

	// Step-4 CHORD acceptance from the same table (2026-08-25 night, decisive
	// live sample: a real C5 strum put ALL THREE tones in the table at once -
	// 55:50.6 60:15.3 67:5.8 - so the #53 harmonic masking does not afflict
	// this seam). The high tone rides just above the 5.0 bar and decays below
	// it fastest, so the rule uses short per-tone memory: every playable tone
	// seen above threshold within ND_CHORD_TONE_MEMORY_SECONDS, and the group
	// held for the lesson duration. Freshness guard mirrors the single-note
	// lapse rule: a group already sounding at latch (the previous strum's
	// ring) must fall apart once before duration credit can begin - which is
	// what makes a fresh strum the only way to accept.
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
		const int ndInputShift = ResearchProbeRuntime::GetInputOnsetShiftSemitones();
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
		// The native ring-carryover answer (Philip, 2026-08-25 night: "maybe
		// its not needed when its native"): duration credit requires the
		// game's own fresh-attack evidence since THIS latch - the level-spike
		// gate or the analysis ring's onset stamp - exactly what the
		// evaluator path already requires. No invented lapse guards; a ring
		// cannot stamp an attack.
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

	// Pins the onset-evidence cursor to the ring's current write position. Called at
	// hold establishment and at every dense relatch, so only frames written AFTER the
	// latch can become attack evidence for the new hold.
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

	// The onset-flag half of the chord attack evidence (the spike gate is the other
	// half; see sawSpikeDuringHold). Walks every ring frame written since the last
	// scan and latches evidence when a post-latch frame carries the ring's own onset
	// flag at a credible level. Runs once per scoring tick from LogDetectorGates,
	// unthrottled, chord holds only.
	void TickChordOnsetEvidence()
	{
		uintptr_t ring = 0;
		int32_t writeIndex = 0;
		int32_t capacity = 0;
		if (!TryResolveAnalysisRing(ring, writeIndex, capacity)) return;
		if (!hasOnsetScanAnchor)
		{
			// The latch could not anchor (chain unreadable at that moment); anchor on
			// the first readable tick instead. Frames before this tick are lost to the
			// scan, which only delays evidence, never falsifies it.
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
			// Rising-edge gate (#58): the flagged frame must be louder than the ring a few
			// frames earlier, or this is a decaying ring / a prior strum's tail, not a
			// fresh re-pick. Reject on an unreadable baseline too - require positive proof.
			int32_t baseIndex = index - ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES;
			if (baseIndex < 0) baseIndex += capacity;
			float priorLevel = 0.0f;
			const bool haveBaseline = TryRead(ring
				+ static_cast<uintptr_t>(baseIndex) * DETECTOR_RING_STRIDE + RING_FRAME_LEVEL,
				priorLevel) && std::isfinite(priorLevel);
			const float rise = haveBaseline ? (level - priorLevel) : 0.0f;
			if (!haveBaseline || rise < ONSET_EVIDENCE_RISE_DB)
			{
				// One line per tick so a loud ring's many onset frames cannot flood the
				// log; the measured rise is what calibrates ONSET_EVIDENCE_RISE_DB.
				if (!loggedRingRejectThisTick)
				{
					loggedRingRejectThisTick = true;
					LOG_INFO("(NBN LAS ONSET EVIDENCE) Rejected onset flag as a ring (no"
						<< " rising edge): level " << std::fixed << std::setprecision(1)
						<< level << " dB, rise " << std::showpos << rise << std::noshowpos
						<< " dB over " << ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES << " frames"
						<< " (need >= " << ONSET_EVIDENCE_RISE_DB << "), "
						<< onsetScanFramesSinceLatch << " frames after latch." << std::endl);
				}
				continue;
			}
			sawSpikeDuringHold = true;
			LOG_INFO("(NBN LAS ONSET EVIDENCE) Fresh attack frame accepted as chord attack"
				<< " evidence: onset flag set at level " << std::fixed
				<< std::setprecision(1) << level << " dB (rise " << std::showpos << rise
				<< std::noshowpos << " dB over " << ONSET_EVIDENCE_RISE_LOOKBACK_FRAMES
				<< " frames), " << onsetScanFramesSinceLatch << " ring frames after the hold"
				<< " latched. The level-spike gate stayed silent (a re-strum over a loud"
				<< " ring moves the meter less than " << DETECTOR_SPIKE_JUMP_DB << " dB)."
				<< std::endl);
			break;
		}
		onsetScanRingIndex = writeIndex;
	}

	// The clock every decompiled window leaf gates on, resolved through the same
	// chain as the gate sample above.
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

	// The wall-clock tick of the last detector level spike, in ANY gate phase. The
	// flow-through fix (2026-09-01, Philip: "I had to wait until after the next note
	// arrived then wait a sec, or the pick got swallowed"): a pick played during the
	// dense rebuild happens while hit decisions are blocked, and by the time the next
	// hold establishes its attack is history - but its string is still RINGING. This
	// stamp lets hold establishment know an attack just happened and open the
	// raw-confirmation window on the sustain.
	ULONGLONG g_lastAttackSpikeTick = 0;
	// Per-note pipeline stamps for the flow-latency investigation (2026-09-01,
	// Philip: "lagging rather than snapping through at my pace"): armed->adopt is
	// the detect+accept+commit+release+rebuild span for one note; adopt->armed is
	// the native transition (play packet + relatch). Logged as (NBN FLOW) per note.
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

		// Dedupe unstick (live stall, 2026-08-22: the open low E at a loop rollback).
		// The controller consumed the expected pitch's edge without committing (the
		// rollback fault landed in between), and the value-dedupe then suppressed every
		// repluck of the same pitch indefinitely - the exact hazard the input-gates
		// investigation named. If a hold spends a sustained run of ticks refusing
		// "already-consumed" for the EXPECTED pitch with both gates passing, reset the
		// dedupe global so the next query re-reports it.
		//
		// SPIKE-GATED (same day): the reset additionally requires attack energy during
		// the streak. A strong sustain CAN hold both gates open past the threshold - a
		// just-released bend rings loudly at exactly the pitch the ticket-#52 seeding
		// parked in the dedupe - and an ungated reset would re-report that ring and
		// auto-commit the follower. A decaying ring cannot spike the level meter; a
		// stuck player replucking does, every attempt. Spikes are the same evidence the
		// same-pitch acceptance paths already trust.
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
			// A spike ALREADY proves a real re-pluck (a decaying ring cannot spike the
			// meter), so the long 40-tick (~1.5s) wait was redundant caution and is what
			// forced the loop's first note (open E) to be played twice: its ring decayed
			// below the level gate before 40 stuck ticks accrued, so the streak kept
			// resetting and the first pluck never unstuck the carried-over dedupe.
			//
			// Unstick on the FIRST stuck tick that carries spike evidence, not four. Both
			// conditions here already prove a genuine fresh attack of the wanted note:
			// stuckOnExpected requires the pitch to have SETTLED onto expectedMidi (so the
			// transient is already past - the four-tick settle wait was for a state we have
			// already reached), and the spike gate still blocks a decaying ring /
			// bend-release carry-over (those cannot spike). Waiting four more ticks
			// (~130 ms at 30 fps) only disregarded fast same-pitch repeats: the note
			// advanced before the streak accrued, so the pick was silently eaten (Philip:
			// "had to pick some notes multiple times as though they were disregarded
			// because I played too quickly").
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
				// Chord holds (#43): attack evidence for the held chord's acceptance.
				// Cleared at hold establishment, so only a strum that happened DURING
				// this hold can satisfy the chord gate - the previous strum's ring
				// (and its spike) belong to the previous target. The minimum hold age
				// exists because the previous strum's envelope is still rising or
				// wobbling when the next hold latches a few hundred ms later, and one
				// such wobble read as a spike and passed a same-chord ring through
				// (Philip, 2026-08-24 second session).
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

		// The onset-flag half of the attack evidence, cursor-scanned so no ring
		// frame is missed between scoring ticks. Unthrottled like the spike tracker
		// above; skipped once evidence is latched (it clears at the next hold).
		// Runs for EVERY hold since the ND acceptance gates on fresh-attack
		// evidence (2026-08-25 night, Philip: the native answer to ring
		// carryover is the game's own attack stamp, not an invented guard).
		if (gatePhase == GatePhase::Holding && !sawSpikeDuringHold)
		{
			TickChordOnsetEvidence();
		}

		// Hold-phase shadow sampler (2026-09-01 late): the accept/reject shadow points
		// never fire for a WRONG note - the upstream level/quality gates and the bin
		// matcher refuse it before either is reached - so the reject side of the tier-1
		// dataset stayed empty no matter how many wrong notes were played. While a hold
		// has live input, sample the raw-route evidence and the companion's opinion with
		// the expected note alongside; offline comparison then sees the wrong-note
		// moments too. Deliberately independent of verboseTrace: this is the tier-1
		// dataset, not diagnostic spam, and its own throttle keeps it to ~3 lines/s.
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

		// Live native-vs-ML agreement for the corner HUD and the comparison CSV. Unlike the
		// tier-0/tier-1 diagnostic shadow above this is not _DEBUG-gated - Philip plays in
		// Release, where the A/B has to be visible. Same ~3 Hz throttle so it stays cheap.
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
		ResearchProtocol::MlNoteEvidence evidence;
		if (expectedMidi == previousExpectedMidi && !sawSpikeDuringHold)
		{
			mlConfirmationState.Observe(evidence);
			return false;
		}
		ResearchProbeRuntime::QueryMlNoteEvidence(expectedMidi, ML_STUCK_RESCUE_CONF,
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

		// Attribution guard (2026-08-25 night): a spike observed in a hold's
		// first ticks is the PREDECESSOR's attack envelope still rising across
		// the latch, not a re-pick of this target - live: a chord committed and
		// its strum tail spiked tick 1-2 of the successor single's hold, whose
		// expected pitch the chord ring carried at quality 99 (accepted in 4
		// ticks, no pick). A genuine re-pick lands later than the transient.
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

		// Quality gate plus pitch agreement from the ungated current-note field
		// (bend-window aware: see MatchesPickPitch). The absolute LEVEL gate is
		// deliberately NOT required here: the window only opens on a spike (or a
		// fired onset), which is level evidence in itself - a decaying ring never
		// spikes - and a quiet signal chain sits below the game's -55 dB gate for
		// every pick (2026-08-18 late session: spike + quality 86 + exact expected
		// pitch rejected at -59.4 dB, forcing a second, harder strum).
		// The detector's read is -1-bin biased and lags the freeze, so an early pick
		// of the CORRECT successor routinely reads expected-1 here and the exact-pitch
		// match never builds a streak (measured 2026-09-01 real play: 57 discarded
		// onsets vs 4 shortcut accepts, reattack accepts ZERO - "the game swallowed my
		// pick"). Fall back to the raw route: Tier0ConfirmsExpected resolves the true
		// fundamental at cent resolution with the full sub-harmonic guard set, so a
		// one-fret wrong or a lower note's harmonic cannot confirm. Only inside the
		// spike/onset-opened window, so attack evidence is still always required.
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
		// #67 (2026-08-27): observe the native hit decision for BEND notes on ANY song, so we
		// can answer whether Rocksmith's own decision would judge a bend (toward deferring bends
		// to native the way chords do). Was gated to a single test-song time window
		// (BEND_DIAGNOSTIC_START/END_TIME); re-gated to a real authored bend amount instead.
		// This fires only in the non-controlling path (HitDecisionDetour line ~5571), so it
		// captures the native decision as the bend passes in normal flow, not under the freeze
		// hold - a first, side-effect-free data point; the under-freeze test needs a careful
		// separate change (a speculative native call has side-effect risk).
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
			<< " nbnEnabled=" << ResearchProbeRuntime::IsNoteByNoteEnabled()
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
		ResearchProtocol::ExpectedAttackEventKind kind)
	{
		ResearchProtocol::ExpectedAttackEvent event;
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
		ResearchProbeRuntime::PublishExpectedAttackEvent(event);
	}

	void ClearSelection()
	{
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
		// previousSelectedString is deliberately NOT cleared here.
		//
		// It records which string is still ringing, which is what decides whether a
		// hammer-on or pull-off is physically playable and therefore whether it may be a
		// target of its own. Clearing it when a selection ends erases that memory, so the
		// next fresh selection sees -1, the skip test fails its >= 0 guard, and the
		// continuation becomes a separate target. Philip saw this as a legato expression
		// being split: an expression may be any length, for example green 15 to 13 to 12,
		// and the run is defined only by the chart's own legato flags plus same-string
		// continuity, never by an assumed pair or link field.
		//
		// It is reset in ResetBootstrap instead, where the section really does change and
		// no string can still be ringing from the previous selection.
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

	// #57 native freeze announcement (decomp-game-event-handlers-2026-08-24).
	// The game's own GE_FreezeOnTag does exactly our mechanical freeze
	// (Stop_TMusic via PlayerSong vtable+0x24, owner vtable+0x50 five-clock set)
	// and then ANNOUNCES it: dispatches the tag through 0x407840 and sets the
	// frozen-on-tag byte at owner+0x5E3, which native systems read to enter the
	// freeze presentation. Our controller has only ever done the mechanical
	// half. This writes the flag around owned holds.
	//
	// DEFAULT ON (Philip, 2026-08-25): the first observed live consumer effect -
	// with the flag on, single-note acceptance is markedly better ("a lot
	// better"), and the sessions where singles felt regressed were exactly the
	// ones where a probe reload had silently reset this to the old OFF default.
	// Mechanism unconfirmed (plausibly a native consumer stops competing for
	// onset edges while the game believes itself frozen-on-tag); A/B live with
	// freeze-flag-off if it needs re-testing.
	constexpr uintptr_t OWNER_FROZEN_ON_TAG = 0x5E3;
	// Default OFF 2026-08-27: the freeze-family fields (+0x5C8..+0x5E8) belong to
	// the frozen-object class, and the working hypothesis backed by the checkpoint
	// lattice is that GamePlaysongLAS's allocation is SMALLER - making every
	// freeze-flag write a heap-smashing out-of-bounds write (the whole two-day
	// crash family). The one-shot owner-size diagnostic on the first hold settles
	// it; freeze-flag-on re-enables for study only after the size clears it.
	volatile bool isNativeFreezeFlagEnabled = false;

	// Native-drive step 1 diagnostics (2026-08-25 rewrite plan). Pure reads, no
	// calls, no writes: at every hold establishment and release the controller
	// reports the ground truth the native freeze/resume swap depends on:
	//   - the GE handlers' mode-object resolve chain ([[0x0135F54C]+0x10]+0x50)
	//     and whether it matches our tracked owner (it resolves GamePlaysong in
	//     lessons; expected DEAD or foreign in Learn A Song per the chord-panel
	//     crash post-mortem - this line is the proof either way);
	//   - the scheduler global 0x0135F5AC the frozen-span destructor shifts
	//     (0x57EB00 op 3 = entry.time += frozenDuration, the anti-sweep);
	//   - the owner's freeze-family fields the frozen-span object maintains
	//     (+0x5C8 tag string begin/end at +0x5D8/+0x5DC, +0x5E4 resume flag,
	//     +0x618 freeze-start-time double) so we can see what state the
	//     mechanical freeze leaves them in versus what GE_FreezeOnTag would.
	constexpr uintptr_t NATIVE_SCHEDULER_GLOBAL = 0x0135F5AC;
	constexpr uintptr_t OWNER_FREEZE_TAG_STRING = 0x5C8;
	constexpr uintptr_t OWNER_FREEZE_TAG_BEGIN = 0x5D8;
	constexpr uintptr_t OWNER_FREEZE_TAG_END = 0x5DC;
	constexpr uintptr_t OWNER_RESUME_FROM_TAG = 0x5E4;
	constexpr uintptr_t OWNER_FREEZE_START_TIME = 0x618;

	// The lesson engine's own chord panel (decomp-freeze-announce-2026-08-24):
	// GE_ShowChordDisplay's implementation. __cdecl(int index); resolves the
	// GamePlaysong owner itself, bounds-checks the index against the 0x48-stride
	// chord template array on the chord-display component at owner+0x78, copies
	// the template and sets the visible flag. Index space live-verified equal to
	// the SNG chordId (idx 9 = G5 [x/x/5/7/8/x], idx 10 = C5 [x/x/5/5/8/x] on the
	// running game). This is the presentation half a real lesson pairs with the
	// frozen-on-tag flag; without it the freeze mode hides the target's normal
	// panel and shows nothing (Philip's "done the opposite" observation).
	// Calling 0x405C00 itself CRASHED the game (2026-08-24 night, crash dump
	// Rocksmith2014.exe(1).35960.dmp: AV inside FUN_006106C0+7, return address
	// in 0x405C00's body): it resolves a "ge_game" registry entry that exists
	// only in lesson mode, not in Learn A Song. The first replacement replicated
	// the call's terminal effect with guarded writes against the SCORING owner's
	// +0x78 - and that corrupted the heap. Crash dump Rocksmith2014.exe(1).9096
	// .dmp (2026-08-24 21:14, AV at 0x541186 in a pool free-list teardown) shows
	// the victim object's allocator fields at +0xB4/+0xB8 holding 67 and -1:
	// adjacent int32s of a G5 template's notes[] array. The object at the LAS
	// owner's +0x78 is NOT the chord-display component; it is a smaller
	// allocation whose +0x94/+0x98 happened to hold an ascending pointer pair,
	// so the bounds check passed and the 0x48-byte template write ran off the
	// end of the allocation into its heap neighbour's free-list links, which the
	// game walked at teardown. Two lessons, both recorded in the atlas:
	//   1. owner+0x78 is only the chord display on the object 0x405C00 itself
	//      resolves: wrapper = [[[0x0135F54C]]+0x10]+0x50], type-checked as
	//      GamePlaysong. ShowNativeChordPanel now resolves through that same
	//      chain and ignores the scoring owner entirely; when the chain is dead
	//      (Learn A Song) the panel is not driven and nothing is written.
	//   2. The toggle now defaults OFF: the panel only ever existed in lesson
	//      mode, and Learn A Song gets its chord identity from the overlay.
	// The +0x1DC sub-object branch is still deliberately skipped.
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

	// Native-drive step 2: the schedule-shift experiment (2026-08-25 rewrite
	// plan, ladder step 3). The frozen-span mode object's destructor compensates
	// the engine for the frozen time: it calls the scheduler service at 0x57EB00
	// with op 3 (entry.time += delta) so nothing downstream reads as "missed"
	// when the transport resumes - the anti-sweep mechanism. The exact call was
	// pinned from the destructor's disassembly (0x471B7B..80):
	//   EAX = 4 (scheduler entry id), EDI = 0 (sub-list index),
	//   stack: push elapsed (double), push 3, push scheduler; RET 0x10
	//   (callee-cleaned). Guarded: scheduler global must be non-zero and the
	//   elapsed span sane. Elapsed uses wall-clock for this experiment (the
	//   native path reads a global clock member; a steady_clock span is the
	//   same unit at our precision - upgrade to the native read if drift shows).
	// DEFAULT OFF; A/B live with schedule-shift-on|off: hold a chord section
	// with it on and watch whether the fretboard sweep/retire behavior changes
	// at release. This is the first native CALL the controller makes - one
	// call per release, only while the toggle is on.
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

	// The game's own spectral harmonic chord matcher (0x4E6E90) - the core behind Lua's
	// IsChordBeingPlayed. ABI PROVEN from raw disasm (2026-08-29): __stdcall(int* det,
	// int* midiArray, int count, int frameOffset, char flag) -> bool in AL, ret 0x14
	// (callee cleans the 5 args, so we must NOT touch ESP after the call); EBX/ESI/EDI/EBP
	// preserved. It matches the chord's pitches against the raw analysis spectrum + per-
	// note harmonic templates, so unlike the sounding table it is immune to the #53
	// masking, and it has NO detector-clock/window gate, so the frozen transport does not
	// desync it. det/state and the two amplitude gates are resolved exactly as the game's
	// caller FUN_00406460 does: det=*(*(*(0x135F57C)+0x10)+0x08), state=*(det+4). Returns
	// 1 = chord sounding, 0 = not sounding / input too quiet, -1 = could not run.
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

	// The native SINGLE-NOTE spectral matcher (0x4E7B30) - the matcher the game's own
	// single-note onset scan (0x4E6A70) calls per onset frame. ABI PROVEN from raw disasm
	// (2026-08-30, from both call sites inside 0x4E6A70): bool __stdcall(int* det,
	// int* expectedMidiBuf, int flag, int frameOffset), ret 0x10 (CALLEE cleans the 4 args -
	// do NOT fix esp after the call), returns AL, preserves EBX/ESI/EDI/EBP. det=root->arr->det
	// (state=det+4) resolved exactly as the chord matcher does. expectedMidiBuf is a 6-int
	// array; entries < 0 are ignored, so a lone note passes {expectedMidi,-1,-1,-1,-1,-1}.
	// This runs the game's own harmonic analysis of the frame, so - unlike a raw ring-frame
	// pitch read - a one-frame attack transient cannot graze a neighbour and a correctly
	// played note is not lost to a noisy attack frame (the two symptoms of the frame-pitch
	// port). It is octave/open-string aware but has NO +/- semitone tolerance.
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

	// Faithful port of the native onset-window hit scan (FUN_004E6A70 scan-mode + the onset
	// gate FUN_004E4B60, 2026-08-29). Returns true iff, in the ring history NEWER than
	// sinceRingTime (our hold-latch stamp - the window origin the native code reads from the
	// frozen detector clock), (a) some frame carries a fresh-attack ONSET flag (frame+0x7B5),
	// AND (b) the native harmonic matcher accepts the chord at SOME frame in that span
	// (any-frame-hit, first match wins - exactly the native combine rule). A decaying ring
	// has no fresh onset frame, so it cannot pass; each dense successor latches a new
	// sinceRingTime, so a prior strum's onset cannot carry across the chain. The algorithm
	// (backward frame walk, timestamp bound, onset-then-match) is copied verbatim; ONLY the
	// window origin is swapped from the (frozen) clock to sinceRingTime.
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

		// Onset gate (FUN_004E4B60): a fresh-attack flag must appear in the span.
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

		// Scan (FUN_004E6A70 scan-mode): the matcher must accept at SOME frame in the span.
		for (int32_t frameOffset = 0; frameOffset < scanLimit; ++frameOffset)
		{
			double frameTs = 0.0;
			if (!frameTimeAt(frameOffset, frameTs) || frameTs <= sinceRingTime) return false;
			if (CallNativeChordMatcher(tones, count, frameOffset) == 1) return true;
		}
		return false;
	}

	// OFFLINE-CAPTURE diagnostic (2026-08-30): logs the raw spectral peak list at each fresh-onset
	// frame during a single-note hold, labelled with the expected pitch, so the accept RULE can be
	// designed and validated offline against real correct-vs-wrong plays instead of guessed live.
	// One line per pluck: expected, frame level, and the loudest peaks {midi:energy}. Passive - it
	// does not affect acceptance.
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
		// Path-B convergence proof (2026-08-31): log the native clock-independent loudest-note
		// query (NDGetLoudestPlayedNote 0x48E5F0) next to the gate's dominant peak (peaks[0]), so
		// we can see LIVE whether the native query and the dominant spectral peak agree for
		// correct vs wrong frets. NDGetLoudestPlayedNote has NO onset dedupe side effect, unlike
		// NDGetOnsetNote, so reading it here is passive.
		const int ndLoudest = QueryNativeLoudestPlayedNote();
		LOG_INFO("(NBN CAPTURE) exp=" << expected << " ndLoudest=" << ndLoudest
			<< " domPeak=" << (found > 0 ? peaks[0].midi : -1) << " lvl=" << std::fixed << std::setprecision(1)
			<< level << " peaks=[" << ss.str() << "]" << std::endl);
	}

	// Tier-0 SHADOW logging (issue #29, 2026-08-31): alongside every single-note accept
	// and (throttled) reject, log the raw-route Goertzel evidence for the expected note -
	// cent-resolution powers measured upstream of the engine's semitone bins, via the
	// host's RawPitchVerifier over the same GetBuffer tap. Shadow only: it never gates
	// the decision yet. Once live data shows the raw measure separating correct from
	// +/-1-fret wrongs (which the semitone features provably cannot), it becomes the
	// tier-0 enforcement layer. expectedMidi is already the detector-frame pitch, i.e.
	// the frame of the audio the observer hears, so the equal-tempered frequency is the
	// right target directly.
	void LogTier0ShadowEvidence(int expectedMidi, const char* context)
	{
		if (expectedMidi < 0) return;
		ResearchProtocol::RawToneEvidence evidence;
		const double frequency =
			440.0 * std::pow(2.0, (static_cast<double>(expectedMidi) - 69.0) / 12.0);
		if (!ResearchProbeRuntime::QueryRawToneEvidence(frequency, 0.15f, evidence)) return;
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

	// Tier-1 SHADOW logging (issue #29, 2026-09-01): next to the tier-0 shadow lines, log
	// what the 64-bit companion pitch service currently hears, so the discrimination data
	// for the eventual ML gate accumulates from the same accept/reject moments. Shadow
	// only, exactly like tier-0 started: it never touches the decision. ml is a float
	// midi in the OBSERVED route frame (the companion hears post-shifter audio; the
	// applied shift travels in the shared header, so the comparison against exp is done
	// offline, not fudged here). Throttled because the freeze re-evaluates every tick and
	// the reject path already fires per tick while a wrong note rings.
	void LogTier1ShadowPitch(int expectedMidi)
	{
		if (expectedMidi < 0) return;
		if (!ResearchProbeRuntime::IsMlPitchServiceAlive()) return;
		// No internal throttle: every call site paces itself (accept is a rare event,
		// reject and the hold-phase sampler are throttled where they fire), and a shared
		// throttle here let the 300ms HOLD sampler starve the accept-moment lines - the
		// exact pairs the offline comparison needs most.
		float mlMidi = 0.0f;
		float mlConfidence = 0.0f;
		double mlAgeSeconds = 0.0;
		if (!ResearchProbeRuntime::QueryMlPitch(mlMidi, mlConfidence, mlAgeSeconds)) return;
		LOG_INFO("(NBN TIER1) shadow exp=" << expectedMidi
			<< std::fixed << std::setprecision(2)
			<< " ml=" << mlMidi
			<< " conf=" << mlConfidence
			<< std::setprecision(3) << " age=" << mlAgeSeconds
			<< std::endl);
	}

	// Per-session native-vs-ML comparison CSV (Philip 2026-09-05). One row per sample, so the
	// A/B can be scored offline while the corner HUD shows it live. Lives beside nbn-trace.log
	// in <game>\RSModsResearch\; a fresh probe load starts a new session block via the header.
	void AppendDetectionCompareCsv(int technique, int expectedMidi, int nativeMidi,
		bool nativeMatch, bool mlHadOpinion, int mlMidi, bool mlMatch, int code)
	{
		static std::ofstream csv = []()
		{
			char modulePath[MAX_PATH] = {};
			HMODULE module = nullptr;
			std::string path = "RSModsResearch\\nbn-detection-compare.csv";
			if (GetModuleHandleExA(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
						| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(&AppendDetectionCompareCsv), &module)
				&& GetModuleFileNameA(module, modulePath, MAX_PATH) != 0)
			{
				std::string full(modulePath);
				const auto lastSlash = full.find_last_of('\\');
				if (lastSlash != std::string::npos)
				{
					const auto parentSlash = full.find_last_of('\\', lastSlash - 1);
					if (parentSlash != std::string::npos)
						path = full.substr(0, parentSlash + 1) + "nbn-detection-compare.csv";
				}
			}
			std::ofstream out(path, std::ios::app);
			if (out.is_open())
				out << "# session " << GetTickCount64()
					<< "\ntickMs,technique,expectedMidi,nativeMidi,nativeMatch,mlHadOpinion,mlMidi,mlMatch,result\n";
			return out;
		}();
		if (!csv.is_open()) return;
		const char* techniqueName = technique == 1 ? "chord" : (technique == 2 ? "bend" : "single");
		const char* result = code == 1 ? "agree" : (code == 0 ? "disagree" : "one-sided");
		csv << GetTickCount64() << ',' << techniqueName << ',' << expectedMidi << ','
			<< nativeMidi << ',' << (nativeMatch ? 1 : 0) << ',' << (mlHadOpinion ? 1 : 0) << ','
			<< mlMidi << ',' << (mlMatch ? 1 : 0) << ',' << result << '\n';
		csv.flush();
	}

	// Sample both detectors passively while a hold is live and record whether they agree that
	// the expected note (or chord tones / bent top) is being played. Native's opinion comes
	// from the game's own loudest-played-note query (no onset dedupe side effect); ML's from
	// the companion pitch service. Neither read touches the acceptance decision - this is the
	// shadow the authority selector promised. Runs in all builds; Philip plays in Release.
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
		if (ResearchProbeRuntime::IsMlPitchServiceAlive())
		{
			float mlMidi = 0.0f, mlConfidence = 0.0f;
			double mlAgeSeconds = 0.0;
			if (ResearchProbeRuntime::QueryMlPitch(mlMidi, mlConfidence, mlAgeSeconds)
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
		AppendDetectionCompareCsv(static_cast<int>(technique), expectedMidi, nativeMidi,
			nativeMatch, mlHadOpinion, mlMidiInt, mlMatch, code);
	}

	// Tier-0 ENFORCEMENT (2026-08-31, after the discrimination sweep: 13/14 notes separate
	// +/-1 semitone at >=10x raw power ratio, the worst - the open low E - at 7.4x).
	// A conservative VETO layered on top of the semitone-bin gate: an accept is refused
	// only when the raw route shows a NEIGHBOUR pitch strongly and positively dominating
	// the expected fundamental - the +/-1-fret wrong the bins provably cannot see (their
	// reads collapse to exp/exp-1 either way; measured all session). No evidence, silence,
	// or ambiguity never vetoes: correct notes carry a 7-500x margin, so this cannot
	// starve them, and the freeze re-evaluates every tick anyway. Default ON per the test
	// policy; the A/B toggle below covers reverts.
	volatile bool g_tier0Enforcement = true;

	// Cent-tolerant target band (2026-09-01, from the full-session veto statistics): the
	// 0.15s rectangular window gives a ~6.7 Hz main lobe, which above ~400 Hz is NARROWER
	// than ordinary pitch reality - fret-12..15 intonation and vibrato run +/-15-25 cents
	// (~+/-7-11 Hz at 660-800 Hz) - so a correctly played high note can read a near-EMPTY
	// target bin (measured: veto rate 4.5/note above 400 Hz vs 0 below 110 Hz, and in a
	// third of veto episodes the game's own detector read the note correct while the
	// single center bin saw nothing). Fix the measurement, not the thresholds: the target
	// is the MAX of three probes at f and f*2^(+/-25 cents); the +/-1/+/-2 neighbours are
	// taken from the center query only. A wrong note a full semitone away gains nothing
	// from a +/-25-cent widening, so the veto/rescue discrimination thresholds - validated
	// live at 3x/5x - are unchanged.
	constexpr double TIER0_TARGET_DETUNE_RATIO = 1.0145453349375237; // 2^(25/1200)

	// Frequency-adaptive analysis window (2026-09-01 evening). The rectangular window's
	// main lobe is ~1/windowSeconds Hz; at 0.15s that is ~6.7 Hz, wider than a semitone
	// below ~130 Hz (the low-E floor: the 7.4x worst case in the original sweep) and
	// wide enough at 100-200 Hz that a loud +/-1 neighbour leaks enough into the target
	// probes to defeat the "empty target" veto signature. Longer windows below G3 narrow
	// the lobe to ~3.3 Hz, separating semitones down to the open low E. The freeze waits
	// for the player, so the extra look-back costs nothing that matters here.
	float Tier0WindowSecondsForFrequency(double frequencyHz)
	{
		if (frequencyHz < 200.0) return 0.30f;    // midi < ~55: lobe ~3.3 Hz
		if (frequencyHz < 330.0) return 0.20f;    // midi < ~64: lobe ~5 Hz
		return 0.15f;
	}

	bool QueryTier0EvidenceWideTarget(int expectedMidi, ResearchProtocol::RawToneEvidence& evidence)
	{
		const double frequency =
			440.0 * std::pow(2.0, (static_cast<double>(expectedMidi) - 69.0) / 12.0);
		const float windowSeconds = Tier0WindowSecondsForFrequency(frequency);
		if (!ResearchProbeRuntime::QueryRawToneEvidence(frequency, windowSeconds, evidence))
		{
			return false;
		}
		ResearchProtocol::RawToneEvidence detuned;
		if (ResearchProbeRuntime::QueryRawToneEvidence(
				frequency * TIER0_TARGET_DETUNE_RATIO, windowSeconds, detuned)
			&& detuned.targetPower > evidence.targetPower)
		{
			evidence.targetPower = detuned.targetPower;
		}
		if (ResearchProbeRuntime::QueryRawToneEvidence(
				frequency / TIER0_TARGET_DETUNE_RATIO, windowSeconds, detuned)
			&& detuned.targetPower > evidence.targetPower)
		{
			evidence.targetPower = detuned.targetPower;
		}
		return true;
	}

	// Wrong-note latch state, shared by the veto AND the rescue (2026-09-01 night): a
	// recent veto for this midi means wrong-note evidence was positively seen, and every
	// acceptance path must then demand stronger positive evidence than usual. Single
	// slot - NBN serializes notes.
	int g_tier0LatchMidi = -1;
	ULONGLONG g_tier0LatchTick = 0;

	// One +/-25-cent-widened power probe at an arbitrary frequency - the shared shape of
	// every sub-harmonic guard (hoisted from the rescue 2026-09-01 late so the veto can
	// run the same guards): a lower note played slightly off-center shifts its harmonics
	// by the same cents, and an under-read sub probe weakens exactly the guard that keeps
	// a lower note's harmonic from masquerading as the expected note.
	bool QueryTier0WideProbePower(double centerHz, float& outPower)
	{
		const float windowSeconds = Tier0WindowSecondsForFrequency(centerHz);
		ResearchProtocol::RawToneEvidence probe;
		if (!ResearchProbeRuntime::QueryRawToneEvidence(centerHz, windowSeconds, probe)) return false;
		outPower = probe.targetPower;
		if (ResearchProbeRuntime::QueryRawToneEvidence(
				centerHz * TIER0_TARGET_DETUNE_RATIO, windowSeconds, probe)
			&& probe.targetPower > outPower)
		{
			outPower = probe.targetPower;
		}
		if (ResearchProbeRuntime::QueryRawToneEvidence(
				centerHz / TIER0_TARGET_DETUNE_RATIO, windowSeconds, probe)
			&& probe.targetPower > outPower)
		{
			outPower = probe.targetPower;
		}
		return true;
	}

	// Fill the low/slight part of a bend the native tracker misses (2026-09-05). Trace evidence:
	// the motion-note tracker only registers once the bend is already ~0.6 semitone up and drops
	// out intermittently, and the integer detector reports out-of-band garbage there - so a slight
	// bend does not move the needle and it "teleports" a quarter of the way up when the tracker
	// finally catches. The raw tap measures the sounding pitch anywhere. A bounded coarse scan
	// (0.5-semitone steps across the bend span, <=10 probes) plus parabolic peak refinement gives a
	// fractional pitch. One QueryRawToneEvidence per point, called only on a tracker miss during a
	// bend - a handful of probes per frame, the same budget the tier-0 rescue already runs at 60
	// FPS (the earlier 1-FPS regression was a 27-point-per-frame grid on TWO code paths). Returns
	// false on silence or no clear tone, so the needle holds rather than chasing noise.
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
		ResearchProtocol::RawToneEvidence e;
		if (!ResearchProbeRuntime::QueryRawToneEvidence(freq, windowSeconds, e)) return false;
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

	bool Tier0VetoesAccept(int expectedMidi)
	{
		if (!g_tier0Enforcement || expectedMidi < 0) return false;
		// DECISION MEASUREMENT: the exact original, validated geometry - 0.15s window,
		// center-frequency probes only. The 2026-09-01 wide/long variants of this
		// measurement are NOT allowed to decide the veto: live same-night proof (Speaker
		// Eb, deliberate one-fret-sharp wrongs) that they dilute the wrong-note
		// signature into the escape zone - the 0.15s center measurement read the same
		// strums at 17x inversion over an empty target while the widened one declined
		// to veto, and the bin family band then accepted the wrong note.
		ResearchProtocol::RawToneEvidence evidence;
		const double frequency =
			440.0 * std::pow(2.0, (static_cast<double>(expectedMidi) - 69.0) / 12.0);
		// HARMONIC DECISION BAND (2026-09-01 late, the low-E blindness). Below ~160 Hz
		// the 0.15s rectangular lobe (~6.7 Hz) is as wide as or wider than a semitone
		// (4.9 Hz at open low E), so the wrong note's own energy lands INSIDE the
		// expected fundamental bin: every low-E wrong this session read tgt ~= -1 ~= +1
		// mush and sailed through the "genuinely sounding" escape - the veto never
		// fired below 123 Hz. Physics offers the exit: at the 2nd and 3rd harmonics the
		// semitone spacing doubles and triples (9.8/14.7 Hz at low E - fully resolved),
		// and the low strings put most of their energy there anyway (the same fact that
		// broke the rescue's octave guard). So below the band edge the SAME dual
		// signature is decided on summed 2f+3f evidence; each query's +/-1/+/-2 bins
		// land exactly on the neighbour NOTE's own 2nd/3rd harmonics, so the sums pair
		// per semitone offset. Midi 52 (164.8 Hz) stays on the fundamental path - its
		// vetoes measured clean this session.
		const bool harmonicDecision = frequency < 160.0;
		float harmonicMinTarget = 0.0f;
		if (harmonicDecision)
		{
			ResearchProtocol::RawToneEvidence second;
			ResearchProtocol::RawToneEvidence third;
			if (!ResearchProbeRuntime::QueryRawToneEvidence(frequency * 2.0, 0.15f, second)
				|| !ResearchProbeRuntime::QueryRawToneEvidence(frequency * 3.0, 0.15f, third))
			{
				return false;                          // no evidence -> fail open
			}
			evidence.targetPower = second.targetPower + third.targetPower;
			evidence.minusOnePower = second.minusOnePower + third.minusOnePower;
			evidence.plusOnePower = second.plusOnePower + third.plusOnePower;
			evidence.minusTwoPower = second.minusTwoPower + third.minusTwoPower;
			evidence.plusTwoPower = second.plusTwoPower + third.plusTwoPower;
			evidence.totalRms = second.totalRms;
			harmonicMinTarget = (std::min)(second.targetPower, third.targetPower);
		}
		else if (!ResearchProbeRuntime::QueryRawToneEvidence(frequency, 0.15f, evidence))
		{
			return false;                              // no evidence -> fail open
		}
		float worstNeighbour = evidence.minusOnePower;
		if (evidence.plusOnePower > worstNeighbour) worstNeighbour = evidence.plusOnePower;
		if (evidence.minusTwoPower > worstNeighbour) worstNeighbour = evidence.minusTwoPower;
		if (evidence.plusTwoPower > worstNeighbour) worstNeighbour = evidence.plusTwoPower;
		// Wrong-note LATCH (2026-09-01 night, from the live decay-tail slip). The veto
		// fires cleanly while a wrong note is loud (measured this session: 25-500x
		// inversions across seven consecutive evaluations), but as the wrong note
		// DECAYS the raw evidence collapses to near-parity (tgt 4e-4, worst 7e-4 at
		// the slip) - below every veto threshold by design - while the engine's biased
		// bins still testify a clean expected-family run, and the family band accepts
		// the dying wrong note. So a veto LATCHES: after any veto for this expected
		// midi, ambiguity is no longer enough to accept - only POSITIVE presence of
		// the expected fundamental (target genuinely sounding AND not out-gunned, or
		// the wide-band override below) unlatches. A decaying wrong note can never
		// produce positive presence; a correct note's sustain produces it within a few
		// evaluations of the pick, which is exactly when it should accept.
		int& latchMidi = g_tier0LatchMidi;
		ULONGLONG& latchTick = g_tier0LatchTick;
		const ULONGLONG nowTick = GetTickCount64();
		const bool latched = latchMidi == expectedMidi && nowTick - latchTick <= 600;
		// Positive presence in the harmonic band demands BOTH harmonic bins lit, not
		// their sum (2026-09-01, first synthetic battery finding): E2's 3rd harmonic IS
		// B3 - a mash containing the open low E lit expected-47's 2f bin dead-center
		// (247.2 vs 246.9 Hz) and the sum passed as "genuinely sounding". A real note
		// cannot avoid lighting 2f AND 3f; a twelfth-collision phantom lights exactly
		// one.
		const bool positivelyPresent = evidence.targetPower >= 5e-4f
			&& worstNeighbour <= evidence.targetPower * 3.0f
			&& (!harmonicDecision || harmonicMinTarget >= 1e-4f);
		auto refreshLatch = [&]() { latchMidi = expectedMidi; latchTick = nowTick; };

		// SUB-HARMONIC VETO (2026-09-01, the -12 sweep hole: 7 of 19 octave-below wrongs
		// accepted, every other offset 0/135). A note an octave below CONTAINS the
		// expected note - its 2nd harmonic lands exactly on the target bin and its 4th on
		// the corroboration bin, so both the engine's folded bin run and every
		// target-presence rule here read "correct" (the energy really is there). The
		// rescue has carried these guards since last night; the run path never consults
		// them - this is that same trio on the veto, so every accept path is covered:
		//   - f/2 or f/3 at or above the target = a lower note's fundamental is the
		//     loudest thing in the series - the "target" is its harmonic. Veto.
		//   - betrayal probe: the sub-octave's 3rd harmonic lands at 1.5f, which NOTHING
		//     in the expected note's own series occupies. Unlike the rescue, the veto
		//     gates EVERY accept, and a decaying previous note a fifth above (the song's
		//     own 59->52 step) also lights 1.5f - so here betrayal additionally requires
		//     trace f/2 energy (the real phantom measured subOct at 0.033x target; a
		//     ringing tail leaves nothing at f/2).
		// Fundamental path only: in the harmonic band targetPower is a 2f+3f sum in a
		// different frame, and that band already demands two-bin corroboration.
		// Entry bar is the 1e-6 audible floor, NOT 1e-5: the first verification sweep
		// (2026-09-01) still accepted 8 sub-octave wrongs because a ringing injected
		// note reads only ~2e-4 at its own fundamental and 1e-6..1e-5 at its 2nd
		// harmonic (the target bin) - a 1e-5 gate skipped the check for every one of
		// them, and the "note alone is ringing" rule below then failed open. Each rule
		// keeps an absolute 2e-5 floor on the probe it trusts, so noise-floor ratios
		// still cannot veto.
		if (!harmonicDecision && evidence.targetPower >= 1e-6f)
		{
			float subOctPower = 0.0f;
			float subTwelfthPower = 0.0f;
			float fifthAbovePower = 0.0f;
			if (QueryTier0WideProbePower(frequency * 0.5, subOctPower)
				&& QueryTier0WideProbePower(frequency / 3.0, subTwelfthPower)
				&& QueryTier0WideProbePower(frequency * 1.5, fifthAbovePower))
			{
				// Thresholds measured at the exp=59 slip (2026-09-01): a ringing
				// sub-octave reads subOct=0.83x and fifthAbove=0.21x its own 2nd
				// harmonic; a correct note reads 0.000000 in both bins. Same numbers as
				// the rescue guards so the two paths agree on what a sub-octave is.
				const bool lowerFundamental =
					(subOctPower >= 2e-5f && subOctPower >= evidence.targetPower * 0.4f)
					|| (subTwelfthPower >= 2e-5f && subTwelfthPower >= evidence.targetPower * 0.4f);
				const bool subOctaveBetrayal =
					fifthAbovePower >= 1e-5f
					&& fifthAbovePower >= evidence.targetPower * 0.1f
					&& subOctPower >= evidence.targetPower * 0.02f;
#if defined(_DEBUG)
				// Diagnostic while the -12 hole is being calibrated: the two verification
				// sweeps failed IDENTICALLY with zero sub-harmonic vetoes, so the block
				// runs but never trips - print what the probes actually read so the next
				// threshold change is measured, not guessed.
				static ULONGLONG lastSubProbeLogTick = 0;
				if (nowTick - lastSubProbeLogTick >= 250)
				{
					lastSubProbeLogTick = nowTick;
					LOG_INFO("(NBN TIER0) SUBPROBE exp=" << expectedMidi
						<< std::fixed << std::setprecision(6)
						<< " tgt=" << evidence.targetPower
						<< " subOct=" << subOctPower
						<< " subTwelfth=" << subTwelfthPower
						<< " fifthAbove=" << fifthAbovePower
						<< " worst=" << worstNeighbour << std::endl);
				}
#endif
				if (lowerFundamental || subOctaveBetrayal)
				{
					refreshLatch();                    // positive wrong-note evidence
					static ULONGLONG lastSubHarmonicLogTick = 0;
					if (nowTick - lastSubHarmonicLogTick >= 250)
					{
						lastSubHarmonicLogTick = nowTick;
						LOG_INFO("(NBN TIER0) VETO exp=" << expectedMidi
							<< std::fixed << std::setprecision(6)
							<< " tgt=" << evidence.targetPower
							<< " subOct=" << subOctPower
							<< " subTwelfth=" << subTwelfthPower
							<< " fifthAbove=" << fifthAbovePower
							<< " sig=" << (lowerFundamental ? "sub-harmonic" : "sub-octave-betrayal")
							<< " - a lower note is sounding, accept refused." << std::endl);
					}
					return true;
				}
			}
		}

		if (worstNeighbour < 1e-6f && evidence.targetPower < 1e-6f)
		{
			// TOTAL SILENCE in the decision bins (2026-09-01, second synthetic battery
			// finding): the engine's accept run trusts ring frames up to 0.2s old, so a
			// wrong sound that ENDS just before the run completes is judged purely on
			// stored, biased testimony - the measured mash slip accepted at
			// tgt=0/-1=0/+1=0/rms=0.01, live audio gone. An accept with zero live
			// corroboration is refused outright; no latch (nothing wrong was observed
			// either). A genuinely ringing correct note is never silent here.
			static ULONGLONG lastSilentLogTick = 0;
			if (nowTick - lastSilentLogTick >= 250)
			{
				lastSilentLogTick = nowTick;
				LOG_INFO("(NBN TIER0) VETO exp=" << expectedMidi
					<< " sig=silent-window - the live route cannot corroborate the"
					<< " detector's stored frames, accept refused." << std::endl);
			}
			return true;
		}
		if (worstNeighbour < 1e-6f && !latched)
		{
			// Quiet neighbours + audible target = the note alone is ringing: fail open -
			// EXCEPT in the harmonic band with only ONE bin lit (2026-09-01, third
			// synthetic battery finding): the decayed mash left twelfth-collision
			// residue in the 2f bin alone, which read as "the note alone is audible"
			// and slipped the accept through while the live route held nothing of the
			// expected note's actual series. One-bin residue is not corroboration.
			if (!harmonicDecision || harmonicMinTarget >= 1e-4f) return false;
			static ULONGLONG lastUncorrobLogTick = 0;
			if (nowTick - lastUncorrobLogTick >= 250)
			{
				lastUncorrobLogTick = nowTick;
				LOG_INFO("(NBN TIER0) VETO exp=" << expectedMidi
					<< std::fixed << std::setprecision(6)
					<< " tgt=" << evidence.targetPower
					<< " minH=" << harmonicMinTarget
					<< " sig=uncorroborated-single-bin - accept refused." << std::endl);
			}
			return true;
		}
		// A genuine wrong note shows BOTH signatures at once (measured live): the
		// neighbour strongly inverted (50-230x) AND the target bin essentially EMPTY
		// (7e-5..3e-4). A correct note fighting pollution - the Speaker-Mode shifter's
		// phase-vocoder residue, sympathetic ring - shows a mild inversion (1.7-5x) but
		// with SUBSTANTIAL target energy (>=1.5e-3 measured on the stuck famous-note
		// class), and must never veto. Requiring both conditions separates the classes
		// cleanly: near-parity alone (a 1.2x bar) vetoed correct notes whose fundamental
		// was clearly present.
		// The inversion bar is 3x - except when the target is TINY (< 1e-4, far below
		// the 1.5e-3+ every polluted-correct note measured), where 2x suffices: the
		// gen-6 sweep's one slip was a +1 wrong the engine's -1 bias folded onto the
		// expected bin (dom=[59,59,59] from a ringing 60) with the raw route at
		// worst=2.9x a 1.3e-5 target - a whisker under 3x. Clean corrects in the same
		// sweep read worst at 0.12-0.2x target: 10x of headroom below the 2x bar.
		const float inversionBar = evidence.targetPower < 1e-4f ? 2.0f : 3.0f;
		bool vetoSignature = evidence.targetPower < 5e-4f
			&& worstNeighbour > evidence.targetPower * inversionBar
			&& worstNeighbour >= 1e-6f;
		if (harmonicDecision && !positivelyPresent)
		{
			// Low-band rule of record (2026-09-01, after three successive synthetic
			// battery slips walked through three successive threshold cracks - loud
			// collision, silent window, sub-threshold residue): below 160 Hz the
			// engine's detector is at its most gullible (it fabricated a clean
			// [47,47,47] run from an E2+C3 mash), so an accept REQUIRES positive
			// two-bin corroboration - both the 2f and 3f bins carrying the expected
			// note. Anything less refuses this tick. A correct note cannot avoid
			// producing that evidence; no phantom, mash, or residue can produce it.
			if (!vetoSignature)
			{
				static ULONGLONG lastNoCorrobLogTick = 0;
				if (nowTick - lastNoCorrobLogTick >= 250)
				{
					lastNoCorrobLogTick = nowTick;
					LOG_INFO("(NBN TIER0) VETO exp=" << expectedMidi
						<< std::fixed << std::setprecision(6)
						<< " tgt=" << evidence.targetPower
						<< " minH=" << harmonicMinTarget
						<< " worst=" << worstNeighbour
						<< " sig=no-corroboration band=2f+3f - accept refused."
						<< std::endl);
				}
				return true;
			}
		}
		if (!vetoSignature)
		{
			if (!latched) return false;
			if (positivelyPresent) { latchMidi = -1; return false; }   // real note arrived
			// Latched and still ambiguous: give the wide-band override its look below
			// (an off-center fundamental is also positive presence), else keep refusing.
		}
		// The original signature says WRONG. One override, and only in the safe
		// direction: the wide (+/-25 cent) adaptive-window measurement may cancel the
		// veto ONLY by positively proving the expected note is strongly sounding -
		// target at or above the measured polluted-correct level AND its own
		// neighbours quiet. This is the high-fret class the center bin misses
		// (intonation/vibrato pushing the fundamental off a 6.7 Hz bin); a wrong note
		// cannot manufacture that evidence, because its energy IS the loud neighbour.
		ResearchProtocol::RawToneEvidence wide;
		if (!harmonicDecision && QueryTier0EvidenceWideTarget(expectedMidi, wide))
		{
			float wideWorst = wide.minusOnePower;
			if (wide.plusOnePower > wideWorst) wideWorst = wide.plusOnePower;
			if (wide.minusTwoPower > wideWorst) wideWorst = wide.minusTwoPower;
			if (wide.plusTwoPower > wideWorst) wideWorst = wide.plusTwoPower;
			if (wide.targetPower >= 1.5e-3f
				&& wideWorst < wide.targetPower * 5.0f)
			{
				latchMidi = -1;                        // positive presence unlatches too
				static ULONGLONG lastOverrideLogTick = 0;
				if (nowTick - lastOverrideLogTick >= 250)
				{
					lastOverrideLogTick = nowTick;
					LOG_INFO("(NBN TIER0) VETO-OVERRIDE exp=" << expectedMidi
						<< std::fixed << std::setprecision(6)
						<< " centerTgt=" << evidence.targetPower
						<< " centerWorst=" << worstNeighbour
						<< " wideTgt=" << wide.targetPower
						<< " wideWorst=" << wideWorst
						<< " - off-center fundamental positively present, veto cancelled."
						<< std::endl);
				}
				return false;
			}
		}
		refreshLatch();
		static ULONGLONG lastVetoLogTick = 0;
		const bool shouldLogVeto = vetoSignature || nowTick - lastVetoLogTick >= 250;
		if (shouldLogVeto)
		{
			lastVetoLogTick = nowTick;
			LOG_INFO("(NBN TIER0) VETO exp=" << expectedMidi
				<< std::fixed << std::setprecision(6)
				<< " tgt=" << evidence.targetPower
				<< " worstNeighbour=" << worstNeighbour
				<< " -1=" << evidence.minusOnePower << " +1=" << evidence.plusOnePower
				<< " -2=" << evidence.minusTwoPower << " +2=" << evidence.plusTwoPower
				<< (vetoSignature
					? (harmonicDecision ? " sig=wrong-note-harmonic" : " sig=wrong-note")
					: " sig=latched-ambiguity")
				<< (harmonicDecision ? " band=2f+3f" : "")
				<< " minH=" << harmonicMinTarget
				<< " - accept refused." << std::endl);
		}
		return true;
	}

	// Tier-0 positive RESCUE (2026-08-31, from Philip's field observation: a correct note
	// that would not register on the first pick registered when he slightly MUTED the
	// fret). Root cause: other strings' ring - a distant pitch dominating the analysis
	// window - makes the bin verdict run FAIL on a perfectly played note until the ring
	// decays; muting removes the ring. The raw comparison is immune to distant ring: a
	// correct note's fundamental still crushes its own semitone neighbours regardless of
	// what else sounds far away. So when the bin gate cannot accept, a strong positive
	// raw confirmation accepts instead. Guards: real energy required, >=5x over the
	// semitone neighbours, and the fundamental must beat the sub-octave and sub-twelfth
	// measures - a lower note's 2nd/3rd harmonic lands exactly on the expected bin and
	// must never masquerade as the note (the octave-wrong trap the bin gate used to
	// catch). The onset gate still applies upstream: a fresh attack is always required.
	bool Tier0ConfirmsExpected(int expectedMidi)
	{
		if (expectedMidi < 0) return false;
		ResearchProtocol::RawToneEvidence evidence;
		const double frequency =
			440.0 * std::pow(2.0, (static_cast<double>(expectedMidi) - 69.0) / 12.0);
		if (!QueryTier0EvidenceWideTarget(expectedMidi, evidence)) return false;
		// While the wrong-note latch is armed for this midi (a veto fired within the
		// last 600ms), the rescue demands the measured polluted-correct energy level
		// instead of the bare noise floor: the multi-string mash that follows a blocked
		// wrong note lights the expected bin with other strings' harmonics at exactly
		// the weak-positive level the floor was letting through (measured live:
		// tgt 9.3e-4 rescue-accepted a latched wrong during a mash).
		const bool latchArmed = g_tier0LatchMidi == expectedMidi
			&& GetTickCount64() - g_tier0LatchTick <= 600;
		if (evidence.targetPower < (latchArmed ? 1.5e-3f : 1e-5f)) return false;
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
		ResearchProtocol::RawToneEvidence subOctave = {};
		ResearchProtocol::RawToneEvidence subTwelfth = {};
		float fifthAbovePower = 0.0f;
		if (!wideTargetPower(frequency * 0.5, subOctave.targetPower)
			|| !wideTargetPower(frequency / 3.0, subTwelfth.targetPower)
			|| !wideTargetPower(frequency * 1.5, fifthAbovePower))
		{
			return false;
		}
		// 0.4x, not 1.0x (2026-09-01, the -12 sweep hole measured live at exp=59): the
		// ringing sub-octave's FUNDAMENTAL read 0.83x its own 2nd harmonic (the "target")
		// - under the old parity bar by a whisker - while a correct note's sub bins read
		// 0.000000 flat. The classes are separated by orders of magnitude; 0.4x sits in
		// the middle with margin for sympathetic low-string ring on a real cable.
		if (subOctave.targetPower >= evidence.targetPower * 0.4f
			|| subTwelfth.targetPower >= evidence.targetPower * 0.4f)
		{
			return false;                  // a lower note's harmonic, not the note itself
		}
		// Octave-phantom betrayal probe (2026-09-01 night, the multi-string mash slip):
		// on guitar, the sub-octave string's FUNDAMENTAL can be weaker than its own 2nd
		// harmonic (the open low E famously so), which walks straight past the guard
		// above - measured live: open-low-E ring rescue-confirmed midi 52 (its exact
		// 2nd harmonic) with subOct reading 30x below the phantom. But a note at f/2
		// cannot hide its 3RD harmonic, which lands at 1.5f - a frequency NOTHING in
		// the expected note's own series occupies. Substantial 1.5f energy = a
		// sub-octave note is sounding and the "fundamental" is its ghost. 0.1x, not
		// 0.25x (2026-09-01 -12 sweep, measured at the exp=59 slip): the ringing
		// sub-octave's 3rd harmonic read 0.21x target - under the old bar by a whisker
		// - while a correct note's 1.5f bin read 0.000000 flat.
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

		g_tier0LatchMidi = -1;             // genuine positive confirmation unlatches
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

	// A/B toggle for the single-note dominance gate (2026-08-31). Default ON (features ship
	// enabled); the bridge can flip it live so the raw permissive matcher can be compared.
	volatile bool g_singleNoteDominanceGate = true;

	// CHORD tier-0 rescue (2026-09-03). Default ON (features ship enabled); the guards are
	// strict enough - EVERY expected tone present + dominant, on a FRESH strum, only after
	// the native scan AND ND both declined - that it can rescue a correct chord but not accept
	// a wrong/partial grab, so a revert (SetChordTier0RescueEnabled(false)) is the A/B, not a
	// safety switch. Floor/margin (1e-4, 2x) still want live confirmation on a real cable.
	// Single notes have a positive tier-0 rescue (Tier0ConfirmsExpected); chords have none,
	// so a correctly strummed chord the native scan + ND both miss just fails, forcing a
	// re-strum (Philip's field report: "a few times i had to keep strumming the correct
	// note"). The chord_verify PoC (tools/ml-string-fret-service/chord_verify.py) showed that
	// KNOWING the expected tones, a per-tone energy check confirms far more chord tones than
	// the blind matcher (Hendrix Purple Haze 51%->81%). Tier0ConfirmsChord ports that idea to
	// the in-game raw-tone API: confirm EVERY expected tone by energy, chord-aware (a
	// neighbour bin that is itself another chord tone is exempt from the dominance test).
	// Conservative on purpose - all tones required + a fresh strum - so it can only rescue a
	// genuinely all-sounding chord, never false-accept a partial/wrong grab. Flip on via the
	// bridge (set_chord_tier0_rescue) to tune the floor/margin live, like the single-note
	// rescue was tuned over several sessions.
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

	// Native's own boundary look-back constant (0.2f @ 0x119AD60): the retrospective span
	// 0x4E6A70 walks when the note window has no usable transport bound - which is exactly
	// the frozen-transport situation NBN creates. Bounding our scan by it restores the
	// temporal precision native gets from its clock window: wrong-note frames age out of
	// the accept surface in 0.2s instead of poisoning the whole hold.
	constexpr double NATIVE_WINDOW_LOOKBACK_SECONDS = 0.2;

	// The permissive native matcher 0x4E7B30 collapses to "the expected bin has ANY energy"
	// for a single note (full decomp: atlas single-note-matcher-0x4E7B30-decomp). Native
	// precision is temporal (the sub-second clock window), which the freeze removes, so the
	// matcher needs an explicit dominance companion - the exact check the matcher's own
	// decomp prescribes for N=1 ("require the expected bin to be the argmax of the
	// histogram"). This is the POSITIVE form: a frame supports the accept only when the
	// expected family [expected-1, expected] IS the dominant peak, or carries at least
	// half the dominant's energy (a correct note masked by a louder still-ringing
	// neighbour string). The family width is DATA, not caution: in every live capture,
	// synthetic and real guitar alike, a correctly played note's dominant reads expected
	// or exactly expected-1 (the low-freq bias), never expected-2 - and a note played one
	// fret BELOW the target reads expected-2 through the same bias, so a [expected-2]
	// edge is precisely the hole that let neighbour frets through (measured live on
	// exp=51: played-flat frames at domPeak=49 passed). With [expected-1, expected] the
	// remaining floor is +/-1 semitone - the peak list is semitone-quantized with +/-1
	// error at guitar frequencies, so adjacent frets are genuinely inseparable in these
	// features; that is native parity, and going tighter needs raw audio (issue #18/#29).
	// The earlier veto form ("override only when expected is essentially absent") passed
	// the synthetic sweep but leaked with a real guitar: real playing lights up enough
	// residual bins that "essentially absent" almost never fires. Ambiguous frames now
	// REJECT - safe under the freeze, because the scan re-evaluates every scoring tick
	// and simply waits for a frame the note actually dominates. Reads the game's own
	// spectral peak list (RING_FRAME_PAIRS), so it works with "Pitch: Off".
	// Per-frame dominance verdict for the persistence run. Three-way, because live capture
	// data showed binary pass/fail starves real notes:
	//  - PASS: the dominant peak reads expected or expected-1 (the reads a sounding correct
	//    note actually produces), or the expected family holds >= 50% of the dominant's
	//    energy (a correct note under a louder still-ringing neighbour string).
	//  - SOFT: no counter-evidence but not proof either. An EMPTY frame (lead silence /
	//    below the -55dB level gate; silence must never veto a run); a WEAK frame whose
	//    dominant carries less than 10% of the loudest peak anywhere in the look-back
	//    window (decay-tail junk: residual harmonics "dominate" such frames at ~1% of the
	//    attack's energy and produced accepts like dom=[45] and phantom dom=[39] on
	//    frames with domE under 1.0 while real notes measure 60-250 - too weak to testify
	//    for OR against, so weak frames neither pass nor veto); or a strong dominant at
	//    expected+1: the semitone quantization jitters +/-1 at the low strings (a correct
	//    low E reads {38,39,40} across frames, measured live), so the high tail cannot be
	//    treated as a different note - but it is also what a +2-fret wrong note reads, so
	//    it must not accept ALONE.
	//  - FAIL: any other STRONG dominant - INCLUDING expected-2. No correct note ever
	//    read expected-2 in any capture, but a -2-fret wrong note reads {exp-3..exp-1}
	//    and leaked through repeated strums while exp-2 was merely Soft: its own-pitch
	//    frames must veto the run (measured: -2 leaked on strums 3-6 at exp=39/42 with
	//    exp-2 Soft; +2/+3 held everywhere).
	// There is deliberately NO family-energy escape anymore: "expected family >= 50% of
	// the dominant" leaked twice (real-guitar residual bins at 27%+ of rich spectra, then
	// weak-frame accepts at dom=[45]); the freeze waits, so a masked correct note simply
	// accepts a few frames later when it actually dominates.
	enum class FrameVerdict { Fail, Soft, Pass };

	FrameVerdict SingleNoteFrameDominanceVerdict(uintptr_t ring, int32_t writeCursor,
		int32_t capacity, int32_t frameOffset, int expectedMidi, float windowMaxEnergy,
		int& outDomMidi, float& outDomEnergy, float& outFamilyEnergy)
	{
		outDomMidi = -1;
		outDomEnergy = 0.0f;
		outFamilyEnergy = 0.0f;
		int32_t idx = writeCursor - frameOffset;
		if (idx < 0) idx += capacity;
		const uintptr_t frame = ring + static_cast<uintptr_t>(idx) * DETECTOR_RING_STRIDE;
		int32_t count = 0;
		if (!TryRead(frame + RING_FRAME_PAIR_COUNT, count) || count <= 0)
		{
			return FrameVerdict::Soft;                  // empty frame: silence is not disproof
		}
		if (count > 48) count = 48;
		int domMidi = -1;
		float domEnergy = 0.0f;
		float expectedFamilyEnergy = 0.0f;
		for (int32_t i = 0; i < count; ++i)
		{
			int32_t midi = -1;
			float energy = 0.0f;
			if (!TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8, midi)
				|| !TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8 + 4, energy)
				|| midi < 0 || midi >= 128 || !std::isfinite(energy) || energy <= 0.0f)
			{
				continue;
			}
			if (energy > domEnergy) { domEnergy = energy; domMidi = midi; }
			// Expected family: the expected bin and the -1 low-freq-bias neighbour.
			if (midi >= expectedMidi - 1 && midi <= expectedMidi && energy > expectedFamilyEnergy)
			{
				expectedFamilyEnergy = energy;
			}
		}
		outDomMidi = domMidi;
		outDomEnergy = domEnergy;
		outFamilyEnergy = expectedFamilyEnergy;
		if (domMidi < 0 || domEnergy <= 0.0f) return FrameVerdict::Soft;
		if (domEnergy < windowMaxEnergy * 0.1f) return FrameVerdict::Soft;  // too weak to testify

		// FALSIFIED EXPERIMENT (2026-08-31, keep for the record): a "high-register bias
		// anchor" briefly made the expected-1 read the ONLY proof above MIDI 45, on the
		// observation that eleven measured correct notes all read expected-1 up there. It
		// STARVED a perfectly played E4 (G string fret 9): that note reads dead-on at 64
		// with huge energy (dom=[64,64,64], domE up to 780, ndLoudest=64) on every frame.
		// The flat read bias is real but NOT consistent per pitch - it varies per
		// note/string - so an at-pitch read can never be treated as evidence of a sharp
		// wrong. Consequence: the +/-1 semitone floor genuinely cannot be beaten in these
		// semitone-quantized features; that discrimination needs raw cable audio
		// (cent-resolution - the tier-0 Goertzel plan on ticket #29).
		if (domMidi >= expectedMidi - 1 && domMidi <= expectedMidi) return FrameVerdict::Pass;
		if (domMidi == expectedMidi + 1) return FrameVerdict::Soft;
		return FrameVerdict::Fail;
	}

	// The SINGLE-NOTE accept, adapted for the freeze (2026-08-30, from the full decomp of the
	// native path GamePlaysongLAS +0xF0 -> 0x7E2640 -> 0x7B8820 -> 0x4E9470 -> scan 0x4E6A70 ->
	// onset gate 0x4E4B60 + matcher 0x4E7B30). Native accepts when the TRANSPORT clock state+0xD08
	// is inside the note's short time window AND the matcher sees expected energy PRESENT. The
	// matcher is deliberately permissive (a lone note passes when the expected bin has ANY energy);
	// native stays honest only because that window is a fraction of a second. Under the NBN freeze
	// state+0xD08 stalls (proven audio-vs-transport clock split), so the window never closes and
	// the permissive check accepts stray leakage from a wrong fret. A 1:1 native port is therefore
	// IMPOSSIBLE here - the discrimination lived in a window the freeze removes.
	//
	// So we keep the game's own inputs - the spectral peak list the matcher histograms, and the
	// ring onset flag +0x7B5 (the real 0x4E4B60 attack gate) - but replace "expected present" with
	// "expected DOMINATES the spectrum": the loudest bin must be in expected's harmonic family
	// (ExpectedInHarmonicFamily / TryFrameDominantBin). That is the discrimination native got for
	// free from its short window, restored explicitly. Works with "Pitch: Off" because it reads the
	// raw spectrum, never the pitch estimate. Driven by our hold-latch ring time (audio-stamped
	// frame timestamps keep advancing while frozen), exactly like the chord port. This is a
	// deliberate, DOCUMENTED divergence from native, forced by the freeze - not a guess.
	bool NativeSingleNoteHitSinceLatch(int expectedMidi, double sinceRingTime)
	{
		if (expectedMidi < 0) return false;
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

		// Native's boundary-path temporal bound: only frames within the last 0.2s (the game's
		// own look-back constant) are eligible, walked back from the head frame's AUDIO time.
		// The earlier port scanned everything since the hold latch - an unbounded, growing
		// span where one permissive frame-match anywhere wins; with a real guitar (many bins
		// always lit) that surface leaked wrong frets constantly. sinceRingTime still floors
		// the walk so nothing from before this note's latch ever counts.
		double headTs = 0.0;
		if (!frameTimeAt(0, headTs)) return false;
		const double oldestEligibleTs = headTs - NATIVE_WINDOW_LOOKBACK_SECONDS;
		auto frameEligible = [&](int32_t frameOffset, double& ts) -> bool
		{
			return frameTimeAt(frameOffset, ts) && ts > sinceRingTime && ts > oldestEligibleTs;
		};

		// Onset gate (FUN_004E4B60 boundary path): a fresh committed attack (+0x7B5) must
		// appear within the look-back - a RECENT attack, not any attack since the latch.
		bool onsetFound = false;
		for (int32_t frameOffset = 0; frameOffset < scanLimit; ++frameOffset)
		{
			double frameTs = 0.0;
			if (!frameEligible(frameOffset, frameTs)) break;
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

		// Scan (FUN_004E6A70 boundary path): the native single-note matcher (0x4E7B30) must
		// accept at some frame INSIDE the 0.2s look-back, and - because the N=1 matcher is a
		// bare presence test (decomp) - the accept must be corroborated by DOMINANCE over a
		// short PERSISTENCE run. A single dominant frame is not enough: a strum's attack
		// transient is broadband for its first frame or two, and the peak-picker can
		// momentarily drop the dominant into the expected family on exactly those frames -
		// so every re-strum of a wrong note was a fresh dice roll, and repeated strumming
		// leaked consistently. The run is the matched frame plus its two older neighbours
		// (~30ms), judged with the three-way verdict above rather than all-must-pass:
		// binary all-pass starved real notes on the measured low-E jitter and lead-silence
		// frames. Cost is at most ~2 ring frames of extra latency right after the attack -
		// the freeze waits, later ticks re-scan, imperceptible in practice mode.
		constexpr int32_t DOMINANCE_PERSIST_FRAMES = 3;

		// The loudest spectral peak anywhere in the eligible look-back: the verdict's
		// weak-frame floor normalizes against it, so "weak" tracks the player's own level
		// (a strum's decay junk is ~1% of its attack) instead of a hard-coded energy.
		float windowMaxEnergy = 0.0f;
		for (int32_t frameOffset = 0; frameOffset < scanLimit; ++frameOffset)
		{
			double frameTs = 0.0;
			if (!frameEligible(frameOffset, frameTs)) break;
			int32_t idx = writeCursor - frameOffset;
			if (idx < 0) idx += capacity;
			const uintptr_t frame = ring + static_cast<uintptr_t>(idx) * DETECTOR_RING_STRIDE;
			int32_t count = 0;
			if (!TryRead(frame + RING_FRAME_PAIR_COUNT, count) || count <= 0) continue;
			if (count > 48) count = 48;
			for (int32_t i = 0; i < count; ++i)
			{
				float energy = 0.0f;
				if (TryRead(frame + RING_FRAME_PAIRS + static_cast<uintptr_t>(i) * 8 + 4, energy)
					&& std::isfinite(energy) && energy > windowMaxEnergy)
				{
					windowMaxEnergy = energy;
				}
			}
		}

		// Current-sound verdict run: the NEWEST three eligible frames - what is sounding
		// RIGHT NOW - judged once per tick, independent of where the matcher fires. The
		// run was previously anchored at the matched frame and looked only OLDER, which
		// starved a measured real case: a correct note reading cleanly at expected-1
		// leaves the expected bin empty on its strong frames (the exact-bin matcher
		// cannot fire there) and the matcher instead fires on a weak jitter frame deeper
		// in the look-back, whose older neighbours are junk - while the strong PASS
		// evidence sat in newer frames the run never examined. Accept therefore means:
		// expected energy appeared somewhere recent (the matcher), AND the current sound
		// supports expected (no Fail, at least one Pass, in the newest three frames).
		int domMidi[DOMINANCE_PERSIST_FRAMES] = { -1, -1, -1 };
		float domEnergy[DOMINANCE_PERSIST_FRAMES] = {};
		float familyEnergy[DOMINANCE_PERSIST_FRAMES] = {};
		bool anyFail = false;
		int passCount = 0;
		for (int32_t k = 0; k < DOMINANCE_PERSIST_FRAMES; ++k)
		{
			double probeTs = 0.0;
			if (k >= scanLimit || !frameEligible(k, probeTs))
			{
				continue;                            // ineligible frame -> Soft
			}
			const FrameVerdict verdict = SingleNoteFrameDominanceVerdict(ring, writeCursor,
				capacity, k, expectedMidi, windowMaxEnergy, domMidi[k], domEnergy[k],
				familyEnergy[k]);
			if (verdict == FrameVerdict::Fail) { anyFail = true; break; }
			if (verdict == FrameVerdict::Pass) { ++passCount; }
		}
		const bool currentSoundSupportsExpected = !anyFail && passCount >= 1;

		// Strong-run accept, matcher-independent: two or more PASS frames among the newest
		// three (the expected family dominating the current sound with real energy) is
		// stronger evidence than the exact-bin matcher's "any energy at the expected bin".
		// Needed because the detector's -1 read bias can put a correct note's ENTIRE energy
		// into the expected-1 bin (measured live at expected=51: clean strong 50-dominant
		// frames, bin 51 empty on every frame), so the exact-bin matcher never fires at all
		// and the note starves. Discrimination is unchanged: a >= 2-fret wrong produces a
		// Fail frame or no Pass frames either way; the +/-1 semitone floor is identical.
		// The onset gate above still applies - a fresh attack is always required.
		// The tier-0 veto is evaluated at most once per scoring tick (2026-09-01): both
		// accept paths consult the same 0.15s of ring audio, and a veto must no longer
		// abort the tick outright - the positive rescue at the bottom still gets its
		// look. (Veto and rescue are near-mutually-exclusive on the same evidence, but
		// they query moments apart; the old early-return silently disabled the rescue
		// for the whole tick.)
		int tier0VetoState = -1;                       // -1 unknown, 0 clear, 1 vetoed
		auto tier0VetoedThisTick = [&]() -> bool
		{
			if (tier0VetoState < 0)
			{
				tier0VetoState = Tier0VetoesAccept(expectedMidi) ? 1 : 0;
			}
			return tier0VetoState == 1;
		};

		if (g_singleNoteDominanceGate && !anyFail && passCount >= 2
			&& !tier0VetoedThisTick())
		{
			LOG_INFO("(NBN SNSCAN) single-note hit expected=" << expectedMidi
				<< " via dominant current-sound run (matcher-independent: dom=[" << domMidi[0]
				<< "," << domMidi[1] << "," << domMidi[2] << "] domE=[" << std::fixed
				<< std::setprecision(1) << domEnergy[0] << "," << domEnergy[1] << ","
				<< domEnergy[2] << "])." << std::endl);
#if defined(_DEBUG)
			LogTier0ShadowEvidence(expectedMidi, "ACCEPT(run)");
			LogTier1ShadowPitch(expectedMidi);
#endif
			return true;
		}

		for (int32_t frameOffset = 0; frameOffset < scanLimit; ++frameOffset)
		{
			double frameTs = 0.0;
			if (!frameEligible(frameOffset, frameTs)) break;
			if (CallNativeSingleNoteMatcher(expectedMidi, frameOffset) != 1) continue;

			if (!g_singleNoteDominanceGate)
			{
				LOG_INFO("(NBN SNSCAN) single-note hit expected=" << expectedMidi
					<< " at frame " << frameOffset << " (raw permissive matcher, gate A/B off)."
					<< std::endl);
				return true;
			}

			if (!currentSoundSupportsExpected)
			{
				// The matcher fired somewhere in the look-back but the current sound does
				// not support expected. The bin verdict cannot change within this tick, so
				// stop scanning and fall through to the tier-0 rescue: distant string ring
				// FAILs the bin run on a genuinely correct note (Philip's mute-to-register
				// symptom), and only the raw evidence can tell that apart from a wrong
				// note. Throttled - this fires per tick while a wrong note rings.
				static ULONGLONG lastRejectLogTick = 0;
				const ULONGLONG nowTick = GetTickCount64();
				if (nowTick - lastRejectLogTick >= 250)
				{
					lastRejectLogTick = nowTick;
					LOG_INFO("(NBN SNSCAN) matcher accepted expected=" << expectedMidi
						<< " at frame " << frameOffset << " but the current sound does not"
						<< " support it: dom=[" << domMidi[0] << "," << domMidi[1] << ","
						<< domMidi[2] << "] domE=[" << std::fixed << std::setprecision(1)
						<< domEnergy[0] << "," << domEnergy[1] << "," << domEnergy[2]
						<< "] famE=[" << familyEnergy[0] << "," << familyEnergy[1]
						<< "," << familyEnergy[2] << "] - rejected." << std::endl);
#if defined(_DEBUG)
					LogTier0ShadowEvidence(expectedMidi, "REJECT");
					LogTier1ShadowPitch(expectedMidi);
#endif
				}
				break;
			}

			if (g_singleNoteDominanceGate && tier0VetoedThisTick())
			{
				// Vetoed, but do not abort the tick: fall through to the rescue below,
				// which requires the opposite evidence and so stays a safe last look.
				break;
			}
			LOG_INFO("(NBN SNSCAN) single-note hit expected=" << expectedMidi
				<< " via native matcher 0x4E7B30 at frame " << frameOffset
				<< " (0.2s window, current-sound dominant: dom=[" << domMidi[0] << ","
				<< domMidi[1] << "," << domMidi[2] << "] domE=[" << std::fixed
				<< std::setprecision(1) << domEnergy[0] << "," << domEnergy[1] << ","
				<< domEnergy[2] << "] famE=[" << familyEnergy[0] << "," << familyEnergy[1]
				<< "," << familyEnergy[2] << "])." << std::endl);
#if defined(_DEBUG)
			LogTier0ShadowEvidence(expectedMidi, "ACCEPT(matcher)");
			LogTier1ShadowPitch(expectedMidi);
#endif
			return true;
		}

		// Tier-0 rescue: the bin paths could not accept (distant ring FAILing the run, or
		// the -1 read bias leaving the exact bin empty), but a fresh attack was present and
		// the raw route positively confirms the expected fundamental. See
		// Tier0ConfirmsExpected for the guards.
		if (g_singleNoteDominanceGate && g_tier0Enforcement && Tier0ConfirmsExpected(expectedMidi))
		{
			LOG_INFO("(NBN SNSCAN) single-note hit expected=" << expectedMidi
				<< " via tier-0 raw confirmation (bin gate could not decide)." << std::endl);
			return true;
		}
		return false;
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
		// THE RESIDUAL CORRUPTOR (2026-08-27, found offline in the runtime image):
		// StartAt core 0x4749C0 is a FROZEN-OBJECT method - its prologue does
		// `mov esi, eax; mov word [esi+0x5E3], 1` unconditionally, writing the
		// frozen-on-tag field on entry. On a real frozen-object that field is in
		// bounds; on a GamePlaysongLAS owner (proven 0x5D0 bytes) +0x5E3 is 0x13
		// past the object end, smashing the next heap block's free-list entry -
		// exactly the owner+0x5E0 corruption the dumps showed. This call ran on
		// EVERY release (isNativeReleaseEnabled default ON), so it corrupted even
		// with the freeze flag off, which is why the zero-writes flight still
		// died. The coordinated PlayerSong rebuild that follows carries the music
		// on its own (the code below and both pure-native attempts confirm StartAt
		// never restarts the PlayerSong), so refusing this call on a LAS owner
		// loses nothing but the smash.
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

	// Native-drive step 3, HYBRID form (third iteration, 2026-08-25 evening):
	// the owned release runs the StartAt core (the game's own unfreeze + seek +
	// speed reset + show-state tail) and then the proven coordinated PlayerSong
	// restart for the music itself - the two pure-native attempts left the
	// PlayerSong stopped (see the falsified-interpretations note below), so the
	// play packet stays mechanical until the native music restart is found.
	// DEFAULT ON (Philip, 2026-08-25 night: "if we are testing a feature then
	// why wouldnt we always have it enabled" - the toggle is an emergency
	// revert, not an opt-in).
	volatile bool isNativeReleaseEnabled = true;
	// The +0x348 component's +0x9C byte: "presentation active" (see the
	// falsified-interpretations note below) - read for the ground log only.
	constexpr uintptr_t OWNER_PRESENTATION_COMPONENT = 0x348;
	constexpr uintptr_t PRESENTATION_ACTIVE_FLAG = 0x9C;

	// Step 5 first flight (2026-08-25 night): the game's freeze is a MODE
	// TRANSITION - GE_FreezeSong's core 0x474840 (EAX=owner) sets owner+0x5E2
	// and calls wrapper->vtbl+0x20(2,0), which stands up a fresh 0x620-byte
	// frozen-mode object (factory 0x46C650, ctor 0x471A30, vtable 0x011A0B70
	// - 16 overridden slots diffed in the rewrite plan) and makes it the
	// active PlaySong-GE. The frozen class's own update (+0x6C) watches
	// deadline doubles at +0x5E8/+0x5F8 and unfreezes ITSELF natively. Our
	// flag-writes never produced the freeze presentation because the
	// presentation IS this other object. 0x474890 (ECX=owner) is the exact
	// mirror: runs the ResumeFromTag core if +0x5E3 is set, then mode (0,2).
	// Both cores are self-contained (they resolve the 0x0135F54C root and
	// bracket the call themselves), so the experiment calls THEM, replicating
	// nothing. One-shot per arm (bridge freeze-mode-test): the next
	// established hold enters mode 2 after our mechanical freeze, and its
	// release exits before the mechanical restart - belt and braces while
	// the observation is presentation behavior during the hold.
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

	// FALSIFIED interpretations, banked so they are not retried (live A/B trail,
	// 2026-08-25 evening):
	//   - "[owner+0x348]+0x9C is a deferred start consumed by 0x4729E0" (handover
	//     20) is WRONG on both halves. 0x4729E0 is GE_HideGame's core (its sole
	//     caller 0x405140 is GE_HideGame's body): calling it faded the running
	//     game to black - the lesson hide transition - and its flag write is part
	//     of hiding. Its sibling 0x472A20 (flag <- 1) is GE_ShowGame's core, and
	//     the StartAt core ends with the same show tail, so the byte reads as
	//     "presentation active": 1 is the steady running state, not an armed
	//     start. Never gate anything on it reading 0.
	//   - Neither the StartAt core alone NOR StartAt + 0x4729E0 restarts the
	//     PlayerSong after a Stop_TMusic latch: both live attempts ended with
	//     running=0 stopped=1 and the transport frozen on an accepted note. The
	//     music restart in this call set is nowhere; the coordinated PlayerSong
	//     restart (COORDINATED_REBUILD) remains the only proven play packet.

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

	// The owner whose +0x5E3 byte this controller last set, so every release and
	// abandon path can unwrite it. Clears deliberately ignore the enable toggle:
	// freeze-flag-off issued mid-hold must not leak a set byte into native state,
	// and an abandoned hold must not leave the game believing it is frozen-on-tag
	// (2026-08-25, the Riff Repeater timeline investigation).
	void* freezeFlagSetOwner = nullptr;

	void SetNativeFreezeFlag(void* owner, bool frozen)
	{
		if (owner == nullptr) return;
		if (frozen && !isNativeFreezeFlagEnabled) return;
		if (!frozen && owner != freezeFlagSetOwner) return;
		// First-hold corruption guard (2026-08-26/27 enable-crash campaign): on a
		// fresh session's first hold the owner's freeze-tag fields can read
		// uninitialized (tagLen in the billions, resumeFlag garbage like 59/93;
		// healthy sessions read ~1.4M and 0/1). Announcing frozen-on-tag then
		// sends the game's scheduler walking a garbage-length tag, and the game
		// corrupts its own heap - 0xc0000409/0xc0000374 on the main thread with
		// no mod frames, seven dumps preserved in the atlas nbn-enable-crash
		// folder. Every first-enable with this write skipped survived. So the
		// write requires a sane tag state; a skipped hold simply runs without the
		// frozen-on-tag announcement (the proven-safe freeze-flag-off behavior)
		// and later holds re-evaluate once the game initializes the structure.
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
			// v2 (2026-08-27 morning): an EMPTY tag is uninitialized too. The v1
			// guard allowed tagLen=0 as "pristine" and the first enable crashed
			// with exactly tagLen=0 (resumeFlag read 168 garbage moments later).
			// The flag may only announce frozen-on-tag over a real populated tag;
			// until the game initializes one, holds run flag-less, which the
			// 35-enable bisect session proved fully playable.
			const bool tagSane = readable
				&& tagBegin != 0
				&& tagEnd > tagBegin
				&& tagLength <= TAG_LENGTH_SANITY_LIMIT
				&& (resumeFlag == 0 || resumeFlag == 1);
			if (!tagSane)
			{
				// v4 (2026-08-27): the "tag" fields are the INTERNALS of the
				// std::string at owner+OWNER_FREEZE_TAG_STRING, which only the
				// frozen-object constructor initializes (see the freeze-mode-test
				// block below for the discovery trail). v3's zeros made a
				// worse-formed string than the garbage; the valid empty state is
				// the constructor's own: begin field -> the inline buffer, end
				// field -> the address of the begin field, first buffer byte NUL.
				// The constructor-only bytes (+0x5E2 frozen-song, +0x5E3
				// frozen-on-tag, resume flag, freeze start) zero alongside.
				// The announce is still withheld this hold.
				// v5 (2026-08-27): NO WRITES. The v4 sanitizer's 7 writes into this
				// region took the crash rate to ~100%, and the lattice bracketed
				// the corruption to exactly that window - these offsets are
				// suspected to lie PAST the end of the GamePlaysongLAS allocation
				// (frozen-object fields on a smaller class). Read-only log; the
				// owner-size diagnostic below settles the layout question.
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

	// The two-day crash family's fix of record (2026-08-27): the owner's freeze
	// tag std::string and its sibling bytes are initialized ONLY by the
	// frozen-object constructor, so a fresh GamePlaysongLAS owner carries garbage
	// there. The FIRST native path that assigns into that string - the game's
	// FreezeOnTag machinery when the freeze flag announces, or the StartAt core's
	// tag assignment at our release - consults the garbage capacity/pointer and
	// frees a wild pointer: heap corruption, detected milliseconds later on the
	// main thread with no mod frames. One surviving assignment normalizes the
	// string, which is why sessions that survived their first hold were then
	// stable for hundreds of holds. So: initialize the string to the valid empty
	// inline form (the constructor's own layout: begin -> inline buffer,
	// inline marker -> the begin field's address, NUL first byte) once per owner,
	// at bootstrap, before any freeze machinery can touch it.
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
			// Step 3: bookkeeping teardown (the notes are still credited through
			// MarkNotes), then the game's own resume. The empty-tag StartAt seeks
			// to the CURRENT transport position, which during a frozen hold is the
			// held epoch; a releaseEpoch that differs is logged so the A/B shows
			// whether the coordinated rebuild's explicit epoch ever mattered.
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

	// A controller fault: the native state diverged from this controller's selection/hold.
	// Philip's STANDING RULE is that NBN must never turn itself off, so recover in place
	// instead of calling HandleControllerFault (which routed to DisableAutomatic and dropped
	// the player out of the feature). During fast Riff Repeater enumeration the fault is a
	// transport discontinuity from the section change; abandon any owned hold safely
	// (ResetBootstrap clears the freeze flag via the live owner so the transport is never
	// left stuck) and re-bootstrap, and the authored-grid timeline re-latches the current
	// section on the next tick. The name is kept for its ~30 callers; only the response
	// changed from "disable" to "recover".
	void FaultWithoutRelease(const std::string& reason)
	{
		void* owner = trackedOwner;
		LOG_ERROR("(NBN LAS FAULT) Recovering in place instead of disabling NBN ("
			<< reason << "); re-bootstrapping for the current section." << std::endl);
		ResetBootstrap(reason.c_str(), owner);
		trackedOwner = owner;
	}

	// liveOwner: the caller's proof of a still-current owner object. When it is
	// the same owner the frozen-on-tag byte was set on, the abandon unwrites it -
	// the RR range march abandons holds on a still-live owner, and a leaked 1
	// leaves native consumers in the freeze presentation (2026-08-25). Without
	// that proof no write happens (a freed owner's byte no longer matters, and
	// writing into recycled memory is the chord-panel corruption class); the
	// tracking is dropped so a later release cannot write to the stale pointer.
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

	// legatoReferenceString is the string that is already ringing, which is what decides
	// whether a hammer-on or pull-off is physically playable and therefore whether it may
	// be a target in its own right. It differs by caller and getting it wrong hangs the
	// feature. A fresh selection compares against the last committed note, so it passes
	// previousSelectedString. A successor search runs while a hold is still owned, so the
	// ringing string is the currently selected one and it must pass selectedString.
	//
	// Passing previousSelectedString from the successor search, as an earlier version did,
	// compares a candidate against the note before the one being held. A pull-off from the
	// held note then fails the skip test whenever the note before it was on another string,
	// so it is queued as its own target. Nothing can ever satisfy it, because a pull-off
	// produces no pick attack, and the hold never releases: Philip saw this as the game
	// freezing after playing a note, with scoring still ticking at the frozen epoch.
	// Reports how the live vector is distributed across phrase iterations, and where the
	// selected note sits within that.
	//
	// Diagnostic only: it refuses nothing. Philip reported the controller targeting grey
	// notes from outside the Riff Repeater loop after a turnover, and a capture showed the
	// selected note jumping from phraseIteration 2 at 19.045 to phraseIteration 1 at 11.714.
	// Gating selection on the iteration is the obvious fix, but it is the same shape as the
	// section-end boundary filter that is disabled below: refuse targets by an inferred
	// boundary and, if the boundary is wrong, every later note is played unheld and turns
	// grey. That filter produced exactly the symptom it was meant to cure.
	//
	// So the question has to be answered first: does a Riff Repeater loop occupy one phrase
	// iteration or several? If one, gating is safe and precise. If several, gating would
	// reproduce the grey notes and the loop range has to be read directly instead.
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
			// Child records are continuations sounded by their parent, so they are not
			// targets in their own right. A bend is the exception. Rocksmith authors a bend
			// as a parent and a child on the same string and fret a fraction of a second
			// apart, and the child is the return, which the player has to pick again.
			// Skipping it made the bend accept itself the moment the parent was played:
			// Philip reported the bend back auto-completing when it required re-picking,
			// and reproduced it on a different fret, which is what makes it structural
			// rather than specific to one note.
			if ((note.mask & NOTE_MASK_CHILD) != 0
				&& (note.mask & NOTE_MASK_BEND) == 0)
			{
				return true;
			}
			if ((note.mask & NOTE_MASK_IGNORE) != 0) return true;
			// A note Rocksmith has already scored cannot be a target. Holding on one asks
			// the player to articulate something the engine considers finished, and the
			// hold ends the moment it starts: CommitBeforeRelease commits as soon as it
			// sees stateC0 and stateC1 both set, which is already true on arrival. Philip
			// saw this as the target skipping a note, and as notes completing by
			// themselves. One was observed committing after two scoring ticks with no
			// matching onset logged at all.
			//
			// This reads the engine's own per-note scoring state rather than assuming
			// anything about how a phrase is authored, so it holds for any chart. The pair
			// is the same one the commit path already treats as authoritative.
			if (note.stateC0 != 0 && note.stateC1 != 0) return true;
			// Hammer-ons and pull-offs ARE targets (Philip, 2026-08-18). The feature is
			// the note sequence, not the technique: every note is its own stop on the
			// timeline, revealed one at a time on the fretboard. The earlier run-as-one-
			// gesture design (skip continuations as targets, confirm the run by pitch
			// under one hold) is retired with it - it existed to serve the old
			// presentation, which kept the whole group lit. Acceptance for a legato
			// target needs no pick: the isLegatoTarget pitch fallback confirms it from
			// the detector, and the hit-decision override forces the native commit. The
			// "played twice" failure that once justified skipping these predates that
			// override. legatoReferenceString is retained by the callers but no longer
			// consulted.
			// Grey lead-in notes are strictly earlier than the section boundary. The note
			// at the boundary time is a real target (Philip-confirmed in game: the open E
			// at the boundary rendered as a normal note and turned grey only after being
			// missed).
			if (note.recordTime < greyCutoff - GREY_EPSILON) return true;
			// A note AT OR beyond Rocksmith's selected section end belongs to the NEXT loop
			// iteration and can never be reached before the loop restarts, so it must not
			// become a target. The section is a half-open interval [start, end): the authored
			// phrase-section end equals the NEXT iteration's start byte-for-byte (verified
			// live 2026-08-28 via the phrase-section vector at owner+0x78 -> +0xF4/+0xF8,
			// stride 0x58, end at +0x28: entry[4].end == entry[5].start == 0x42183333 ==
			// 38.04999923706055, and the leaking note's recordTime IS that exact boundary
			// value). The old strict "> end + GREY_EPSILON" left that boundary note eligible,
			// so once the section's own notes were consumed it was selected and demanded a
			// commit before the loop could restart - Philip's "extra note before looping"
			// (blocker #1). Excluding recordTime >= end drops it. The START gate above stays
			// strict-less because a note AT the section start IS a real target
			// (Philip-confirmed); only the END side is half-open-exclusive.
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
			ResearchProtocol::ExpectedAttackEventKind::CandidateChanged);
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
			// expectedMidi is the AUTHORED tuning (0x1199D2C, the player's own guitar frame) plus
			// the fret, plus the INPUT pitch shift. The detector table (state+0x134C) is NOT used:
			// it reads the Eb chart frame under Speaker Mode while the input stays in the E guitar
			// frame, which put expected a semitone low and accepted the fret below (Philip, live
			// 2026-08-30: Speaker Eb->E, red fret 7 is detected as E fret 7 = 47, not 46). inputShift
			// is 0 for Speaker/Off (the input is not retuned) and the Drop amount for the Drop Pedal
			// (which DOES retune the input), so authored+fret+inputShift == the pitch the detector
			// actually reports for the correct fret, in every mode. See HostGetInputOnsetShiftSemitones.
			const int inputShift = ResearchProbeRuntime::GetInputOnsetShiftSemitones();
			// Detector frame (Speaker/Off, inputShift==0): open-string MIDI from state+0x134C +
			// fret - the frame the detector reports the input in (open low E = 39 under Eb Speaker
			// Mode). Verified "correct for notes and chords"; the authored 0x1199D2C table read the
			// shifted E frame (40) and stuck the note. Authored path stays the fallback / Drop Pedal
			// (input retuned) branch.
			// Expected in the PLAYER'S PHYSICAL frame - the tuning the detector actually hears. Philip
			// plays E standard, so an open low E is 40 (E); the authored table (0x1199D2C) + fret +
			// inputShift gives that. Reading the detector's own resolved table (state+0x134C) was WRONG
			// here: under Speaker Mode it holds the Eb CHART frame (39), so expected chased the
			// detector's E<->Eb FLICKER value instead of the physical E the player produces
			// (live-confirmed, E-standard guitar). The low-string flicker is tolerated at the accept.
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
			// Terminal-fault autopsy (2026-08-24 beta): two play-through records in a
			// row let the running transport overshoot the next chord's boundary by
			// 0.276s, and faulting here disabled the whole feature mid-song. A record
			// that can no longer be held cleanly is one missed hold, not a controller
			// failure: it plays through natively like any suppressed record and the
			// scan selects the next target once it expires.
			isHoldSuppressed = true;
			LOG_ERROR("(NBN LAS HOLD) Hold boundary overshot by "
				<< std::fixed << std::setprecision(3) << (updateTime - selectedHoldTime)
				<< "s (max " << HOLD_BOUNDARY_MAX_OVERSHOOT << ") for record=0x"
				<< std::hex << selectedRecord << std::dec
				<< "; the record plays through natively instead of faulting."
				<< std::endl);
			return false;
		}
		// FreezeOnTag pins the owner to the exact compensated event epoch. Using the
		// already-overshot callback time lets the noteway retire the target after the
		// first held frame, so its native prompt instance can no longer animate.
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
				// Diagnostic for the chord prompt route (Philip's strum-line desync:
				// without the prompt, the chord panel floats short of the line). The
				// chord's Vfx controller carries its own vtable; logging it names the
				// slots to validate in Ghidra so chord arming can join
				// ArmSelectedNativePrompt.
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
		// #55/#56 (Philip's "sweep on note freeze", 2026-08-24 late): the
		// Play_FreezeNoteTrack event is what sweeps the note track at freeze, and
		// the armed NoteVfx prompt is what carries the frozen note through that
		// sweep. A chord cannot arm the prompt, so dispatching the event swept the
		// chord's own panel off the highway and the player read the NEXT chord as
		// the target. The freeze itself is Stop_TMusic plus the pinned clocks
		// (record-agnostic, handover 17), so the event is presentation-only and
		// skipped for chords: the chord panel stays on the unswept track.
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
		// First-hold corruption lattice (2026-08-27): the fresh-session first hold
		// crashes ~70% with heap corruption detected moments later, and the coarse
		// hold-established checkpoint reads clean - so the corrupting write is
		// bracketed step by step through establishment and the early ticks.
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
		mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());

		// Flow-through grace (2026-09-01): a pick played while hit decisions were
		// blocked (dense rebuild, commit, release) left an attack spike behind and its
		// string is still ringing now. Open the raw-confirmation reattack window so
		// the input-release wait can accept that sustain through tier-0 instead of
		// discarding it as carry-over and forcing a second pick. The window only ever
		// ACCEPTS on the full tier-0 positive confirmation (or an exact detector
		// match), and same-pitch successors stay barred by allowAccept, so a
		// predecessor's ring cannot ride this in.
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
		// The spike-reattack window must not survive into a new hold (Philip's
		// one false progression, 2026-08-25 evening: a chord strum's spike opened
		// the window, the latch landed ~100ms later, and the chord's ringing
		// D-string tone matched the successor's expected 55 for 3 ticks -
		// accepted at 4 held ticks without a fresh pick). A decaying ring cannot
		// spike, so requiring the spike to happen DURING the hold closes the
		// leak; a genuinely early pick is still covered by the primed-onset
		// stash, which rides the native edge rather than the level meter.
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
			// First flight round 1 (2026-08-25) was a NO-OP: owner+0x5E2 is only
			// ever initialized by the frozen-object constructor, so a LAS owner
			// carries garbage there (0x97/0xE8 observed live) and the core's
			// "already frozen" check skips the whole transition. Zero it first
			// so the game sees not-frozen and actually performs mode (2,0).
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
		// One-shot owner allocation measurement (2026-08-27): the freeze-family
		// fields sit at +0x5C8..+0x5E8, offsets proven on the frozen-object class.
		// If GamePlaysongLAS's own allocation is smaller, every freeze-family
		// write was an out-of-bounds heap smash - the two-day crash family in one
		// number. HeapSize against each process heap finds the block's true size.
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
			// Transpose-frame verification (single-note fix, 2026-08-30): expectedMidi is now
			// built in the detector's tuning frame (state+0x134C[string] + state+0x1358 + fret),
			// the same frame the onset is detected in, so a correctly fretted note reads the
			// same MIDI. authoredTemplateTone/inputShift stay logged only to confirm the input
			// onset lands on expectedMidi live.
			<< "; authoredTemplateTone=" << selectedNativeTone
			<< " inputShift=" << ResearchProbeRuntime::GetInputOnsetShiftSemitones()
			<< ". Scoring and the native onset detector continue at the frozen time."
			<< std::endl);
		EmitCandidateEvent(owner, updateTime, selected,
			ResearchProtocol::ExpectedAttackEventKind::HoldEstablished);
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
		// Chord successors take the dense rebuild too (2026-08-24 desync session).
		// The old chordId bounce sent every tight chord chain through the plain
		// release path, whose skipped noteway rebuild is exactly the stale-visual
		// desync the dense machinery was built to cure for fast singles: the panel
		// showed one chord while the evaluator judged another, refusals piled into
		// safety releases, and the same chordId that accepted at wide spacing
		// refused at tight spacing. Only chords that would not HOLD are excluded.
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
			const int inputShift = ResearchProbeRuntime::GetInputOnsetShiftSemitones();
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
		// ML disagreement veto (single notes only). NativeOnly never vetoes. Blend vetoes only
		// when ML is CONFIDENTLY seeing a genuinely different note - never on an octave-equivalent
		// read (the model's known low-note octave ambiguity, e.g. calling an open low E an A-string
		// E an octave up, which was blocking a correct native open E), and never on Pending (ML
		// latency must not stall a correct native accept). MlOnly is the strict A/B mode: any
		// conflict or a not-yet-ready read blocks. The shadow sampler still records disagreements.
		const DetectionStrategy strat = CurrentTechniqueStrategy();
		if (strat != DetectionStrategy::NativeOnly
			&& !isBendTarget && !isLegatoTarget && selectedChordId == -1 && !isConfirmingLegatoRun)
		{
			ResearchProtocol::MlNoteEvidence evidence;
			if (ResearchProbeRuntime::QueryMlNoteEvidence(expectedMidi, 0.5f,
				mlConfirmationState.GetMinimumSampleIndex(), evidence))
			{
				bool veto = false;
				const char* reason = "";
				if (evidence.verdict == ResearchProtocol::MlNoteVerdict::Conflicting)
				{
					const bool octaveEquivalent = evidence.observedMidi >= 0
						&& evidence.observedMidi != expectedMidi
						&& ((evidence.observedMidi - expectedMidi) % 12 == 0);
					if (!octaveEquivalent)
					{
						if (strat == DetectionStrategy::MlOnly)
						{
							veto = true; reason = "conflicting-pitch(ml-only)";
						}
						else if (evidence.confidence >= ML_BLEND_VETO_CONF)
						{
							veto = true; reason = "conflicting-pitch(confident)";
						}
					}
				}
				else if (evidence.verdict == ResearchProtocol::MlNoteVerdict::Pending
					&& strat == DetectionStrategy::MlOnly)
				{
					veto = true; reason = "awaiting-current-target-audio(ml-only)";
				}
				if (veto)
				{
					static ULONGLONG lastConflictLogTick = 0;
					const ULONGLONG nowTick = GetTickCount64();
					if (nowTick - lastConflictLogTick >= 250)
					{
						lastConflictLogTick = nowTick;
						LOG_INFO("(NBN ML GATE) Plain-note acceptance held: expected="
							<< expectedMidi << " observed=" << evidence.observedMidi
							<< " confidence=" << evidence.confidence << " ageMs="
							<< evidence.ageSeconds * 1000.0 << " audioSample="
							<< evidence.analyzedSampleIndex << " minimumSample="
							<< mlConfirmationState.GetMinimumSampleIndex()
							<< " reason=" << reason << std::endl);
					}
					return false;
				}
			}
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

	// The arming's edge-drain (both prime sites) exists to discard a still-ringing
	// previous note, which re-reports its own pitch once the dedupe clears. But the
	// same drain silently ate a played-ahead pick: a player who knows the riff picks
	// the next note during the commit/rebuild dead window, the edge latches it, and
	// priming threw it away (Philip, 2026-08-18: "if the user knows the riff and
	// happens to play the right note just before the next note appears, shouldn't
	// we allow it?"). So a primed onset is kept for the first holding tick when it
	// is distinguishable from the previous note — the same rule as the release
	// wait's fresh-playing shortcut — and matches the new target.
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


	// Builds the target's legato gesture: the contiguous run of hammer-on / pull-off
	// notes that immediately follows it on the same string.
	//
	// These notes are deliberately not targets (the controller skips them so a legato
	// run is one stop rather than several), but they are played as part of the target's
	// gesture and have to stay on the highway. The renderer cannot work this out for
	// itself because it sees one note per call with no ordering, so the run is computed
	// here, where the whole note vector is available in time order, and published.
	//
	// Chart order matters: sparing every legato note in the song, which an earlier
	// attempt did, drew unrelated notes next to the target and implied gestures that do
	// not exist (Philip saw an apparent 12-to-15 hammer-on that the chart never asks
	// for). Only the contiguous same-string run qualifies.
	void BuildVisualGroup(void* owner, uintptr_t groupRecord, float groupTime, int groupString)
	{
		// Note-by-note (Philip, 2026-08-18): the group is always published empty. Every
		// legato note is its own target now, so no note needs group-exemption from the
		// presentation gates - the target itself is the whole whitelist. The protocol
		// fields stay for a possible future "flow mode" that lights whole gestures.
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
		mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
	}

	void BuildLegatoRun(void* owner, float runTime, int runString)
	{
		// Note-by-note (Philip, 2026-08-18): continuations are no longer collected -
		// every hammer-on / pull-off is its own target now, so there is no run to hold
		// as a unit. This is now purely the reset of the run machinery, which BENDS
		// still use: BeginBendConfirmation inserts the bend as element [0] of an empty
		// run and confirms it from the continuous pitch tracker, unchanged.
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
		// Kept so the input-release wait can tell a genuinely carried onset (which
		// necessarily has the previous note's pitch) from fresh playing.
		// What is still ringing, which for a bend is the bent pitch rather than the fretted
		// one. The carry-over guard compares an incoming onset against this to tell a
		// still-sounding previous note from a fresh pick, and recording the unbent pitch
		// made it compare against a note that is no longer sounding. Philip bent green 15
		// from 74 to 76, and purple 12 is also 76, so the ringing bend was accepted as a
		// fresh pick of the next note and consumed it. The log named it exactly: "Accepted
		// onset 76 ... it matches the successor and not the previous note (74)".
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
		// A dense successor is a NEW target and must require its OWN fresh attack, exactly
		// as EstablishHold does - otherwise the previous strum's spike/onset evidence and
		// the still-ringing prior chord carry across the whole chain, so one pick (even a
		// single string) commits an entire repeat-strum section at once (Philip, 2026-08-29;
		// this is the very "spike opened the window, ring matched, accepted without a fresh
		// pick" leak the EstablishHold reset above was written to close). Reset the same
		// fresh-attack state here so each strum in the chain needs its own re-pick.
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
		// Flow-through grace on the DENSE path too (2026-09-01 second pass: the
		// EstablishHold grace measured dormant because flowing play advances through
		// here instead). The reset above stays authoritative for the repeat-strum leak
		// it was written for: a same-pitch successor is still barred from the reattack
		// accept by allowAccept, and a different-pitch successor only accepts on the
		// full tier-0 positive confirmation of ITS OWN pitch, which the previous
		// strum's ring cannot produce. So a recent attack spike may re-open the
		// window for a plain different-pitch single note: the pick that caused it was
		// played through the transition and its sustain deserves to be judged. The
		// window does not tick down until the input-release wait starts consuming it.
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
		// Flow-through grace, re-seeded AFTER the reset above (2026-09-01 second
		// pass: the AdoptDenseSuccessor seeding measured dead because THIS reset
		// wiped it before the input-release wait ever ticked the window). The
		// same-pitch auto-advance the reset protects against stays closed: the wait
		// calls the reattack with allowAccept barred for a successor whose pitch the
		// previous note could satisfy, and a different-pitch successor only accepts
		// on an exact detector match or the full tier-0 positive confirmation of its
		// OWN pitch - a decaying predecessor ring can produce neither. Only a plain
		// different-pitch single note qualifies.
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
		// Every relatch demands a FRESH strum (Philip, 2026-08-25: "repeated
		// strummed chords would automatically progress if the previous chord rang
		// out. Each strum should be forced... no accidental progression due to note
		// sustain."). An earlier version carried the spike evidence across the
		// relatch so a commit-strum spike near the rebuild would count; that is
		// exactly what let a repeated same-chord auto-advance on the previous
		// chord's ring. Both evidence sources reset here, so a same-chord successor
		// cannot commit until a new attack frame lands after this relatch. The
		// dense chain's whole point (avoiding a full release/rebuild) is unaffected;
		// only the "is there fresh attack" question is re-asked, and a real re-strum
		// answers it within ~100 ms (the onset scan runs every tick against a
		// ~95 frame/s ring).
		sawSpikeDuringHold = false;
		sawLevelSpikeDuringHold = false;
		lastStuckWarnHeldSeconds = 0.0;
		ResetChordOnsetEvidenceAnchor();
		SetNativeFreezeFlag(owner, true);
		// Each note in a dense chain gets its own safety budget.
		//
		// Without this the anchor stayed where the *first* hold of the chain set it, so every
		// successor inherited however much of the 60 seconds its predecessors had already
		// spent. A chain whose earlier notes were slow left the later ones with none: the
		// 2026-08-17 capture caught a successor arming and being safety-released on the very
		// next diagnostic sample, reporting 61.8s of "no progress" for a hold that had existed
		// for about four seconds, with no onset ever polled against it.
		//
		// That also means earlier stalls read from this log are not all equivalent. One with
		// a full run of (NBN LAS DETECT) samples really did sit unsatisfied for its whole
		// budget; one that releases within a sample or two of arming never got a budget at all.
		holdProgressAnchor = std::chrono::steady_clock::now();
		mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
		hasHoldProgressAnchor = true;
		lastStuckWarnHeldSeconds = 0.0;
		int primedOnset = QueryNativeOnsetNote();
		int currentMidi = QueryNativeLoudestPlayedNote();
		// The successor is now the held note, so its own legato run is what the transport
		// must wait for.
		BuildLegatoRun(owner, selectedRecordTime, selectedString);
		// A bend child skips the input-release wait entirely.
		//
		// That wait exists to stop the previous note, still ringing, from satisfying the
		// successor as though it were fresh playing. For a bend child the still-ringing sound
		// *is* the correct satisfaction: the player is holding one gesture across the parent
		// and the child, and the child's requirement is that the string be at the bend pitch,
		// which it already is.
		//
		// Waiting here is what made the fix incomplete when it was first written. The wait
		// only clears once the string falls silent, so by the time the child armed the bend was
		// over and the tracker could never read the bend pitch again. The 2026-08-17 capture
		// shows precisely that: pitchNow=76 while waiting, then the note dies, then the child
		// arms with nothing left to observe.
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
			ResearchProtocol::ExpectedAttackEventKind::CandidateChanged);
		EmitCandidateEvent(owner, heldEpoch, selected,
			ResearchProtocol::ExpectedAttackEventKind::HoldEstablished);
		denseSuccessor = {};
		return true;
	}

	void CompleteHeldCommit(void* owner, float updateTime, const LiveNote& selected, const char* reason)
	{
		consumedRecords.insert(selectedRecord);
		// Note-by-note (2026-08-18): only the committed record is consumed. The old
		// group-consumption existed because run continuations were played under this
		// commit without ever being targets; now each continuation IS a target and must
		// stay selectable, so consuming anything beyond the selected record would skip
		// real notes. (The visual group is always empty now anyway.)
		// This note is now the one that is ringing, whichever path the commit took. The
		// dense-successor path already recorded it when adopting a successor, but an
		// ordinary commit did not, so a legato continuation that arrived through a fresh
		// selection lost its reference string and became a target of its own.
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
					// Chord records do NOT share the single-note state-byte semantics:
					// the first chord-hold experiment (2026-08-24) read stateC3 nonzero
					// three seconds BEFORE the chord's own time, so for chords this byte
					// is not the expiry marker and faulting on it killed every chord
					// hold at selection. Until the chord state packing is decoded, log
					// it and let the hold proceed; the safety timeout and the kill
					// switch bound the damage if the record really is dead.
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
						// A single-note target whose native record expired before its hold
						// boundary. In a dense run the release -> input-release-wait -> rebuild
						// latency can let the running transport overshoot a tightly-packed
						// successor, so the game marks it expired (stateC3) before EstablishHold
						// could freeze on it. This is NOT a controller-ownership fault - no hold
						// is owned yet in Armed, so there is nothing native to unwind - and
						// faulting here disabled the whole feature on a single missed successor
						// (Philip: "it fails to attach to the next note and then never stops").
						// Accept the game's own miss verdict, consume the record and advance;
						// the next tick selects the next live note and NBN stays alive. The
						// overshoot is logged so its frequency stays visible if the advance
						// latency later needs shortening so the note is caught, not skipped.
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
					// Repeat chord strums (#54). The play-through stopgap is retired by
					// default: skipping the strums counted them as missed, and two
					// play-through records in a row let the running transport overshoot
					// the next chord's hold boundary, which is what ended the 2026-08-24
					// beta. The original chronic-stall observation predates the spike
					// gate; if bare-0x2 records stall again, the chord safety budget
					// frees them in CHORD_HOLD_SAFETY_RELEASE_SECONDS and
					// repeat-holds-off restores the old play-through classification.
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
				// Chord records do not share single-note state-byte semantics: stateC3
				// reads nonzero seconds before a chord's own time (2026-08-24 chord
				// session), so for a chord successor only C0/C1 gate here - the same
				// relaxation EstablishHold's expiry check uses.
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
					// A genuinely carried onset is the previous note still ringing, so it
					// carries the previous note's pitch. An onset matching the successor
					// therefore cannot be carry-over: it is the player having already moved
					// on. On guitar that is ordinary technique, because the next note is
					// routinely played while the previous string is still sounding, and
					// discarding it rejected correct playing and forced a second attempt.
					//
					// The wait still applies when the two pitches are equal, which is the
					// only case where the detector genuinely cannot distinguish them.
					// A bend just played can still be sounding anywhere between its own
					// pitch and the top of its sweep, so an onset in that range may be
					// the previous note rather than fresh playing. Philip hit exactly
					// this: orange 14 is MIDI 69, a whole-step bend on it produces 71,
					// and green 12 expects 71, so the ringing bend satisfied the next
					// target instantly. In that range the shortcut is unsafe and the
					// normal input-release confirmation has to do the work.
					// previousExpectedMidi is bend-adjusted (the BENT pitch), and a
					// released bend sweeps DOWN toward its base; it cannot ring above
					// the bent pitch. The old upward range double-counted the bend and
					// swallowed a genuine next note just above it (the 124.58s trace:
					// two picks of 74 discarded as carry-over of a bend already at 71,
					// which is exactly the "strummed twice" feel).
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

				// A bend TARGET confirmed by its raw pitch (#68). When the target itself is a
				// bend, a pick-and-bend played straight after the previous note has its onset
				// eaten as carry-over (the string was already sounding and only the fretted
				// pitch climbs), so none of the onset shortcuts above fire; and the silence wait
				// below never completes because the bent note keeps sounding, resetting the count
				// every tick. The bent pitch is evidence in itself: a fresh bend RISES from the
				// base toward the bent target, which a decaying previous ring cannot do, and the
				// bent pitch is distinguishable from the previous note. Keep the same-pitch bar
				// (never accept a pitch the previous note could be producing). Without this the
				// hold armed only after the bend decayed to silence ~3s later, so Philip had to
				// bend the note twice.
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
						// Require a brief hold at target even when rising, so a single momentary
						// at-target read on a partial bend cannot commit (Philip: bends activating
						// without the full bend). A genuine bend climbing into target holds there.
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

				// A held bend's sustained ring can MASK the follower's onset entirely
				// (QueryNativeOnsetNote returns -1 while the loud ring dominates), so the onset
				// shortcuts above never fire AND the silence wait below never completes -
				// currentMidi stays non-silent on the ring, resetting the count every tick - so
				// the follower's picks are refused until the ring decays (Philip: "after a held
				// bend the following note needed multiple hits"). A level spike is the fresh pick
				// regardless of the masking: a decaying ring cannot spike the meter. Arm on it so
				// the pick registers now; the Holding phase then confirms it by pitch as usual,
				// and a spike with no real follow-through simply fails that pitch check.
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
				// Stale-overlay repair (session 7, 2026-08-24 late): the target text is
				// built from the note object's chord template AT hold establishment,
				// but the noteway refreshes recycled objects asynchronously - session 7
				// caught the overlay showing the PREVIOUS chord (G5) for a hold whose
				// evaluator read the fresh template (C5), and Philip faithfully played
				// the wrong chord. Re-emitting the candidate event once a second lets
				// the host rebuild the text from the settled object.
				if (selectedChordId != -1 && holdTickCount != 0 && holdTickCount % 60 == 0)
				{
					EmitCandidateEvent(owner, updateTime, selected,
						ResearchProtocol::ExpectedAttackEventKind::CandidateChanged);
				}
				if (selected.stateC0 != 0 || selected.stateC1 != 0)
				{
					PerformOwnedRelease(owner,
						"the held record committed while its native hit decision was blocked");
					FaultWithoutRelease("The held record committed before an exact onset was accepted");
					return;
				}
				// Safety release, DEFAULT OFF (Philip, 2026-08-25): "remove this auto
				// unlock after 15 seconds and let it just get hung on the note. It's
				// causing false positives and causing more problems than it fixes."
				// From the player's seat a release is indistinguishable from an accept
				// (#58), so every release both misled and buried the refusing state we
				// need on record. Disabled, a stuck hold stays stuck and visible; the
				// escape hatches are the N toggle (Stop releases an owned hold) and the
				// pause/rollback paths. Re-enable live with safety-release-on.
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
					if (!isConfirmingLegatoRun && !isBendTarget && !isLegatoTarget)
					{
						// The native scan proposes a picked-note onset. IsAcceptedOnset applies
						// the shared ML disagreement veto before this proposal can commit.
						if (hasHoldLatchRingTime
							&& NativeSingleNoteHitSinceLatch(expectedMidi, holdLatchRingTime)
							// Strict MlOnly test mode promotes ML to a required co-sign; Blend and
							// NativeOnly let native's own accept stand (Blend adds ML as
							// rescue + guarded veto, not a required co-sign).
							&& (!NativeAcceptNeedsMlCosign() || MlConfirmsHeldNote()))
						{
							onset = expectedMidi;
						}
						else if (isMlStuckRescueEnabled && MlMayRescueNow() && MlConfirmsHeldNote())
						{
							onset = expectedMidi;
							LOG_INFO("(NBN LAS ML RESCUE) Picked-note hold accepted: two distinct"
								<< " post-target audio predictions confirm expected "
								<< expectedMidi << "." << std::endl);
						}
						else if (g_tier0Enforcement
							&& CurrentTechniqueStrategy() != DetectionStrategy::MlOnly)
						{
							// Tier-0 high-fret vouch: native's quality gate refused a note whose
							// fundamental the DSP still reads cleanly (the 35-50 band, e.g. B12).
							// Reliable where the ML rescue flickers. Only when native scored in the
							// band on THIS hold (not a dead signal), and streak-gated for wobble.
							DetectorGateSample vouchSample;
							const bool inVouchBand = TryReadDetectorGates(vouchSample)
								&& !vouchSample.passesQuality
								&& std::isfinite(vouchSample.quality)
								&& vouchSample.quality >= TIER0_VOUCH_QUALITY_FLOOR;
							if (inVouchBand && Tier0ConfirmsExpected(expectedMidi))
							{
								if (++tier0VouchStreak >= TIER0_VOUCH_STREAK_TICKS)
								{
									tier0VouchStreak = 0;
									onset = expectedMidi;
									LOG_INFO("(NBN LAS TIER0 VOUCH) High-fret hold accepted: native"
										<< " quality " << std::fixed << std::setprecision(0)
										<< vouchSample.quality << " under its gate, but tier-0 confirms"
										<< " the fundamental at expected " << expectedMidi << " for "
										<< TIER0_VOUCH_STREAK_TICKS << " ticks (DSP is reliable where"
										<< " the ML rescue flickers on high notes)." << std::endl);
								}
							}
							else
							{
								tier0VouchStreak = 0;
							}
						}
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
						// BEND TARGET rescue (2026-09-03): the detector read wobbles on a high bend
						// (measured on one bend: 76, 87, 77) so the departure-then-lock can miss it,
						// but we KNOW the exact bend target (bendAcceptMidi). Confirm the pitch
						// actually reached it - by tier-0 raw energy AND/OR the ML companion, both
						// DI-robust where the detector is noisy. Additive: only when native's onset
						// paths gave nothing (onset still -1); streak-gated so a passing glide-through
						// cannot trip it, only a held-at-target bend. Better-than-base-game bends come
						// from using the chart's known target instead of a blind continuous read.
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
						// Near-miss forensics (2026-08-25, the B-string +1 sessions: picks
						// at fret 12/15 read 72-for-71 and 75-for-74 while the open string
						// tuned clean). The frame's per-pitch energy bins tell sharp
						// fretting from a genuinely wrong fret: a sharp-fretted note splits
						// energy between the expected bin and its neighbor, a wrong fret
						// concentrates in the neighbor alone. This is the measurement a
						// "sharp-fretting grace" accept rule would be built on; log it for
						// every near-miss onset.
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
						// A bend's pick lands in the band base..bentPitch and the bent pitch
						// itself NEVER arrives as an onset (you bend up to it, no fresh pick),
						// so IsAcceptedOnset - which for a known-amount bend demands the exact
						// bent pitch - can never admit the pick that STARTS the gesture, and the
						// confirmation phase below (with all the half-bend / #52 logic) never
						// begins. The note hangs (Philip's 69->71 bend, pick read 70, stuck the
						// whole 150s capture). An in-band onset is exactly what MatchesPickPitch
						// already defines as "the player picked this bend" (see its comment), so
						// admit it here to start BeginBendConfirmation. The bent-pitch requirement
						// is unchanged - it is enforced at confirmation, not at this entry gate -
						// and the #52 decaying-ring guard above still zeroes a ringing false onset
						// before it reaches this point.
						if (IsAcceptedOnset(onset) || (isBendTarget && MatchesPickPitch(onset)))
						{
							// A bend is the same shape as a legato run: the pick starts the
							// gesture and the pitch completes it. Bending produces no new
							// pick attack, so the bent pitch can never arrive as an onset
							// and only the current-pitch query observes it. Accepting the
							// pick alone is what let a bend complete before it was bent,
							// which is what Philip saw as bends stopping mid-bend.
							//
							// EVERY accepted onset on a bend target starts confirmation, not
							// only an exact base-pitch match (ticket #52 second half,
							// 2026-08-22 beta): the bend acceptance band spans base..base+3,
							// but the old equality test meant an in-band onset ABOVE base
							// fell through to the direct commit - no gesture required. The
							// note before the orange 3:14 bend in Philip's riff is green
							// 4:12, pitch 71, inside that band, so its ring or re-attack
							// committed the bend in 2 ticks. Routing the whole band here
							// costs nothing for a genuinely pre-bent pick (the tracker
							// confirms immediately) and makes the gesture mandatory for
							// everything else.
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
						// A bend CHILD is the parent bend's SUSTAIN: the player holds ONE gesture
						// across the parent and the child, so the string is ALREADY at the bent
						// pitch with no fresh below-target approach. The base game treats the bend
						// and its sustain as a single action and never re-demands the sustain; we
						// were splitting it in two (Philip: "does the note, then asks for another to
						// do the sustain"). The approach guard (hasBendApproachBeenObserved) exists
						// to stop a stray DECAYING ring from a different note confirming a bend that
						// was never bent - but this ring IS the held bend, the same gesture, so the
						// guard must not apply. Pre-satisfy the approach for a bend child so the
						// held bent pitch confirms the sustain immediately (via the native sounding
						// table, which lists the bent pitch while it sounds). A released child is
						// still safe: its pitch falls below target, so nothing confirms.
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
											// Only a genuine below-target read (a fresh approach / a real
											// re-bend) restarts the count; a non-below dip is the
											// top-of-bend wobble and holds the streak.
											// Reset the streak only on a GENUINE release toward the unbent base, not on the
											// integer detector flickering to target-1 at the TOP of the bend. The on-screen
											// meter reads the fractional pitch and stays green there, but the integer field
											// briefly quantizes one semitone low; the old reset (< target - 0.5) zeroed the
											// streak on every such frame, so a reached bend never accumulated its ticks and the
											// player had to re-pick at the peak (Philip 2026-09-02: green on the tuner, no
											// accept). A drop back to within half a semitone of the unbent base IS a release; a
											// one-integer top wobble is not. Only widened for bends deeper than a semitone,
											// where base+0.5 sits below target-0.5; a 1-semitone bend keeps the old threshold.
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
						// Fractional veto (2026-09-05): the integer detector rounds a bend that has NOT
						// reached pitch up to the target, and the strong-green / streak / sounding-table
						// paths then accept it on a single frame - the bend "activates before it is
						// reached" (Philip 2026-09-05). The raw-tap fractional estimate knows the true
						// pitch; when it is confident and clearly below the target band, veto the reach.
						// It releases the instant the pitch climbs into the band, and never fires when the
						// estimate is unavailable or low-confidence, so it cannot make a bend unplayable.
						// Bend elements only; a fretted legato note is judged exactly by the polls below.
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
						// A bend counts the moment it reaches pitch and is not required to stay
						// there. Requiring two consecutive polls meant a bend that touched its
						// target and wobbled below reset the counter, so it could fail half way
						// through and then need re-bending, which Philip found unreliable. A
						// fretted legato note still needs the two polls, because its pitch is
						// stable once fretted and a single poll is more prone to noise.
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
								mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
								if (legatoRunIndex >= legatoRunCount)
								{
									isConfirmingLegatoRun = false;
									gatePhase = GatePhase::CommitBeforeRelease;
									commitTickCount = 0;
									wasCommitOverrideLogged = false;
									LOG_INFO("(NBN LAS LEGATO) Run complete; all " << legatoRunCount
										<< " continuation(s) played. The gesture commits as one note."
										<< std::endl);
									// Consume the committed record so the eligibility scan does not re-select
									// it. A bend note's window extends through its SUSTAIN, so without this the
									// just-bent record stays eligible and is re-SELECTED the instant it commits,
									// demanding a second bend (Philip: "hit the pitch, it moves slightly to the
									// sustain, then requires a new bend"). Chords already consume on commit; runs
									// (bend or legato) need it too. lastCommittedChordRecord carries it across the
									// release's PlayerSong restart (which clears consumedRecords), same as chords.
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

					// Legato fallback. A hammer-on or pull-off is fretted onto a string
					// that is already sounding, so it produces no pick attack and the
					// edge-detected onset query often never reports it at all. The
					// current-pitch query does observe it. This is only consulted for
					// records the chart itself flags as hammer-on or pull-off, and only
					// when the successor's pitch differs from the note just played, so a
					// still-ringing previous note cannot satisfy it.
					//
					// The pitch must ARRIVE, not merely be present (2026-08-18): a bend
					// release sweeps the string down through real pitches while it is
					// still ringing loudly, and a bare equality poll accepted those as
					// played notes - Philip's orange-14 half bend cascaded commits
					// through the following targets on its release. So acceptance arms
					// only once the detector has reported something OTHER than the
					// expected pitch since this target became active: the note counts
					// when its pitch appears, not when it is inherited.
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

		bool isEnabled = ResearchProbeRuntime::IsNoteByNoteEnabled();

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

		// A pending enable-time re-arm. The enable event fires host-side, off this loop,
		// so a section change that keeps the SAME owner is invisible to the owner-identity
		// reset below and would otherwise leave stale confirmed bounds from the previous
		// section (Philip's Riff-Repeater repro: enable in one section, cycle to another,
		// re-enable, and it never finds the epoch). Reset here for the current owner so the
		// bootstrap proceeds fresh; adopting the owner skips the redundant owner-change
		// reset immediately below.
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

		// Song-reload guard. The owner+0x78 grid is repopulated when a new song loads, and it
		// can settle AFTER NBN has already confirmed against the previous song's grid (the
		// owner-identity reset above misses it because at re-arm time the grid still held the
		// old container). If the live grid identity no longer matches the cached timeline, the
		// song changed - re-arm so the new song's sections are latched and no stale/out-of-range
		// section carries over (Philip: the timeline controller must live per song lifecycle).
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

		// PRIMARY arming path (2026-08-28): latch the loop bounds directly from the stable
		// authored phrase-section grid, immediately, on the first valid tick after enable.
		// This replaces the delayed-identity rollback bootstrap for the normal case - no
		// waiting for a loop turnover, no dependence on the churning +0x3C0/+0x3C8 mirror
		// that hung NBN on "Waiting for Rocksmith's native section epoch". On a transient
		// read it leaves isEpochConfirmed false and we retry next tick. The delayed-identity
		// path below still runs ONLY when this has not confirmed, as a fallback for an owner
		// whose grid is somehow unreadable - so a weird owner degrades to the old behavior
		// instead of failing outright. See docs/investigations/note-by-note-bug-tracker.md.
		if (!isEpochConfirmed)
		{
			TryLatchSectionFromGrid(owner);
		}
		else if (HasSelectionMovedToNewSection(owner))
		{
			// The player navigated to a different Riff Repeater section while NBN stayed on.
			// Auto-follow: abandon any hold from the old section (ResetBootstrap clears the
			// freeze flag safely) and re-latch to the new selection immediately - no N toggle,
			// and NBN never turns off. ResetBootstrap does not touch the cached timeline, so
			// the re-latch reuses the grid.
			// Preserve the current start as a hint: extending the range rightward moves only
			// the end, and +0x3C0 stays marched, so without this the re-latch would collapse
			// the loop to its last phrase (Philip: "extending the range broke it").
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
				// Beta autopsy (2026-08-24): the game restarts its own section (failure /
				// manual / natural loop) and faulting here disabled the feature, which read
				// as Note by Note turning itself off. The hold is already moot (the game
				// initiated the rollback and is running again), so abandon it and
				// re-bootstrap in place, staying enabled; delayed-identity re-confirms.
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
				// Armed: a genuine loop turnover. Advance the epoch and free the section's
				// notes for the next pass. Neither boundary is touched - they were latched
				// once at confirmation and are immutable for the life of the section.
				++epochIndex;
				ClearSelection();
				consumedRecords.clear();
				// Clear the pitch dedupe global too. A bend seeds it with the BENT pitch on
				// commit (so the sustained ring cannot auto-commit a follower); without this
				// reset that seed survives the loop, and the same bend the next pass reads its
				// own ring as already-consumed and fails until a spike unsticks it (Philip:
				// "failed again on the bend after the loop"). A fresh loop iteration must start
				// with no carried-over consume state.
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
				// Flow phase 0 (2026-09-04): sample the detector gates while ARMED too, so the
				// attack-spike stamp (g_lastAttackSpikeTick) sees picks played while the
				// transport is running. Until now it was stamped only during hold-phase
				// ticks, which left the hold-establishment grace blind to exactly the pick
				// that flow-until-miss must measure ("hold established N ms after the last
				// attack spike"). The read is a handful of SEH-guarded peeks per tick and
				// calls no native query; the Holding-only branches inside stay inert here.
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
						std::snprintf(seam, sizeof(seam), "early-tick-%d", holdTickCount);
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

		if (ResearchProbeRuntime::IsNoteByNoteEnabled() && isEpochConfirmed)
		{
			HandleAfterUpdate(owner, updateTime);
		}
	}

	// #53: evaluates the held chord's own hit decision with the detection window
	// slid onto the detector clock when the clock has left the authored window.
	// The slide translates, never widens - the same width the chart authored,
	// centered on the clock, so the onset scan looks at "was this chord strummed
	// just now" instead of a span pinned to a time the transport froze at. The
	// authored deltas are restored before returning, whatever the writes did, so a
	// rebuild can never inherit a slid window. When everything reads clean and the
	// clock is still in-window, the original runs untouched: a refusal logged
	// in-window would falsify the clock hypothesis, which is worth as much as the
	// fix working.
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
		// NEGATIVE window, not a re-centered one (session 8, 2026-08-24 night).
		// The re-centered slide still evaluated only the CURRENT analysis frame
		// (0x4E6A70's positive-window path calls the matcher with frame index 0),
		// and post-attack frames lose a chord's high tones to masking: a correct
		// C5 refused 571 evaluations with tones 55 and 60 blazing in the frame
		// and 67 never detected. 0x4E6A70's OTHER path - taken when windowStart
		// is negative - scans every analysis frame of the last 0.2s
		// (_DAT_0119AD60) and accepts if ANY matches, which includes the attack
		// frame where the full chord's tones exist. That is how chords pass in
		// normal play; a negative start delta routes the frozen evaluation onto
		// the same attack-inclusive path.
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
			|| !ResearchProbeRuntime::IsNoteByNoteEnabled())
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
					// OBSERVE-FIRST (2026-08-29, native harmonic matcher, issue #58): call
					// the spectral matcher 0x4E6E90 and LOG its verdict next to the existing
					// decision, WITHOUT acting on it yet. This proves the native call is safe
					// (no crash) and shows whether the matcher discriminates a full strum
					// (YES) from a partial (no) where the sounding table cannot - before it is
					// wired into acceptance. Verbose-gated and throttled so it never floods.
					// Native harmonic matcher (issue #58, validated 2026-08-29): the game's
					// own spectral matcher 0x4E6E90 is the reliable chord discriminator under
					// the freeze - it reads the raw analysis spectrum, so it is immune to the
					// #53 sounding-table masking, and has no clock/window gate, so the frozen
					// transport does not desync it. Called EVERY tick (the YES peak lasts only
					// a few frames). verdict: 1 YES / 0 ran-and-rejected / -2 input too quiet /
					// -1 could not run (det unresolved or ND tones not set for this record).
					int matcherTones[6] = { 0, 0, 0, 0, 0, 0 };
					int matcherToneCount = 0;
					{
						// The authored template tones are in the guitar frame; the spectrum the matcher
						// compares against is the detected frame, so add the input shift (0 for
						// Speaker/Off, the Drop amount for Drop Pedal) - see the single-note hold.
						const int matcherInputShift = ResearchProbeRuntime::GetInputOnsetShiftSemitones();
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

						// Publish the matcher's tones for the research state (fake-guitar
						// harness), tagged with this record so a poll never sees a stale set.
						if (matcherToneCount > 0)
						{
							researchChordToneCount = matcherToneCount;
							for (int i = 0; i < matcherToneCount; ++i)
								researchChordTones[i] = matcherTones[i];
							researchChordTonesRecord = selectedRecord;
						}
					}
					// NATIVE ONSET-WINDOW HIT (port, 2026-08-29): run the game's own onset gate
					// (FUN_004E4B60) + harmonic scan (FUN_004E6A70 scan-mode) over the ring since this
					// note latched. True only when a fresh ATTACK and a full-chord harmonic MATCH both
					// occurred since the latch stamp - the native "one re-pick per note", freeze-proof
					// (frame timestamps keep advancing while frozen). Authority whenever it can run
					// (latch stamp + tones); the legacy paths below are only a fallback for when it cannot.
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
						mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
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
						mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
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
						// Consumption symmetry (the #52 rule, chord form - live leak
						// 2026-08-25: a blue-string single inside the chord's tones
						// committed 2 ticks after latch off the strum's unconsumed
						// edge). A chord never reads the onset edge, so consume it
						// here the way every picked commit does: one query call,
						// which itself writes the dedupe global.
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
						mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
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
						mlConfirmationState.Reset(ResearchProbeRuntime::GetMlAudioSampleIndex());
						// The post-release rebuild resets the chord's native states, and
						// unlike singles the chord has no recommit repair, so without this
						// the eligibility scan re-selects and re-holds every committed
						// chord - the session log shows each acceptance followed by a
						// re-SELECT of the same record, one of them requiring a second
						// strum of an already-played chord. The set clears on epoch
						// restart, so loop replays still re-target chords correctly.
						consumedRecords.insert(selectedRecord);
						// The set alone is not enough at the SECTION START: this commit's
						// PlayerSong restart reads as a section change, ResetBootstrap
						// clears the set, and the same chord re-selects (2026-08-24 beta,
						// double-strum of the loop's first chord). This pair survives the
						// reset, wall-clock bounded.
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
						// Native-drive step 4 chord shadow (2026-08-25 night): what
						// the ND sounding table holds for each chord tone, next to
						// every throttled refusal. Go/no-go data for chord acceptance
						// from the table that now accepts singles: refused chords
						// showing all playable tones present mean the per-tone check
						// works; missing high tones (the #53 masking) mean chords
						// need the 0x4E6E90 harmonic matcher instead.
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
	ResearchProtocol::ScoringUpdate original)
{
	if (original == nullptr) return;
	originalScoringUpdate = reinterpret_cast<ScoringUpdateFn>(original);
	ScoringUpdateDetour(owner, updateTime);
}

bool NoteByNoteNativeScoring::ProcessHitDecision(
	void* owner,
	void* unusedEdx,
	void* note,
	ResearchProtocol::HitDecision original)
{
	if (original == nullptr) return false;
	originalHitDecision = reinterpret_cast<HitDecisionFn>(original);
	return HitDecisionDetour(owner, unusedEdx, note);
}

ResearchProtocol::NoteByNoteState NoteByNoteNativeScoring::GetResearchState()
{
	std::lock_guard<std::recursive_mutex> lock(controllerMutex);
	ResearchProtocol::NoteByNoteState state;
	state.isInitialized = isInitialized ? 1 : 0;
	state.isEpochConfirmed = isEpochConfirmed ? 1 : 0;
	state.ownsNativeHold = OwnsNativeHold() ? 1 : 0;
	state.gatePhase = static_cast<ResearchProtocol::GatePhase>(gatePhase);
	state.trackedOwner = reinterpret_cast<uintptr_t>(trackedOwner);
	state.selectedRecord = selectedRecord;
	state.epoch = epochIndex;
	state.holdTickCount = holdTickCount;
	state.visualGroupCount = visualGroupCount;
	for (uint32_t i = 0; i < visualGroupCount
		&& i < ResearchProtocol::NoteByNoteState::MaxVisualGroup; ++i)
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

	// The current chord target's tones for the fake-guitar harness, only when the target
	// really is a chord and the published set was read for this exact record (otherwise a
	// previous chord's tones would leak onto a single note or a just-changed target).
	if (selectedChordId >= 0 && researchChordToneCount > 0
		&& researchChordTonesRecord == selectedRecord)
	{
		state.expectedChordToneCount = (std::min)(static_cast<uint32_t>(researchChordToneCount),
			ResearchProtocol::NoteByNoteState::MaxChordTones);
		for (uint32_t i = 0; i < state.expectedChordToneCount; ++i)
			state.expectedChordTones[i] = researchChordTones[i];
	}

	// The target's bend, for the fake-guitar harness to glide up to instead of sounding a
	// static note (available throughout the hold, unlike the live bend-visualizer fields).
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
			ResearchProtocol::NoteByNoteState::CompareHistoryLength);
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

void NoteByNoteNativeScoring::HeapCheckpointForResearch(const char* seam)
{
	HeapCheckpoint(seam);
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
