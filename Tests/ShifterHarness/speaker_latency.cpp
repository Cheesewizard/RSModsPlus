#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../../DLL/Audio/ThirdParty/SignalsmithStretch/signalsmith-stretch.h"

namespace
{
	constexpr int SAMPLE_RATE = 48000;
	constexpr int CALLBACK_FRAMES = 128;
	constexpr int TEST_SECONDS = 30;
	constexpr double PI = 3.14159265358979323846;

	struct Configuration
	{
		const char* name;
		int blockSamples;
		int intervalSamples;
	};

	struct StereoSignal
	{
		std::vector<float> left;
		std::vector<float> right;
	};

	StereoSignal ProcessSignal(
		const Configuration& configuration,
		const StereoSignal& input,
		float semitones,
		double& elapsedSeconds)
	{
		signalsmith::stretch::SignalsmithStretch<float> stretch(0);
		stretch.configure(2, configuration.blockSamples, configuration.intervalSamples);
		stretch.setTransposeSemitones(semitones);

		StereoSignal output;
		output.left.resize(input.left.size());
		output.right.resize(input.right.size());

		const auto started = std::chrono::steady_clock::now();
		for (size_t offset = 0; offset < input.left.size(); offset += CALLBACK_FRAMES)
		{
			const int frameCount = static_cast<int>(std::min<size_t>(
				CALLBACK_FRAMES,
				input.left.size() - offset));
			const float* inputChannels[] = { input.left.data() + offset, input.right.data() + offset };
			float* outputChannels[] = { output.left.data() + offset, output.right.data() + offset };
			stretch.process(inputChannels, frameCount, outputChannels, frameCount);
		}
		elapsedSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - started).count();
		return output;
	}

	StereoSignal ProcessFixedSignal(
		const Configuration& configuration,
		const StereoSignal& input,
		float semitones)
	{
		signalsmith::stretch::SignalsmithStretch<float> stretch(0);
		stretch.configure(2, configuration.blockSamples, configuration.intervalSamples);
		stretch.setTransposeSemitones(semitones);

		const int seekFrames = stretch.outputSeekLength(1.0f);
		const float* seekChannels[] = { input.left.data(), input.right.data() };
		stretch.outputSeek(seekChannels, seekFrames);

		StereoSignal output;
		output.left.resize(input.left.size());
		output.right.resize(input.right.size());
		size_t outputOffset = 0;
		for (size_t inputOffset = seekFrames; inputOffset < input.left.size(); inputOffset += CALLBACK_FRAMES)
		{
			const int frameCount = static_cast<int>(std::min<size_t>(
				CALLBACK_FRAMES,
				input.left.size() - inputOffset));
			const float* inputChannels[] =
			{
				input.left.data() + inputOffset,
				input.right.data() + inputOffset
			};
			float* outputChannels[] =
			{
				output.left.data() + outputOffset,
				output.right.data() + outputOffset
			};
			stretch.process(inputChannels, frameCount, outputChannels, frameCount);
			outputOffset += frameCount;
		}

		float* flushChannels[] =
		{
			output.left.data() + outputOffset,
			output.right.data() + outputOffset
		};
		stretch.flush(flushChannels, seekFrames, 1.0f);
		return output;
	}

	StereoSignal GenerateTestSignal()
	{
		const int sampleCount = SAMPLE_RATE * TEST_SECONDS;
		StereoSignal signal;
		signal.left.resize(sampleCount);
		signal.right.resize(sampleCount);

		uint32_t noiseState = 0x12345678;
		for (int sample = 0; sample < sampleCount; sample++)
		{
			const double time = sample / static_cast<double>(SAMPLE_RATE);
			noiseState = noiseState * 1664525u + 1013904223u;
			const float noise = (static_cast<int32_t>(noiseState) / 2147483648.0f) * 0.015f;
			const double transientPhase = std::fmod(time, 0.5);
			const double transient = transientPhase < 0.015
				? std::exp(-transientPhase * 260.0) * std::sin(2.0 * PI * 1200.0 * time)
				: 0.0;

			signal.left[sample] = static_cast<float>(
				0.18 * std::sin(2.0 * PI * 41.2 * time)
				+ 0.20 * std::sin(2.0 * PI * 82.4 * time)
				+ 0.16 * std::sin(2.0 * PI * 110.0 * time)
				+ 0.12 * std::sin(2.0 * PI * 196.0 * time)
				+ 0.10 * std::sin(2.0 * PI * 329.6 * time)
				+ 0.08 * transient + noise);
			signal.right[sample] = static_cast<float>(
				0.18 * std::sin(2.0 * PI * 41.2 * time + 0.2)
				+ 0.20 * std::sin(2.0 * PI * 82.4 * time + 0.1)
				+ 0.16 * std::sin(2.0 * PI * 146.8 * time)
				+ 0.12 * std::sin(2.0 * PI * 246.9 * time)
				+ 0.10 * std::sin(2.0 * PI * 440.0 * time)
				+ 0.08 * transient - noise);
		}

		return signal;
	}

	double MeasureFrequency(
		const std::vector<float>& samples,
		int start,
		int length,
		double expectedFrequency)
	{
		double bestFrequency = 0.0;
		double bestPower = 0.0;
		for (double frequency = expectedFrequency * 0.98;
			frequency <= expectedFrequency * 1.02;
			frequency *= 1.0001)
		{
			const double coefficient = 2.0 * std::cos(2.0 * PI * frequency / SAMPLE_RATE);
			double previous = 0.0;
			double previousPrevious = 0.0;
			for (int sample = 0; sample < length; sample++)
			{
				const double window = 0.5 - 0.5 * std::cos(2.0 * PI * sample / length);
				const double current = samples[start + sample] * window
					+ coefficient * previous - previousPrevious;
				previousPrevious = previous;
				previous = current;
			}

			const double power = previous * previous + previousPrevious * previousPrevious
				- coefficient * previous * previousPrevious;
			if (power > bestPower)
			{
				bestPower = power;
				bestFrequency = frequency;
			}
		}

		return bestFrequency;
	}

	double CorrelationAtLag(
		const std::vector<float>& input,
		const std::vector<float>& output,
		int outputStart,
		int length,
		int lag,
		int stride);

	void RunConfiguration(const Configuration& configuration, const StereoSignal& input)
	{
		double elapsed = 0.0;
		const auto output = ProcessSignal(configuration, input, 1.0f, elapsed);

		const double expectedFrequency = 82.4 * std::pow(2.0, 1.0 / 12.0);
		const int analysisStart = SAMPLE_RATE * 10;
		const int analysisLength = SAMPLE_RATE * 2;
		const double measuredFrequency = MeasureFrequency(
			output.left,
			analysisStart,
			analysisLength,
			expectedFrequency);
		const double pitchErrorCents = 1200.0 * std::log2(measuredFrequency / expectedFrequency);
		const double inputLatencyMs = configuration.blockSamples * 500.0 / SAMPLE_RATE;
		const double outputLatencyMs = configuration.blockSamples * 500.0 / SAMPLE_RATE;
		const double realTimeCpuPercent = elapsed / TEST_SECONDS * 100.0;

		std::printf(
			"%-12s block %4d interval %4d | latency %5.1f + %5.1f = %5.1f ms"
			" | CPU %5.2f%% real-time | pitch error %+5.2f cents\n",
			configuration.name,
			configuration.blockSamples,
			configuration.intervalSamples,
			inputLatencyMs,
			outputLatencyMs,
			inputLatencyMs + outputLatencyMs,
			realTimeCpuPercent,
			pitchErrorCents);
	}

	void RunFixedAlignment(const Configuration& configuration, const StereoSignal& input)
	{
		const auto shifted = ProcessFixedSignal(configuration, input, 1.0f);
		const auto restored = ProcessFixedSignal(configuration, shifted, -1.0f);
		const int outputStart = SAMPLE_RATE * 12;
		const int correlationLength = SAMPLE_RATE / 2;
		int bestLag = 0;
		double bestCorrelation = -1.0;
		for (int lag = -configuration.intervalSamples; lag <= configuration.intervalSamples; lag++)
		{
			const double correlation = CorrelationAtLag(
				input.left,
				restored.left,
				outputStart,
				correlationLength,
				lag,
				2);
			if (correlation > bestCorrelation)
			{
				bestCorrelation = correlation;
				bestLag = lag;
			}
		}

		std::printf(
			"%-12s fixed      | aligned lag %+5.2f ms | correlation %.4f\n",
			configuration.name,
			bestLag * 1000.0 / SAMPLE_RATE,
			bestCorrelation);
	}

	bool ReadPcm16Stereo(const char* path, StereoSignal& signal)
	{
		FILE* file = std::fopen(path, "rb");
		if (file == nullptr) return false;

		const size_t maximumFrames = SAMPLE_RATE * TEST_SECONDS;
		std::vector<int16_t> interleaved(maximumFrames * 2);
		const size_t sampleCount = std::fread(
			interleaved.data(),
			sizeof(int16_t),
			interleaved.size(),
			file);
		std::fclose(file);

		const size_t frameCount = sampleCount / 2;
		if (frameCount < SAMPLE_RATE * 15) return false;

		signal.left.resize(frameCount);
		signal.right.resize(frameCount);
		for (size_t frame = 0; frame < frameCount; frame++)
		{
			signal.left[frame] = interleaved[frame * 2] / 32768.0f;
			signal.right[frame] = interleaved[frame * 2 + 1] / 32768.0f;
		}
		return true;
	}

	double CorrelationAtLag(
		const std::vector<float>& input,
		const std::vector<float>& output,
		int outputStart,
		int length,
		int lag,
		int stride)
	{
		double dot = 0.0;
		double inputPower = 0.0;
		double outputPower = 0.0;
		for (int sample = 0; sample < length; sample += stride)
		{
			const double inputSample = input[outputStart + sample - lag];
			const double outputSample = output[outputStart + sample];
			dot += inputSample * outputSample;
			inputPower += inputSample * inputSample;
			outputPower += outputSample * outputSample;
		}
		return dot / std::sqrt(inputPower * outputPower);
	}

	void RunSongRoundTrip(const Configuration& configuration, const StereoSignal& input)
	{
		double shiftUpSeconds = 0.0;
		double shiftDownSeconds = 0.0;
		const auto shifted = ProcessSignal(configuration, input, 1.0f, shiftUpSeconds);
		const auto restored = ProcessSignal(configuration, shifted, -1.0f, shiftDownSeconds);

		const int outputStart = SAMPLE_RATE * 12;
		const int correlationLength = SAMPLE_RATE / 2;
		const int expectedLag = configuration.blockSamples * 2;
		const int searchRadius = configuration.intervalSamples;
		int bestLag = expectedLag;
		double bestCorrelation = -1.0;
		for (int lag = expectedLag - searchRadius; lag <= expectedLag + searchRadius; lag += 8)
		{
			const double correlation = CorrelationAtLag(
				input.left,
				restored.left,
				outputStart,
				correlationLength,
				lag,
				4);
			if (correlation > bestCorrelation)
			{
				bestCorrelation = correlation;
				bestLag = lag;
			}
		}
		for (int lag = bestLag - 8; lag <= bestLag + 8; lag++)
		{
			const double correlation = CorrelationAtLag(
				input.left,
				restored.left,
				outputStart,
				correlationLength,
				lag,
				1);
			if (correlation > bestCorrelation)
			{
				bestCorrelation = correlation;
				bestLag = lag;
			}
		}

		std::printf(
			"%-12s round-trip | aligned lag %5.1f ms | correlation %.4f | CPU %5.2f%% real-time\n",
			configuration.name,
			bestLag * 1000.0 / SAMPLE_RATE,
			bestCorrelation,
			(shiftUpSeconds + shiftDownSeconds) / (TEST_SECONDS * 2) * 100.0);
	}
}

int main(int argumentCount, char** arguments)
{
	const Configuration configurations[] =
	{
		{ "Current", SAMPLE_RATE * 120 / 1000, SAMPLE_RATE * 30 / 1000 },
		{ "80 ms", SAMPLE_RATE * 80 / 1000, SAMPLE_RATE * 20 / 1000 },
		{ "60 ms", SAMPLE_RATE * 60 / 1000, SAMPLE_RATE * 15 / 1000 },
		{ "40 ms", SAMPLE_RATE * 40 / 1000, SAMPLE_RATE * 10 / 1000 }
	};

	const auto input = GenerateTestSignal();
	std::printf("Signalsmith stereo stream benchmark: 48 kHz, 128-frame callbacks, +1 semitone\n");
	for (const auto& configuration : configurations)
	{
		RunConfiguration(configuration, input);
	}

	std::printf("\nOffline fixed-length alignment (+1/-1 semitone round trip):\n");
	for (const auto& configuration : configurations)
	{
		RunFixedAlignment(configuration, input);
	}

	if (argumentCount == 2)
	{
		StereoSignal song;
		if (!ReadPcm16Stereo(arguments[1], song))
		{
			std::fprintf(stderr, "Could not read at least 15 seconds of stereo 16-bit raw PCM.\n");
			return 1;
		}

		std::printf("\nActual-song +1/-1 semitone round trip (first 30 seconds):\n");
		for (const auto& configuration : configurations)
		{
			RunSongRoundTrip(configuration, song);
		}
	}

	return 0;
}
