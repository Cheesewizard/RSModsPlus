#include "stdafx.h"
#include "RawPitchVerifier.hpp"

#include <atomic>
#include <cmath>
#include <cstring>

namespace
{
	// ~0.68 s at 48 kHz. Power of two so wrap arithmetic is a mask. The query window is
	// capped at 0.3 s, well under half the ring, so the audio thread cannot overwrite a
	// window while a reader copies it unless the reader stalls for >0.3 s - and a torn
	// tail merely perturbs an energy measurement, it cannot fault.
	constexpr uint32_t RING_CAPACITY = 32768;
	constexpr uint32_t RING_MASK = RING_CAPACITY - 1;

	float ring[RING_CAPACITY] = {};
	std::atomic<uint64_t> ringWriteCount{ 0 };
	std::atomic<uint32_t> ringSampleRate{ 0 };

	// One Goertzel pass over a window for a single frequency. Returns power normalized
	// by (n/2)^2: a full-scale sine at the frequency reads ~1.0 for any window length.
	float GoertzelPower(const float* window, uint32_t count, double frequencyHz, double sampleRate)
	{
		if (count < 8 || frequencyHz <= 0.0 || frequencyHz >= sampleRate * 0.5) return 0.0f;
		const double omega = 6.283185307179586 * frequencyHz / sampleRate;
		const double coefficient = 2.0 * std::cos(omega);
		double s0 = 0.0;
		double s1 = 0.0;
		double s2 = 0.0;
		for (uint32_t index = 0; index < count; ++index)
		{
			s0 = static_cast<double>(window[index]) + coefficient * s1 - s2;
			s2 = s1;
			s1 = s0;
		}
		const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
		const double half = static_cast<double>(count) * 0.5;
		const double normalized = power / (half * half);
		return normalized > 0.0 ? static_cast<float>(normalized) : 0.0f;
	}
}

void RawPitchVerifier::Observe(uint32_t routeIndex, const float* samples, uint32_t count,
	uint32_t sampleRate)
{
	if (routeIndex != 0 || samples == nullptr || count == 0) return;
	if (sampleRate != 0) ringSampleRate.store(sampleRate, std::memory_order_relaxed);

	uint64_t writeCount = ringWriteCount.load(std::memory_order_relaxed);
	for (uint32_t index = 0; index < count; ++index)
	{
		ring[(writeCount + index) & RING_MASK] = samples[index];
	}
	ringWriteCount.store(writeCount + count, std::memory_order_release);
}

bool RawPitchVerifier::IsReady()
{
	return ringSampleRate.load(std::memory_order_relaxed) != 0
		&& ringWriteCount.load(std::memory_order_acquire) >= RING_CAPACITY / 4;
}

bool RawPitchVerifier::QueryToneEvidence(double frequencyHz, float windowSeconds,
	ToneEvidence& out)
{
	out = {};
	const uint32_t sampleRate = ringSampleRate.load(std::memory_order_relaxed);
	if (sampleRate == 0 || frequencyHz <= 0.0) return false;

	if (windowSeconds < 0.05f) windowSeconds = 0.05f;
	if (windowSeconds > 0.3f) windowSeconds = 0.3f;
	uint32_t windowCount = static_cast<uint32_t>(windowSeconds * static_cast<float>(sampleRate));
	if (windowCount > RING_CAPACITY / 2) windowCount = RING_CAPACITY / 2;

	const uint64_t writeCount = ringWriteCount.load(std::memory_order_acquire);
	if (writeCount < windowCount) return false;

	// Copy the newest windowCount samples out of the ring (oldest-first order; Goertzel
	// does not care about absolute phase, only that the samples are contiguous).
	static thread_local float window[RING_CAPACITY / 2];
	const uint64_t start = writeCount - windowCount;
	for (uint32_t index = 0; index < windowCount; ++index)
	{
		window[index] = ring[(start + index) & RING_MASK];
	}

	double sumSquares = 0.0;
	for (uint32_t index = 0; index < windowCount; ++index)
	{
		sumSquares += static_cast<double>(window[index]) * window[index];
	}

	constexpr double SEMITONE = 1.0594630943592953;
	out.sampleRate = sampleRate;
	out.windowSampleCount = windowCount;
	out.totalRms = static_cast<float>(std::sqrt(sumSquares / windowCount));
	out.targetPower = GoertzelPower(window, windowCount, frequencyHz, sampleRate);
	out.minusOnePower = GoertzelPower(window, windowCount, frequencyHz / SEMITONE, sampleRate);
	out.plusOnePower = GoertzelPower(window, windowCount, frequencyHz * SEMITONE, sampleRate);
	out.minusTwoPower = GoertzelPower(window, windowCount, frequencyHz / (SEMITONE * SEMITONE), sampleRate);
	out.plusTwoPower = GoertzelPower(window, windowCount, frequencyHz * SEMITONE * SEMITONE, sampleRate);
	return true;
}
