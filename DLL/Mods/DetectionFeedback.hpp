#pragma once

#include <cstdint>
#include <cmath>
#include <string>

#include "../PitchNames.hpp"

namespace NoteByNote
{
	enum class DetectorRole : uint8_t
	{
		Unused,
		Confirmed,
		Partial,
		Rejected
	};

	struct DetectionFeedback
	{
		uint64_t tick = 0;
		uintptr_t targetRecord = 0;
		DetectorRole nativeRole = DetectorRole::Unused;
		DetectorRole enhancedRole = DetectorRole::Unused;
		DetectorRole mlRole = DetectorRole::Unused;
		int nativeMidi = -1;
		int enhancedMidi = -1;
		int mlMidi = -1;
		int targetMidi = -1;
		int stringIndex = -1;
		int fret = -1;
	};

	inline bool MatchesDetectorTarget(int midi, int targetMidi, float confidence = 1.0f)
	{
		return targetMidi >= 0 && targetMidi <= 127 && midi == targetMidi
			&& std::isfinite(confidence) && confidence >= 0.5f && confidence <= 1.0f;
	}

	inline void SetPickedDetectorRoles(DetectionFeedback& feedback, bool nativeAgrees,
		bool mlConfirms)
	{
		feedback.mlRole = mlConfirms ? DetectorRole::Confirmed : DetectorRole::Unused;
		feedback.nativeRole = feedback.nativeMidi < 0 ? DetectorRole::Unused
			: nativeAgrees ? DetectorRole::Confirmed : DetectorRole::Rejected;
		feedback.enhancedRole = DetectorRole::Confirmed;
	}

	inline uint32_t GetDetectorColor(DetectorRole role, uint64_t tick, uint64_t now)
	{
		constexpr uint64_t HOLD_MS = 650;
		constexpr uint64_t FADE_MS = 650;
		if (tick == 0 || now < tick || now - tick >= HOLD_MS + FADE_MS) return 0xFFFFFFFF;
		uint32_t color = 0xFFFFFFFF;
		switch (role)
		{
		case DetectorRole::Confirmed: color = 0xFF55DD77; break;
		case DetectorRole::Partial: color = 0xFFFFAA44; break;
		case DetectorRole::Rejected: color = 0xFFFF5555; break;
		default: break;
		}
		const uint64_t elapsed = now - tick;
		if (elapsed <= HOLD_MS) return color;
		uint32_t faded = 0xFF000000;
		for (unsigned shift = 0; shift <= 16; shift += 8)
		{
			const uint32_t channel = (color >> shift) & 255;
			faded |= (channel + static_cast<uint32_t>((255 - channel) * (elapsed - HOLD_MS) / FADE_MS)) << shift;
		}
		return faded;
	}

	inline void PublishDetectionFeedback(DetectionFeedback& displayed, DetectionFeedback next)
	{
		if (next.targetMidi >= 0 && next.targetMidi <= 127)
		{
			if (next.nativeMidi == next.targetMidi)
				next.nativeRole = DetectorRole::Confirmed;
			if (next.enhancedMidi == next.targetMidi && next.enhancedRole == DetectorRole::Partial)
				next.enhancedRole = DetectorRole::Confirmed;
		}
		// A later stage of the same attack may have no new opinion from one detector.
		// Keep its visible result until the shared timer expires or a pick clears it.
		if (next.targetRecord != 0 && next.targetRecord == displayed.targetRecord
			&& next.targetMidi == displayed.targetMidi)
		{
			if (next.nativeRole == DetectorRole::Unused
				&& GetDetectorColor(displayed.nativeRole, displayed.tick, next.tick) != 0xFFFFFFFF)
			{
				next.nativeRole = displayed.nativeRole;
				next.nativeMidi = displayed.nativeMidi;
			}
			if (next.enhancedRole == DetectorRole::Unused
				&& GetDetectorColor(displayed.enhancedRole, displayed.tick, next.tick) != 0xFFFFFFFF)
			{
				next.enhancedRole = displayed.enhancedRole;
				next.enhancedMidi = displayed.enhancedMidi;
			}
			if (next.mlRole == DetectorRole::Unused
				&& GetDetectorColor(displayed.mlRole, displayed.tick, next.tick) != 0xFFFFFFFF)
			{
				next.mlRole = displayed.mlRole;
				next.mlMidi = displayed.mlMidi;
			}
		}
		displayed = next;
	}

	inline bool CreditConfirmedMlFeedback(DetectionFeedback& feedback, uint64_t analyzedSample,
		uint64_t minimumSample, uint64_t maximumSample, uint64_t now)
	{
		if (feedback.targetMidi < 0 || feedback.targetMidi > 127
			|| analyzedSample <= minimumSample || analyzedSample > maximumSample
			|| GetDetectorColor(DetectorRole::Confirmed, feedback.tick, now) == 0xFFFFFFFF) return false;
		feedback.mlMidi = feedback.targetMidi;
		feedback.mlRole = DetectorRole::Confirmed;
		return true;
	}

	inline std::string FormatPitch(int midi)
	{
		if (midi < 0 || midi > 127) return "--";
		return PitchNames::ForPitchClass(midi);
	}

	inline std::string FormatPosition(int stringIndex, int fret, int openStringMidi)
	{
		if (stringIndex < 0 || stringIndex >= 6 || fret < 0 || fret > 24
			|| openStringMidi < 0 || openStringMidi + fret > 127) return "--";
		const std::string prefix = stringIndex == 0 ? "low " : stringIndex == 5 ? "high " : "";
		return prefix + FormatPitch(openStringMidi) + " string, "
			+ (fret == 0 ? "open" : "fret " + std::to_string(fret));
	}

	inline int GetStringFretMidi(int stringIndex, int soundingFret)
	{
		static constexpr int OPEN_MIDI[] = { 40, 45, 50, 55, 59, 64 };
		if (stringIndex < 0 || stringIndex >= 6 || soundingFret < 0 || soundingFret > 24) return -1;
		return OPEN_MIDI[stringIndex] + soundingFret;
	}
}
