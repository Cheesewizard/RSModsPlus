#pragma once

#include "../../DLL/Audio/AsioHook.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace AudioCaptureTests
{
	inline void RequireNoise(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	inline void NoiseSuppressorOffIsBitExact()
	{
		const std::array<float, 8> original{ 0.0f, 0.25f, -0.75f, 1.0f, -1.0f, 0.0001f, -0.2f, 0.6f };
		auto samples = original;
		Audio::AsioHook::Detail::NoiseSuppressor suppressor;
		suppressor.Process(samples.data(), samples.size(), 48000, 0.0f);
		RequireNoise(samples == original, "Disabled noise suppressor changed samples");
	}

	inline void NoiseSuppressorSuppressesFloor()
	{
		std::array<float, 4800> samples{};
		samples.fill(std::pow(10.0f, -70.0f / 20.0f));
		Audio::AsioHook::Detail::NoiseSuppressor suppressor;
		suppressor.Process(samples.data(), samples.size(), 48000, std::pow(10.0f, -40.0f / 20.0f));
		const float peak = *std::max_element(samples.begin(), samples.end());
		RequireNoise(peak < 1.0e-6f, "-70 dBFS floor was not deeply suppressed");
	}

	inline void NoiseSuppressorRejectsSpike()
	{
		std::array<float, 480> samples{};
		samples[0] = 1.0f;
		Audio::AsioHook::Detail::NoiseSuppressor suppressor;
		suppressor.Process(samples.data(), samples.size(), 48000, std::pow(10.0f, -40.0f / 20.0f));
		RequireNoise(samples[0] < 0.001f, "One-sample spike opened the suppressor");
		RequireNoise(*std::max_element(samples.begin(), samples.end()) < 0.001f, "Short spike leaked through confirmation");
	}

	inline void NoiseSuppressorRejectsSustainedFault()
	{
		std::array<float, 4800> samples{};
		samples.fill(std::pow(10.0f, -37.0f / 20.0f));
		Audio::AsioHook::Detail::NoiseSuppressor suppressor;
		suppressor.Process(samples.data(), samples.size(), 48000, std::pow(10.0f, -50.0f / 20.0f));
		const float peak = *std::max_element(samples.begin(), samples.end());
		RequireNoise(peak < 1.0e-3f, "Sustained -37 dBFS fault opened the suppressor");
	}

	inline void NoiseSuppressorOpensTone()
	{
		std::array<float, 960> samples{};
		for (size_t index = 0; index < samples.size(); ++index)
			samples[index] = 0.25f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(index) / 48000.0f);
		Audio::AsioHook::Detail::NoiseSuppressor suppressor;
		suppressor.Process(samples.data(), samples.size(), 48000, std::pow(10.0f, -40.0f / 20.0f));
		const auto onset = std::max_element(samples.begin() + 200, samples.begin() + 480,
			[](float left, float right) { return std::fabs(left) < std::fabs(right); });
		RequireNoise(std::fabs(*onset) > 0.15f, "Sustained guitar-like tone did not open promptly");
	}

	inline void NoiseSuppressorKeepsLowTailOpen()
	{
		std::array<float, 9600> samples{};
		for (size_t index = 0; index < 4800; ++index)
			samples[index] = 0.25f;
		for (size_t index = 4800; index < samples.size(); ++index)
			samples[index] = std::pow(10.0f, -37.0f / 20.0f);
		Audio::AsioHook::Detail::NoiseSuppressor suppressor;
		suppressor.Process(samples.data(), samples.size(), 48000, std::pow(10.0f, -50.0f / 20.0f));
		RequireNoise(samples[7200] > 0.01f, "Sustain below attack threshold was incorrectly closed");
	}

	inline void NoiseSuppressorClosesSmoothly()
	{
		Audio::AsioHook::Detail::NoiseSuppressor suppressor;
		const float threshold = std::pow(10.0f, -40.0f / 20.0f);
		suppressor.Configure(48000, threshold);
		for (size_t index = 0; index < 4800; ++index)
			suppressor.NextGain(0.25f, threshold);
		const float firstTail = suppressor.NextGain(0.001f, threshold);
		float laterTail = firstTail;
		for (size_t index = 0; index < 36000; ++index)
			laterTail = suppressor.NextGain(0.001f, threshold);
		float finalTail = laterTail;
		float maximumStep = 0.0f;
		for (size_t index = 0; index < 12000; ++index)
		{
			const float next = suppressor.NextGain(0.001f, threshold);
			maximumStep = std::max(maximumStep, std::fabs(next - finalTail));
			finalTail = next;
		}
		RequireNoise(firstTail > laterTail && laterTail > finalTail, "Suppressor decay was not smooth");
		RequireNoise(maximumStep < 0.01f, "Suppressor closed too abruptly");
	}
}
