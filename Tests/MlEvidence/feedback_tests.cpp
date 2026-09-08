#include "../../DLL/Mods/DetectionFeedback.hpp"
#include "../../DLL/Mods/PickedAttackQueue.hpp"
#include <cassert>
#include <iostream>

int main()
{
	using namespace NoteByNote;
	assert(FormatPosition(5, 15, 64) == "high E string, fret 15");
	assert(FormatPosition(0, 15, 40) == "low E string, fret 15");
	assert(FormatPosition(-1, 15, 40) == "--");
	assert(FormatPosition(0, 0, 40) == "low E string, open");
	assert(FormatPosition(0, 2, 40) == "low E string, fret 2");
	assert(FormatPitch(80) == "Ab");
	// An Eb song played on an E standard guitar under the Drop Pedal: the HUD must say
	// Eb, the name a guitarist reads off the tuning, not D# (Philip, 2026-09-08).
	assert(FormatPitch(63) == "Eb" && FormatPitch(70) == "Bb");
	assert(FormatPosition(4, 15, 57) == "A string, fret 15");
	assert(FormatPitch(57 + 15) == "C");
	assert(FormatPosition(5, 12, 62) == "high D string, fret 12");
	assert(FormatPosition(0, 0, 38) == "low D string, open");
	assert(FormatPosition(4, 15, 58) == "Bb string, fret 15");
	assert(FormatPitch(58 + 15) == "C#");
	assert(FormatPosition(4, 15, -1) == "--");
	assert(FormatPitch(67) == "G" && FormatPitch(69) == "A");
	assert(FormatPitch(61) == "C#" && FormatPitch(62) == "D");
	assert(FormatPitch(GetStringFretMidi(0, 0)) == FormatPitch(40));
	assert(FormatPitch(GetStringFretMidi(5, 3)) == FormatPitch(67));
	assert(FormatPitch(GetStringFretMidi(4, 8)) == FormatPitch(67));
	assert(FormatPitch(GetStringFretMidi(0, -1)) == "--");
	assert(GetStringFretMidi(-1, 0) == -1);
	assert(MatchesDetectorTarget(76, 76));
	assert(!MatchesDetectorTarget(64, 76)); // Both display E; an octave error is not an exact match.
	assert(!MatchesDetectorTarget(-1, -1));
	assert(!MatchesDetectorTarget(76, 76, 0.49f));
	assert(!MatchesDetectorTarget(76, 76, NAN));
	assert(MatchesDetectorTarget(76, 76, 0.5f));
	for (const auto role : { DetectorRole::Confirmed, DetectorRole::Partial, DetectorRole::Rejected })
	{
		const auto initial = GetDetectorColor(role, 100, 100);
		assert(initial != 0xFFFFFFFF);
		assert(GetDetectorColor(role, 100, 750) == initial);
		const auto halfway = GetDetectorColor(role, 100, 1075);
		assert(halfway != initial && halfway != 0xFFFFFFFF);
		assert(GetDetectorColor(role, 100, 1400) == 0xFFFFFFFF);
		assert(GetDetectorColor(role, 0, 100) == 0xFFFFFFFF);
		assert(GetDetectorColor(role, 100, 99) == 0xFFFFFFFF);
		assert(GetDetectorColor(role, 1400, 1400) == initial);
	}
	assert(GetDetectorColor(DetectorRole::Unused, 100, 100) == 0xFFFFFFFF);
	assert(GetDetectorColor(DetectorRole::Confirmed, 100, 100) == 0xFF55DD77);
	assert(GetDetectorColor(DetectorRole::Partial, 100, 100) == 0xFFFFAA44);
	DetectionFeedback feedback;
	for (bool nativeAgrees : { false, true })
	{
		for (bool mlConfirms : { false, true })
		{
			for (bool mlRequired : { false, true })
			{
				if (mlRequired && !mlConfirms) continue;
				feedback.nativeMidi = nativeAgrees ? 42 : 41;
				feedback.enhancedMidi = 42;
				SetPickedDetectorRoles(feedback, nativeAgrees, mlConfirms);
				assert(feedback.enhancedRole == DetectorRole::Confirmed || feedback.mlRole == DetectorRole::Confirmed);
				assert((feedback.mlRole == DetectorRole::Confirmed) == mlConfirms);
				assert(feedback.nativeRole == (nativeAgrees ? DetectorRole::Confirmed : DetectorRole::Rejected));
				assert(feedback.enhancedRole == DetectorRole::Confirmed);
			}
		}
	}
	PickedAttackQueue queue;
	feedback.nativeMidi = -1;
	SetPickedDetectorRoles(feedback, false, false);
	assert(feedback.nativeRole == DetectorRole::Unused);
	assert(feedback.enhancedRole == DetectorRole::Confirmed);
	DetectionFeedback displayed;
	displayed.tick = 1000;
	displayed.targetRecord = 42;
	displayed.targetMidi = 80;
	displayed.mlMidi = 80;
	displayed.mlRole = DetectorRole::Confirmed;
	DetectionFeedback committed;
	committed.tick = 1016;
	committed.targetRecord = 42;
	committed.targetMidi = 80;
	committed.nativeMidi = 80;
	committed.nativeRole = DetectorRole::Confirmed;
	committed.enhancedRole = DetectorRole::Confirmed;
	committed.enhancedMidi = 80;
	PublishDetectionFeedback(displayed, committed);
	assert(displayed.mlRole == DetectorRole::Confirmed && displayed.mlMidi == 80);
	for (uint64_t elapsed : { 0u, 16u, 649u, 650u, 975u, 1299u, 1300u })
	{
		assert(GetDetectorColor(displayed.nativeRole, displayed.tick, displayed.tick + elapsed)
			== GetDetectorColor(displayed.mlRole, displayed.tick, displayed.tick + elapsed));
		assert(GetDetectorColor(displayed.enhancedRole, displayed.tick, displayed.tick + elapsed)
			== GetDetectorColor(displayed.mlRole, displayed.tick, displayed.tick + elapsed));
	}
	const auto retained = displayed;
	DetectionFeedback agreement = committed;
	agreement.nativeRole = DetectorRole::Unused;
	agreement.enhancedRole = DetectorRole::Partial;
	PublishDetectionFeedback(displayed, agreement);
	assert(displayed.nativeRole == DetectorRole::Confirmed);
	assert(displayed.enhancedRole == DetectorRole::Confirmed);
	agreement.enhancedMidi = 68;
	PublishDetectionFeedback(displayed, agreement);
	assert(displayed.enhancedRole == DetectorRole::Partial);
	displayed = committed;
	assert(!CreditConfirmedMlFeedback(displayed, 200, 200, 400, 1100));
	assert(!CreditConfirmedMlFeedback(displayed, 401, 200, 400, 1100));
	assert(displayed.mlRole == DetectorRole::Unused);
	assert(CreditConfirmedMlFeedback(displayed, 300, 200, 400, 1100));
	assert(displayed.mlRole == DetectorRole::Confirmed && displayed.mlMidi == 80);
	assert(displayed.tick == committed.tick);
	assert(!CreditConfirmedMlFeedback(displayed, 300, 200, 400, committed.tick + 1300));
	displayed = {}; // A fresh pick clears both contributions.
	PublishDetectionFeedback(displayed, committed);
	assert(displayed.mlRole == DetectorRole::Unused);
	displayed = retained;
	committed.targetRecord = 43;
	PublishDetectionFeedback(displayed, committed);
	assert(displayed.mlRole == DetectorRole::Unused);
	displayed = retained;
	committed.targetRecord = 42;
	committed.tick = retained.tick + 1300;
	PublishDetectionFeedback(displayed, committed);
	assert(displayed.mlRole == DetectorRole::Unused);

	PickedAttack attack;
	attack.time = 1;
	attack.confirmedMidi = 80;
	attack.feedback.nativeRole = DetectorRole::Partial;
	attack.feedback.enhancedRole = DetectorRole::Confirmed;
	attack.feedback.enhancedMidi = 80;
	attack.feedback.mlRole = DetectorRole::Confirmed;
	assert(queue.Push(attack));
	PickedAttack consumed;
	assert(queue.Take(80, consumed));
	assert(consumed.feedback.nativeRole == DetectorRole::Partial);
	assert(consumed.feedback.enhancedRole == DetectorRole::Confirmed && consumed.feedback.enhancedMidi == 80);
	assert(consumed.feedback.mlRole == DetectorRole::Confirmed);
	std::cout << "Detection feedback formatting, fade, reset and buffered attribution passed\n";
}
