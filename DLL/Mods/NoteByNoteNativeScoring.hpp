#pragma once

#include "NoteByNoteProbe.hpp"
#include "NoteByNoteProtocol.hpp"

namespace NoteByNoteNativeScoring
{
	void Initialize();
	bool IsAvailable();

	// Generic observation-hook service. The bridge calls SetRequestedGenericHooks after every
	// probe load and unload with the loaded probe's requested addresses (or an empty list). The
	// request is staged and reconciled from a main-thread render seam: new addresses get a
	// pass-through detour, and slots the current probe no longer wants stop being forwarded.
	// Detours are never removed; an unclaimed hook simply runs the original and notifies no one.
	void SetRequestedGenericHooks(const NoteByNoteProtocol::HookRequest* requests, uint32_t count);
	NoteByNoteProtocol::NoteByNoteState GetStateSnapshot();
	void ObserveRenderedAttack(const NoteByNoteProbe::NativeRenderedAttack& attack);
	bool ProcessHitDecision(
		void* owner,
		void* unusedEdx,
		void* note,
		NoteByNoteProtocol::HitDecision original);
	void ProcessScoringUpdate(
		void* owner,
		float updateTime,
		NoteByNoteProtocol::ScoringUpdate original);
	void Shutdown();
	void Stop();
	void RequestReArm();
	bool TryGetStateSnapshot(NoteByNoteProtocol::NoteByNoteState& state);
	// Chord holds (#43): on = held chords release on the game's own hit decision;
	// off = the pre-#43 fallback where chords play through natively.
	void SetChordHoldsEnabled(bool enabled);
	void SetFreezePromptSoundEnabled(bool enabled);
	// Flow-until-miss (docs/designs/nbn-flow-until-miss.md). DEFAULT OFF - parked for the
	// stable release (fragile freeze-core change, belongs with the parked timeline work).
	// On = the hold boundary sits LATE (recordTime + grace) so on-time notes commit
	// naturally and the transport plays straight through - no per-note freeze/restart, so
	// dense sections keep their audio; off = the proven freeze-per-note boundary
	// (recordTime - compensation). SetFlowLateGraceSeconds tunes how late, live.
	void SetFlowUntilMissEnabled(bool enabled);
	bool GetFlowUntilMissEnabled();
	void SetFlowLateGraceSeconds(float seconds);
	float GetFlowLateGraceSeconds();

	void SetHeapCheckEnabled(bool shouldEnable);
	// Cross-thread lattice arm: lets other threads (the render hook) run the same
	// named-seam heap validation, so a wandering corruption gets a thread-labeled
	// bracket. Same enable flag and latch as the scoring-side checkpoints.
	bool GetChordHoldsEnabled();
	void SetVerboseTrace(bool enabled);
	bool GetVerboseTrace();
	enum class DetectionStrategy { Blend = 0, NativeOnly = 1, MlOnly = 2 };
	void SetChordDetectionStrategy(DetectionStrategy strategy);
	void SetNoteDetectionStrategy(DetectionStrategy strategy);
	void SetBendDetectionStrategy(DetectionStrategy strategy);
	// Chord window slide (#53): on = a held chord whose detection window the
	// detector clock has left is evaluated with the window slid onto the clock;
	// off = authored windows only, refusals measured but not repaired.
	void SetChordWindowSlideEnabled(bool enabled);
	bool GetChordWindowSlideEnabled();
	void SetChordTier0RescueEnabled(bool enabled);   // per-tone energy chord rescue (default off; live-tuned)
	bool GetChordTier0RescueEnabled();
	// Repeat-strum holds (#54): on = bare-0x2 repeat records hold like full
	// chords; off = the old play-through stopgap (skipped strums count missed).
	void SetRepeatStrumHoldsEnabled(bool enabled);
	bool GetRepeatStrumHoldsEnabled();
	// Native freeze announcement (#57): on = owned holds write the game's own
	// frozen-on-tag byte at owner+0x5E3, the flag GE_FreezeOnTag sets so native
	// systems enter the freeze presentation. Default off until observed live.
	void SetNativeFreezeFlagEnabled(bool enabled);
	bool GetNativeFreezeFlagEnabled();
	// Native chord panel (#55/#57): on = chord holds call the lesson engine's
	// own GE_ShowChordDisplay implementation with the chord's id, so the game
	// draws its frozen chord panel; hidden again at release.
	void SetNativeChordPanelEnabled(bool enabled);
	bool GetNativeChordPanelEnabled();
	void SetSafetyReleaseEnabled(bool enabled);
	// Native schedule-shift experiment (rewrite-plan ladder step 3, default
	// OFF): on = each owned release calls the game's scheduler service
	// (0x57EB00, entry 4, op add) with the frozen span, replicating the
	// frozen-span destructor's anti-sweep compensation. A/B live with
	// schedule-shift-on|off and watch the fretboard sweep at release.
	void SetScheduleShiftEnabled(bool enabled);
	// Native-drive phase A: request ONE StartAt-core call (the game's own
	// resume + seek-to-now + speed reset, 0x4749C0 with an empty tag) on the
	// next idle scoring tick, main-thread, vtable-guarded. Observe-first: run
	// it while the transport plays normally and watch what a native seek does
	// before wiring it into the release path. Bridge: native-seek-test.
	void RequestNativeSeekTest();
	// Native-drive step 3, hybrid (default OFF): on = owned releases run the
	// StartAt core (the game's own unfreeze + seek + speed reset) followed by
	// the coordinated PlayerSong restart for the music. Pure-native release is
	// blocked on finding the native music restart: two live attempts (StartAt
	// alone, StartAt + 0x4729E0 - which is GE_HideGame's core and faded the
	// game to black) both left the PlayerSong stopped. A/B live with
	// native-release-on|off.
	void SetNativeReleaseEnabled(bool enabled);
	// Native-drive step 4 (default ON): single-note acceptance from the lesson
	// engine's own primitive - the expected pitch present in the detector's
	// current-sounding table (+0x604/+0x6A4) above the native threshold
	// (0x01199DD4) continuously for ~0.18s. Runs ahead of the heuristic
	// recoveries; nd-accept-off is the emergency revert.
	void SetNdAcceptEnabled(bool enabled);
	// Step-5 first flight: arm ONE hold to enter the game's own frozen mode
	// (GE_FreezeSong core 0x474840: owner+0x5E2 + mode call 2,0 - stands up
	// the real frozen-mode object, vtable 0x011A0B70) on top of the
	// mechanical freeze, exiting at release via the UnfreezeSong core
	// 0x474890 (native ResumeFromTag unwind + mode 0,2). Observe-first: the
	// question is what the FREEZE PRESENTATION does (stopped-preview doubles,
	// panels) while mode 2 is active. Bridge: freeze-mode-test.
	void RequestFreezeModeTest();
	// Formats the chord a native note object would be judged as - "G5 [3/5/5/x/x/x]"
	// - from the SNG chord template at note+0x30 (name at +0x28, per-string frets
	// at +0x04, values < 0x1A playable). Returns false when the note carries no
	// readable template. Used by the desync logs and the overlay target line.
	bool TryDescribeChordTarget(uintptr_t noteAddress, char* buffer, size_t bufferLength);
}
