#pragma once

#include <cmath>
#include <cstdint>

namespace NoteByNote
{
	inline bool SupportsCloseChord(const int* tones, int count)
	{
		return tones && count == 2 && tones[0] >= 57 && tones[0] <= 88
			&& tones[1] >= 57 && tones[1] <= 88
			&& std::abs(tones[0] - tones[1]) >= 1 && std::abs(tones[0] - tones[1]) <= 2;
	}

	inline bool ConfirmCloseChord(const float* samples, uint32_t sampleCount, uint32_t sampleRate,
		const int* tones, int toneCount)
	{
		if (!samples || !SupportsCloseChord(tones, toneCount) || sampleRate < 8000 || sampleRate > 96000) return false;
		const uint32_t count = sampleRate / 4;
		if (count > sampleCount || count > 24000) return false;
		const float* source = samples + sampleCount - count;
		float window[24000];
		double energy = 0.0;
		for (uint32_t i = 0; i < count; ++i)
		{
			if (!std::isfinite(source[i])) return false;
			energy += source[i] * source[i];
			window[i] = static_cast<float>(source[i] * (0.5 - 0.5 * std::cos(6.283185307179586 * i / (count - 1))));
		}
		energy /= count;
		if (energy < 1e-6) return false;
		auto powerAt = [&](double frequency)
		{
			const double coefficient = 2.0 * std::cos(6.283185307179586 * frequency / sampleRate);
			double previous = 0.0, beforePrevious = 0.0;
			for (uint32_t i = 0; i < count; ++i)
			{
				const double next = window[i] + coefficient * previous - beforePrevious;
				beforePrevious = previous;
				previous = next;
			}
			return (previous * previous + beforePrevious * beforePrevious - coefficient * previous * beforePrevious)
				* 16.0 / (static_cast<double>(count) * count);
		};
		for (int tone = 0; tone < toneCount; ++tone)
		{
			double bestPower = 0.0, bestFrequency = 0.0;
			for (int cents = -30; cents <= 30; cents += 10)
			{
				const double frequency = 440.0 * std::pow(2.0, (tones[tone] - 69.0 + cents / 100.0) / 12.0);
				const double power = powerAt(frequency);
				if (power > bestPower) { bestPower = power; bestFrequency = frequency; }
			}
			if (bestPower < 1e-5 || bestPower < energy * 0.03) return false;
			// Each note needs its own spectral peak. Merely exempting a neighbouring
			// chord tone from a dominance test lets one note's leakage count twice.
			const double shoulderRatio = std::pow(2.0, 40.0 / 1200.0);
			if (bestPower < 1.15 * powerAt(bestFrequency / shoulderRatio)
				|| bestPower < 1.15 * powerAt(bestFrequency * shoulderRatio)) return false;
		}
		return true;
	}
}
