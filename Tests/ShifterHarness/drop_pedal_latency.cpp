// Measures the observable content delay added by the production Drop Pedal shifter.
// A deterministic amplitude pattern survives pitch shifting, allowing input and output
// envelopes to be aligned even though their carrier frequencies are different.
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>
#define NOMINMAX
#include <windows.h>

#include "DelayLinePitchShifter.hpp"

namespace
{
	constexpr double SAMPLE_RATE = 48000.0;
	constexpr double PI = 3.14159265358979323846;
	constexpr uint32_t CALLBACK_FRAMES = 128;
	constexpr int TOTAL_FRAMES = 12 * (int)SAMPLE_RATE;
	constexpr int WARMUP_FRAMES = 2 * (int)SAMPLE_RATE;
	constexpr int ENVELOPE_WINDOW_FRAMES = 4096;
	constexpr int ENVELOPE_DECIMATION = 16;
	constexpr int MAX_DELAY_FRAMES = 4096;
	constexpr int TRANSITION_RADIUS = ENVELOPE_WINDOW_FRAMES / (2 * ENVELOPE_DECIMATION);
	constexpr double CALLBACK_DEADLINE_MICROSECONDS = CALLBACK_FRAMES * 1000000.0 / SAMPLE_RATE;
	constexpr const char* BUILD_ARCHITECTURE = sizeof(void*) == 8 ? "Win64" : "Win32";
	constexpr const char* RESULTS_FILE_NAME = "drop_pedal_latency_results.txt";
	FILE* resultsFile = nullptr;

	struct EnvelopeTransition
	{
		int frame;
		int direction;
	};

	struct MarkedNote
	{
		std::vector<float> samples;
		std::vector<EnvelopeTransition> transitions;
	};

	struct TransitionPeak
	{
		int index;
		double strength;
	};

	struct LatencySummary
	{
		double medianFrames;
		double percentile95Frames;
		double maximumFrames;
		double minimumStrengthRatio;
		int transitionCount;
	};

	struct ProcessingTimingSummary
	{
		double averageMicroseconds;
		double percentile95Microseconds;
		double percentile99Microseconds;
		double maximumMicroseconds;
		double maximumDeadlinePercent;
		int deadlineMissCount;
		int callbackCount;
	};

	struct MeasurementSummary
	{
		LatencySummary latency;
		ProcessingTimingSummary processing;
	};

	MarkedNote GenerateMarkedNote(double frequency, uint32_t patternState)
	{
		MarkedNote markedNote;
		markedNote.samples.resize(TOTAL_FRAMES);
		int nextLevelChange = 0;
		float level = 0.1f;
		double phases[5] = {};
		const double harmonicLevels[5] = { 1.0, 0.45, 0.25, 0.14, 0.08 };

		for (int frame = 0; frame < TOTAL_FRAMES; ++frame)
		{
			if (frame == nextLevelChange)
			{
				const float previousLevel = level;
				patternState = patternState * 1664525u + 1013904223u;
				const int intervalFrames = 24000 + (int)(patternState % 12001u);
				nextLevelChange += intervalFrames;
				level = level < 0.5f ? 0.9f : 0.1f;
				if (frame > 0)
				{
					markedNote.transitions.push_back({ frame, level > previousLevel ? 1 : -1 });
				}
			}

			double sample = 0.0;
			for (int harmonic = 0; harmonic < 5; ++harmonic)
			{
				phases[harmonic] += 2.0 * PI * frequency * (harmonic + 1) / SAMPLE_RATE;
				sample += harmonicLevels[harmonic] * std::sin(phases[harmonic]);
			}

			markedNote.samples[frame] = (float)(sample * level * 0.35);
		}

		return markedNote;
	}

	std::vector<float> CalculateEnvelope(const std::vector<float>& signal)
	{
		std::vector<float> envelope;
		envelope.reserve(signal.size() / ENVELOPE_DECIMATION + 1);
		double sumSquares = 0.0;

		for (size_t frame = 0; frame < signal.size(); ++frame)
		{
			const double sample = signal[frame];
			sumSquares += sample * sample;
			if (frame >= ENVELOPE_WINDOW_FRAMES)
			{
				const double removedSample = signal[frame - ENVELOPE_WINDOW_FRAMES];
				sumSquares -= removedSample * removedSample;
			}

			if (frame % ENVELOPE_DECIMATION == 0)
			{
				const int divisor = frame < ENVELOPE_WINDOW_FRAMES
					? (int)frame + 1
					: ENVELOPE_WINDOW_FRAMES;
				envelope.push_back((float)std::sqrt(std::max(0.0, sumSquares) / divisor));
			}
		}

		return envelope;
	}

	TransitionPeak FindTransitionPeak(
		const std::vector<float>& envelope,
		int start,
		int end,
		int direction)
	{
		start = std::max(start, TRANSITION_RADIUS);
		end = std::min(end, (int)envelope.size() - TRANSITION_RADIUS - 1);
		TransitionPeak peak{ start, -1.0 };
		for (int index = start; index <= end; ++index)
		{
			const double difference = direction * (
				envelope[index + TRANSITION_RADIUS]
				- envelope[index - TRANSITION_RADIUS]);
			if (difference > peak.strength)
			{
				peak = { index, difference };
			}
		}

		return peak;
	}

	double Percentile(std::vector<double> values, double percentile)
	{
		std::sort(values.begin(), values.end());
		if (values.empty()) return 0.0;

		const double position = percentile * (values.size() - 1);
		const size_t lowerIndex = (size_t)position;
		const size_t upperIndex = std::min(lowerIndex + 1, values.size() - 1);
		const double fraction = position - lowerIndex;
		return values[lowerIndex] * (1.0 - fraction) + values[upperIndex] * fraction;
	}

	void AppendLatencyMeasurements(
		double frequency,
		int semitones,
		uint32_t patternSeed,
		std::vector<double>& delays,
		double& minimumStrengthRatio,
		std::vector<double>& callbackMicroseconds)
	{
		const MarkedNote markedNote = GenerateMarkedNote(frequency, patternSeed);
		const std::vector<float>& input = markedNote.samples;
		std::vector<float> output = markedNote.samples;

		Audio::DelayLinePitchShifter shifter(semitones);
		Audio::CaptureFormat format;
		format.sampleRate = (uint32_t)SAMPLE_RATE;
		format.channelCount = 1;
		format.sampleFormat = Audio::SampleFormat::Float32;
		shifter.Prepare(format);

		for (size_t offset = 0; offset < output.size(); offset += CALLBACK_FRAMES)
		{
			const uint32_t frameCount = (uint32_t)std::min<size_t>(
				CALLBACK_FRAMES,
				output.size() - offset);
			const auto processingStart = std::chrono::steady_clock::now();
			shifter.Process(output.data() + offset, frameCount);
			const auto processingEnd = std::chrono::steady_clock::now();
			if (offset >= WARMUP_FRAMES && frameCount == CALLBACK_FRAMES)
			{
				callbackMicroseconds.push_back(std::chrono::duration<double, std::micro>(
					processingEnd - processingStart).count());
			}
		}

		const std::vector<float> inputEnvelope = CalculateEnvelope(input);
		const std::vector<float> outputEnvelope = CalculateEnvelope(output);

		for (const EnvelopeTransition& transition : markedNote.transitions)
		{
			if (transition.frame < WARMUP_FRAMES
				|| transition.frame + MAX_DELAY_FRAMES + ENVELOPE_WINDOW_FRAMES >= TOTAL_FRAMES)
			{
				continue;
			}

			const int expectedIndex = transition.frame / ENVELOPE_DECIMATION;
			const int transitionWidth = ENVELOPE_WINDOW_FRAMES / ENVELOPE_DECIMATION;
			const TransitionPeak inputPeak = FindTransitionPeak(
				inputEnvelope,
				expectedIndex - TRANSITION_RADIUS,
				expectedIndex + transitionWidth + TRANSITION_RADIUS,
				transition.direction);
			const TransitionPeak outputPeak = FindTransitionPeak(
				outputEnvelope,
				expectedIndex - TRANSITION_RADIUS,
				expectedIndex + (MAX_DELAY_FRAMES + ENVELOPE_WINDOW_FRAMES)
					/ ENVELOPE_DECIMATION + TRANSITION_RADIUS,
				transition.direction);

			delays.push_back((double)((outputPeak.index - inputPeak.index) * ENVELOPE_DECIMATION));
			if (inputPeak.strength > 0.0)
			{
				minimumStrengthRatio = std::min(
					minimumStrengthRatio,
					outputPeak.strength / inputPeak.strength);
			}
		}
	}

	MeasurementSummary MeasureLatency(double frequency, int semitones)
	{
		const uint32_t patternSeeds[] = { 0x5a17c9e3, 0xc31d872b, 0x16f42a95 };
		std::vector<double> delays;
		std::vector<double> callbackMicroseconds;
		double minimumStrengthRatio = 1.0;
		for (uint32_t patternSeed : patternSeeds)
		{
			AppendLatencyMeasurements(
				frequency,
				semitones,
				patternSeed,
				delays,
				minimumStrengthRatio,
				callbackMicroseconds);
		}

		const double maximumMicroseconds = *std::max_element(
			callbackMicroseconds.begin(),
			callbackMicroseconds.end());
		const int deadlineMissCount = (int)std::count_if(
			callbackMicroseconds.begin(),
			callbackMicroseconds.end(),
			[](double durationMicroseconds)
			{
				return durationMicroseconds > CALLBACK_DEADLINE_MICROSECONDS;
			});

		return {
			{
				Percentile(delays, 0.5),
				Percentile(delays, 0.95),
				*std::max_element(delays.begin(), delays.end()),
				minimumStrengthRatio,
				(int)delays.size()
			},
			{
				std::accumulate(callbackMicroseconds.begin(), callbackMicroseconds.end(), 0.0)
					/ callbackMicroseconds.size(),
				Percentile(callbackMicroseconds, 0.95),
				Percentile(callbackMicroseconds, 0.99),
				maximumMicroseconds,
				maximumMicroseconds * 100.0 / CALLBACK_DEADLINE_MICROSECONDS,
				deadlineMissCount,
				(int)callbackMicroseconds.size()
			}
		};
	}

	double FramesToMilliseconds(double frames)
	{
		return frames * 1000.0 / SAMPLE_RATE;
	}

	std::string GetResultsPath()
	{
		char executablePath[MAX_PATH] = {};
		const DWORD pathLength = GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
		if (pathLength == 0 || pathLength == MAX_PATH)
		{
			return RESULTS_FILE_NAME;
		}

		std::string resultsPath(executablePath, pathLength);
		const size_t separatorIndex = resultsPath.find_last_of("\\/");
		resultsPath.resize(separatorIndex == std::string::npos ? 0 : separatorIndex + 1);
		resultsPath += RESULTS_FILE_NAME;
		return resultsPath;
	}

	void PrintResult(const char* format, ...)
	{
		va_list arguments;
		va_start(arguments, format);
		std::vprintf(format, arguments);
		va_end(arguments);

		if (resultsFile == nullptr) return;

		va_start(arguments, format);
		std::vfprintf(resultsFile, format, arguments);
		va_end(arguments);
	}

	void WaitIfLaunchedFromExplorer()
	{
		DWORD processIds[2] = {};
		if (GetConsoleProcessList(processIds, 2) != 1) return;

		std::printf("\nPress Enter to close...");
		std::fflush(stdout);
		(void)std::getchar();
	}
}

int main()
{
	const double frequencies[] = { 41.20, 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 };
	const char* names[] = { "Bass E1", "Guitar E2", "Guitar A2", "Guitar D3", "Guitar G3", "Guitar B3", "Guitar E4" };
	const int shifts[] = { 0, -1, -2, -4, -12, 2 };
	std::vector<MeasurementSummary> summaries;
	summaries.reserve(
		(sizeof(shifts) / sizeof(shifts[0]))
		* (sizeof(frequencies) / sizeof(frequencies[0])));
	const std::string resultsPath = GetResultsPath();
	if (fopen_s(&resultsFile, resultsPath.c_str(), "w") != 0)
	{
		resultsFile = nullptr;
		std::printf("Warning: could not write results to %s\n\n", resultsPath.c_str());
	}

	PrintResult("Drop Pedal observable content-delay test\n");
	PrintResult("Production shifter, %s, %.0f Hz, %u-frame callbacks, observable envelope transitions\n\n",
		BUILD_ARCHITECTURE,
		SAMPLE_RATE,
		CALLBACK_FRAMES);
	PrintResult("Callback deadline: %.2f microseconds\n\n", CALLBACK_DEADLINE_MICROSECONDS);
	PrintResult("%-10s %6s | %9s %9s %9s | %8s | %s\n",
		"Input",
		"Shift",
		"Median",
		"P95",
		"Maximum",
		"Min edge",
		"Markers");

	for (int semitones : shifts)
	{
		for (size_t frequencyIndex = 0; frequencyIndex < sizeof(frequencies) / sizeof(frequencies[0]); ++frequencyIndex)
		{
			const MeasurementSummary summary = MeasureLatency(frequencies[frequencyIndex], semitones);
			summaries.push_back(summary);
			PrintResult("%-10s %+6d | %7.2fms %7.2fms %7.2fms | %8.4f | %d\n",
				names[frequencyIndex],
				semitones,
				FramesToMilliseconds(summary.latency.medianFrames),
				FramesToMilliseconds(summary.latency.percentile95Frames),
				FramesToMilliseconds(summary.latency.maximumFrames),
				summary.latency.minimumStrengthRatio,
				summary.latency.transitionCount);
		}
		PrintResult("\n");
	}

	PrintResult("CPU processing time per 128-frame callback\n");
	PrintResult("%-10s %6s | %9s %9s %9s %9s | %9s | %s\n",
		"Input",
		"Shift",
		"Average",
		"P95",
		"P99",
		"Maximum",
		"Max deadline",
		"Misses/Calls");
	size_t summaryIndex = 0;
	for (int semitones : shifts)
	{
		for (size_t frequencyIndex = 0; frequencyIndex < sizeof(frequencies) / sizeof(frequencies[0]); ++frequencyIndex)
		{
			const MeasurementSummary& summary = summaries[summaryIndex++];
			PrintResult("%-10s %+6d | %7.2fus %7.2fus %7.2fus %7.2fus | %8.2f%% | %d/%d\n",
				names[frequencyIndex],
				semitones,
				summary.processing.averageMicroseconds,
				summary.processing.percentile95Microseconds,
				summary.processing.percentile99Microseconds,
				summary.processing.maximumMicroseconds,
				summary.processing.maximumDeadlinePercent,
				summary.processing.deadlineMissCount,
				summary.processing.callbackCount);
		}
		PrintResult("\n");
	}

	if (resultsFile != nullptr)
	{
		std::fclose(resultsFile);
		resultsFile = nullptr;
		std::printf("Results written to:\n%s\n", resultsPath.c_str());
	}

	WaitIfLaunchedFromExplorer();

	return 0;
}
