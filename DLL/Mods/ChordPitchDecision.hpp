#pragma once

namespace NoteByNote
{
	enum class ChordPitchConfirmation
	{
		None,
		Unison,
		Natural,
		NativeMatcher,
		CloseDyad,
		MlStrings,
		Tier0,
		PowerChord
	};

	struct ChordPitchDecisionInput
	{
		int toneCount = 0;
		bool isFretHandMuted = false;
		bool didBuildTarget = false;
		bool hasFreshAttack = false;
		bool isCorroborated = false;
		bool isUnison = false;
		bool rawUnisonMatches = false;
		bool naturalMatches = false;
		bool nativeMatcherMatches = false;
		bool closeDyadMatches = false;
		bool mlStringsMatch = false;
		bool isTier0Enabled = false;
		bool tier0Matches = false;
		// Power chord: the fifth confirmed in the raw audio with a fresh attack
		// (PowerChordConfirmation.hpp), and the game's vote or chord matcher agreed on this strum.
		bool powerChordRawMatches = false;
		bool nativeAgreedOnStrum = false;
	};

	inline ChordPitchConfirmation EvaluateChordPitchDecision(
		const ChordPitchDecisionInput& input)
	{
		if (input.isFretHandMuted || !input.didBuildTarget || !input.hasFreshAttack)
		{
			return ChordPitchConfirmation::None;
		}

		if (input.isUnison)
		{
			return input.rawUnisonMatches
				? ChordPitchConfirmation::Unison
				: ChordPitchConfirmation::None;
		}

		// These paths confirm every authored tone from the fresh attack itself. Do not
		// put the native sounding table in front of stronger exact evidence: it routinely
		// under-reports a real close dyad as one of two tones.
		if (input.closeDyadMatches) return ChordPitchConfirmation::CloseDyad;
		// The game's sounding table almost never lists a power chord's fifth, so the table bar
		// below is unreachable for one. The fifth confirmed in the raw audio (which a root alone
		// cannot produce) plus the game's own vote or matcher on this strum stands in for it.
		if (input.powerChordRawMatches && input.nativeAgreedOnStrum) return ChordPitchConfirmation::PowerChord;
		// ML per-string reads confirm a chord of three or more tones ahead of the sounding
		// table, but a two-note chord needs the table behind it: FretNet read a ringing
		// sub-octave's second harmonic as the lower tone of [x/x/7/5/x/x] and accepted a
		// single note as the dyad (2026-09-22 line 6349). A dyad may only take the ML path
		// once corroborated.
		if (input.mlStringsMatch && input.toneCount >= 3) return ChordPitchConfirmation::MlStrings;
		if (!input.isCorroborated) return ChordPitchConfirmation::None;

		if (input.naturalMatches) return ChordPitchConfirmation::Natural;
		if (input.nativeMatcherMatches) return ChordPitchConfirmation::NativeMatcher;
		if (input.mlStringsMatch) return ChordPitchConfirmation::MlStrings;
		if (input.isTier0Enabled && input.tier0Matches) return ChordPitchConfirmation::Tier0;
		return ChordPitchConfirmation::None;
	}

	inline const char* GetChordPitchConfirmationName(ChordPitchConfirmation confirmation)
	{
		switch (confirmation)
		{
			case ChordPitchConfirmation::Unison: return "unison";
			case ChordPitchConfirmation::Natural: return "natural";
			case ChordPitchConfirmation::NativeMatcher: return "native-matcher";
			case ChordPitchConfirmation::CloseDyad: return "close-dyad";
			case ChordPitchConfirmation::MlStrings: return "ml-strings";
			case ChordPitchConfirmation::Tier0: return "tier0";
			case ChordPitchConfirmation::PowerChord: return "power-chord";
			default: return "none";
		}
	}

	inline int GetRequiredChordSoundingToneCount(int toneCount)
	{
		return toneCount - (toneCount >= 5 ? 1 : 0);
	}

	inline bool IsChordSoundingCorroborated(
		int toneCount,
		int soundingPeak,
		bool naturalMatches,
		double attackAgeMilliseconds)
	{
		if (toneCount < 1 || soundingPeak < 0) return false;
		const bool chordSounded = soundingPeak >= GetRequiredChordSoundingToneCount(toneCount);
		const int halfSounding = (toneCount + 1) / 2;
		// A native vote may compensate for the sounding table under-reporting a
		// larger chord, but it must never reduce a two-note chord to one note.
		const bool votedAndHalf = toneCount >= 3
			&& naturalMatches && soundingPeak >= halfSounding;
		return (chordSounded && (naturalMatches || attackAgeMilliseconds <= 100.0))
			|| votedAndHalf;
	}
}
