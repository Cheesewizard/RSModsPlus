#pragma once
#include <cmath>
#include <cstdlib>
#include "NoteTechniqueClassification.hpp"   // CHORD_STRING_SENTINEL

// Below 160 Hz the raw pitch verifier confirms a note on its 2nd and 3rd harmonics only,
// because the rig's magnetic pickup leaves the low-string fundamental ~30 dB down (measured
// Pf/P2f ~ 0.001 on the Little Wing Speaker-mode captures, so useHarmonics deliberately skips
// the fundamental check in RawPitchVerifier::ConfirmSnapshot). The failure that motivates this
// gate: a chord that committed a moment earlier is still ringing an octave above a queued low
// note (Fbsus2's F3 + C4 sit exactly on F2's 2f + 3f). That decay is spectrally identical to a
// fresh pick of the low note, so the raw verifier confirms a phantom the player never played and
// the pick buffer force-commits it. Raw evidence cannot separate the two; only the game's own
// (native) detector can, because it reads the ringing chord's actual tones, not the octave below.
//
// See [[prefer-native-detection]]: native is the arbiter here, not a divergent custom heuristic.
namespace NoteByNote
{
	// Mirrors RawPitchVerifier.cpp:296 (frequencyHz < 160.0): the harmonic-only regime where the
	// fundamental is unavailable and an octave-up chord can masquerade as the note.
	inline bool IsHarmonicOnlyTarget(int midi)
	{
		return midi >= 0 && 440.0 * std::pow(2.0, (midi - 69.0) / 12.0) < 160.0;
	}

	// A ringer BELOW the target by 12/19/24 puts its 2f/3f/4f on the target's fundamental band,
	// which the rig's ~-30 dB fundamentals stop lowerFundamental from rejecting (2026-09-22
	// 1422.32: a ringing E2 confirmed E3, targetRise 0.0007, native 2 octaves off). A ringer
	// ABOVE by the same intervals masks only a harmonic-only target, whose 2f/3f are the very
	// bands the verifier measures.
	inline bool RingingNoteMasksTarget(int midi, int ringerMidi)
	{
		if (midi < 0 || ringerMidi < 0) return false;
		const int below = midi - ringerMidi;
		const int above = ringerMidi - midi;
		if (below == 12 || below == 19 || below == 24) return true;
		return IsHarmonicOnlyTarget(midi) && (above == 12 || above == 19 || above == 24);
	}

	// Any still-ringing tone (a chord's authored tones) that masks the target.
	inline bool RingingTonesMaskTarget(int midi, const int* tones, int count)
	{
		for (int i = 0; tones != nullptr && i < count; ++i)
			if (RingingNoteMasksTarget(midi, tones[i])) return true;
		return false;
	}

	// Philip's rule: a chord followed by a plain single note always needs its own attack. The raw
	// stream cannot tell that pick from the chord strum's tail or a sloppy strum ringing the next
	// note (2026-09-22 893.23), so native must hear the note. Legato successors (hammer/pull/tap)
	// are exempt - they continue a ringing string with no pick.
	inline bool ChordToSingleBoundaryRequiresNative(int previousString, bool isLegatoTarget)
	{
		return previousString == CHORD_STRING_SENTINEL && !isLegatoTarget;
	}

	// The native detector reads one semitone off expectedMidi in Speaker Mode (the E/Eb chart-1
	// frame: 11/11 real picks corroborated at expected-1 on 2026-09-22, and the same fret read
	// expected instead on the 2026-08-30 E<->Eb flicker day), so tolerate +/-1. A ringing octave
	// or fifth reads +11..+19 away and never falls inside this window, which is the whole point.
	inline bool NativeCorroboratesPick(int nativeMidi, int midi)
	{
		return nativeMidi >= 0 && std::abs(nativeMidi - midi) <= 1;
	}
}
