#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../../DLL/Audio/ThirdParty/SignalsmithStretch/signalsmith-stretch.h"

namespace
{
	constexpr int CHANNELS = 2;
	constexpr int PROCESS_FRAMES = 8192;
	constexpr float INT16_NORMALIZER = 1.0f / 32768.0f;
	constexpr float INT16_SCALE = 32768.0f;

	struct StereoSignal
	{
		std::vector<float> left;
		std::vector<float> right;
		uint32_t sampleRate = 0;
	};

	struct Comparison
	{
		double normalizedRootMeanSquareError = 0;
		double maximumError = 0;
		double maximumSeamError = 0;
	};

	struct BoundaryContinuity
	{
		double overallRootMeanSquareDelta = 0;
		double boundaryRootMeanSquareDelta = 0;
		double maximumDelta = 0;
		double maximumBoundaryDelta = 0;
	};

	struct PcmComparison
	{
		size_t mismatchCount = 0;
		int maximumDifference = 0;
	};

	class TemporaryMappedPcm final
	{
	public:
		TemporaryMappedPcm(const std::filesystem::path& path, size_t totalFrames)
		{
			try
			{
				fileHandle = CreateFileW(
					path.c_str(),
					GENERIC_READ | GENERIC_WRITE | DELETE,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					nullptr,
					CREATE_ALWAYS,
					FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
					nullptr);
				if (fileHandle == INVALID_HANDLE_VALUE)
				{
					throw std::runtime_error("temporary PCM create failed");
				}

				LARGE_INTEGER endPosition = {};
				endPosition.QuadPart = static_cast<LONGLONG>(
					totalFrames * CHANNELS * sizeof(int16_t));
				if (!SetFilePointerEx(fileHandle, endPosition, nullptr, FILE_BEGIN)
					|| !SetEndOfFile(fileHandle))
				{
					throw std::runtime_error("temporary PCM sizing failed");
				}

				mappingHandle = CreateFileMappingW(
					fileHandle,
					nullptr,
					PAGE_READWRITE,
					0,
					0,
					nullptr);
				if (mappingHandle == nullptr)
				{
					throw std::runtime_error("temporary PCM mapping failed");
				}

				mappedData = static_cast<int16_t*>(MapViewOfFile(
					mappingHandle,
					FILE_MAP_READ | FILE_MAP_WRITE,
					0,
					0,
					0));
				if (mappedData == nullptr)
				{
					throw std::runtime_error("temporary PCM view failed");
				}
			}
			catch (...)
			{
				Close();
				throw;
			}
		}

		~TemporaryMappedPcm()
		{
			Close();
		}

		TemporaryMappedPcm(const TemporaryMappedPcm&) = delete;
		TemporaryMappedPcm& operator=(const TemporaryMappedPcm&) = delete;

		int16_t* Data() const
		{
			return mappedData;
		}

	private:
		void Close()
		{
			if (mappedData != nullptr)
			{
				UnmapViewOfFile(mappedData);
				mappedData = nullptr;
			}
			if (mappingHandle != nullptr)
			{
				CloseHandle(mappingHandle);
				mappingHandle = nullptr;
			}
			if (fileHandle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(fileHandle);
				fileHandle = INVALID_HANDLE_VALUE;
			}
		}

		HANDLE fileHandle = INVALID_HANDLE_VALUE;
		HANDLE mappingHandle = nullptr;
		int16_t* mappedData = nullptr;
	};

	uint16_t ReadUInt16(std::ifstream& stream)
	{
		uint16_t value = 0;
		stream.read(reinterpret_cast<char*>(&value), sizeof(value));
		return value;
	}

	uint32_t ReadUInt32(std::ifstream& stream)
	{
		uint32_t value = 0;
		stream.read(reinterpret_cast<char*>(&value), sizeof(value));
		return value;
	}

	StereoSignal ReadWave(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream) throw std::runtime_error("could not open WAV");

		char riff[4] = {};
		stream.read(riff, sizeof(riff));
		ReadUInt32(stream);
		char wave[4] = {};
		stream.read(wave, sizeof(wave));
		if (memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0)
		{
			throw std::runtime_error("input is not RIFF WAVE");
		}

		StereoSignal result;
		uint32_t dataSize = 0;
		std::streampos dataOffset = 0;
		while (stream && dataOffset == std::streampos(0))
		{
			char chunkId[4] = {};
			stream.read(chunkId, sizeof(chunkId));
			if (stream.gcount() != sizeof(chunkId)) break;
			const uint32_t chunkSize = ReadUInt32(stream);
			const auto chunkStart = stream.tellg();
			if (memcmp(chunkId, "fmt ", 4) == 0)
			{
				const uint16_t format = ReadUInt16(stream);
				const uint16_t channels = ReadUInt16(stream);
				result.sampleRate = ReadUInt32(stream);
				ReadUInt32(stream);
				ReadUInt16(stream);
				const uint16_t bits = ReadUInt16(stream);
				if (format != 1 || channels != CHANNELS || bits != 16)
				{
					throw std::runtime_error("expected stereo 16-bit PCM WAV");
				}
			}
			else if (memcmp(chunkId, "data", 4) == 0)
			{
				dataSize = chunkSize;
				dataOffset = stream.tellg();
				break;
			}

			stream.seekg(chunkStart + static_cast<std::streamoff>(chunkSize + (chunkSize & 1)));
		}

		if (result.sampleRate == 0 || dataSize == 0 || dataSize % 4 != 0)
		{
			throw std::runtime_error("invalid WAV chunks");
		}

		const size_t frames = dataSize / 4;
		std::vector<int16_t> interleaved(frames * CHANNELS);
		stream.seekg(dataOffset);
		stream.read(reinterpret_cast<char*>(interleaved.data()), dataSize);
		if (stream.gcount() != dataSize) throw std::runtime_error("truncated WAV");

		result.left.resize(frames);
		result.right.resize(frames);
		for (size_t frame = 0; frame < frames; frame++)
		{
			result.left[frame] = interleaved[frame * 2] * INT16_NORMALIZER;
			result.right[frame] = interleaved[frame * 2 + 1] * INT16_NORMALIZER;
		}
		return result;
	}

	StereoSignal RenderExact(const StereoSignal& input, size_t start, size_t frames, float semitones)
	{
		signalsmith::stretch::SignalsmithStretch<float> stretch(0);
		stretch.presetDefault(CHANNELS, static_cast<float>(input.sampleRate));
		stretch.setTransposeSemitones(semitones);

		StereoSignal output;
		output.sampleRate = input.sampleRate;
		output.left.resize(frames);
		output.right.resize(frames);
		const float* inputChannels[] = { input.left.data() + start, input.right.data() + start };
		float* outputChannels[] = { output.left.data(), output.right.data() };
		if (!stretch.exact(inputChannels, static_cast<int>(frames), outputChannels, static_cast<int>(frames)))
		{
			throw std::runtime_error("Signalsmith exact render rejected the buffer");
		}
		return output;
	}

	StereoSignal RenderProgressive(const StereoSignal& input, float semitones, size_t stopAfterFrames = SIZE_MAX)
	{
		signalsmith::stretch::SignalsmithStretch<float> stretch(0);
		stretch.presetDefault(CHANNELS, static_cast<float>(input.sampleRate));
		stretch.setTransposeSemitones(semitones);

		const size_t seekFrames = static_cast<size_t>(stretch.outputSeekLength(1.0f));
		const float* seekChannels[] = { input.left.data(), input.right.data() };
		stretch.outputSeek(seekChannels, static_cast<int>(seekFrames));

		const size_t requestedFrames = std::min(input.left.size(), stopAfterFrames);
		StereoSignal output;
		output.sampleRate = input.sampleRate;
		output.left.resize(requestedFrames);
		output.right.resize(requestedFrames);
		size_t inputOffset = seekFrames;
		size_t outputOffset = 0;
		while (outputOffset < requestedFrames && inputOffset < input.left.size())
		{
			const size_t frameCount = std::min({
				static_cast<size_t>(PROCESS_FRAMES),
				input.left.size() - inputOffset,
				requestedFrames - outputOffset
			});
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
			stretch.process(inputChannels, static_cast<int>(frameCount), outputChannels, static_cast<int>(frameCount));
			inputOffset += frameCount;
			outputOffset += frameCount;
		}

		if (outputOffset < requestedFrames)
		{
			const size_t flushFrames = requestedFrames - outputOffset;
			float* outputChannels[] =
			{
				output.left.data() + outputOffset,
				output.right.data() + outputOffset
			};
			stretch.flush(outputChannels, static_cast<int>(flushFrames), 1.0f);
		}
		return output;
	}

	int16_t FloatToInt16(float value)
	{
		return static_cast<int16_t>(std::lround(
			std::clamp(value * INT16_SCALE, -32768.0f, 32767.0f)));
	}

	void RenderProgressiveToMappedPcm(
		const StereoSignal& input,
		float semitones,
		int16_t* mappedData)
	{
		if (mappedData == nullptr) throw std::invalid_argument("mapped PCM is missing");

		signalsmith::stretch::SignalsmithStretch<float> stretch(0);
		stretch.presetDefault(CHANNELS, static_cast<float>(input.sampleRate));
		stretch.setTransposeSemitones(semitones);

		const size_t seekFrames = static_cast<size_t>(stretch.outputSeekLength(1.0f));
		if (input.left.size() < seekFrames)
		{
			throw std::runtime_error("input is too short for progressive alignment");
		}

		const float* seekChannels[] = { input.left.data(), input.right.data() };
		stretch.outputSeek(seekChannels, static_cast<int>(seekFrames));

		const size_t bufferFrames = std::max(static_cast<size_t>(PROCESS_FRAMES), seekFrames);
		std::vector<float> outputLeft(bufferFrames);
		std::vector<float> outputRight(bufferFrames);
		size_t inputOffset = seekFrames;
		size_t outputOffset = 0;
		const auto writeFrames = [&](size_t frameCount)
		{
			for (size_t frame = 0; frame < frameCount; frame++)
			{
				mappedData[(outputOffset + frame) * CHANNELS] = FloatToInt16(outputLeft[frame]);
				mappedData[(outputOffset + frame) * CHANNELS + 1] = FloatToInt16(outputRight[frame]);
			}
			outputOffset += frameCount;
		};

		while (inputOffset < input.left.size())
		{
			const size_t frameCount = std::min(
				static_cast<size_t>(PROCESS_FRAMES),
				input.left.size() - inputOffset);
			const float* inputChannels[] =
			{
				input.left.data() + inputOffset,
				input.right.data() + inputOffset
			};
			float* outputChannels[] = { outputLeft.data(), outputRight.data() };
			stretch.process(
				inputChannels,
				static_cast<int>(frameCount),
				outputChannels,
				static_cast<int>(frameCount));
			writeFrames(frameCount);
			inputOffset += frameCount;
		}

		float* flushChannels[] = { outputLeft.data(), outputRight.data() };
		stretch.flush(flushChannels, static_cast<int>(seekFrames), 1.0f);
		writeFrames(std::min(seekFrames, input.left.size() - outputOffset));
		if (outputOffset != input.left.size())
		{
			throw std::runtime_error("mapped progressive render produced the wrong frame count");
		}
	}

	PcmComparison CompareMappedPcm(const StereoSignal& expected, const int16_t* mappedData)
	{
		PcmComparison comparison;
		for (size_t frame = 0; frame < expected.left.size(); frame++)
		{
			const int16_t expectedSamples[] =
			{
				FloatToInt16(expected.left[frame]),
				FloatToInt16(expected.right[frame])
			};
			for (int channel = 0; channel < CHANNELS; channel++)
			{
				const int difference = std::abs(
					static_cast<int>(mappedData[frame * CHANNELS + channel])
					- expectedSamples[channel]);
				if (difference == 0) continue;

				comparison.mismatchCount++;
				comparison.maximumDifference = std::max(
					comparison.maximumDifference,
					difference);
			}
		}
		return comparison;
	}

	BoundaryContinuity MeasureBoundaries(const StereoSignal& signal, size_t boundaryFrames)
	{
		BoundaryContinuity result;
		double overallSquaredDelta = 0;
		double boundarySquaredDelta = 0;
		size_t overallSamples = 0;
		size_t boundarySamples = 0;
		for (size_t frame = 1; frame < signal.left.size(); frame++)
		{
			const bool isBoundary = frame % boundaryFrames == 0;
			for (const auto* channel : { &signal.left, &signal.right })
			{
				const double delta = std::abs((*channel)[frame] - (*channel)[frame - 1]);
				overallSquaredDelta += delta * delta;
				result.maximumDelta = std::max(result.maximumDelta, delta);
				overallSamples++;
				if (!isBoundary) continue;

				boundarySquaredDelta += delta * delta;
				result.maximumBoundaryDelta = std::max(result.maximumBoundaryDelta, delta);
				boundarySamples++;
			}
		}

		result.overallRootMeanSquareDelta = std::sqrt(overallSquaredDelta / overallSamples);
		result.boundaryRootMeanSquareDelta = std::sqrt(boundarySquaredDelta / boundarySamples);
		return result;
	}

	StereoSignal RenderSegments(
		const StereoSignal& input,
		float semitones,
		size_t segmentFrames,
		size_t contextFrames,
		int threadCount)
	{
		StereoSignal output;
		output.sampleRate = input.sampleRate;
		output.left.resize(input.left.size());
		output.right.resize(input.right.size());
		const size_t segmentCount = (input.left.size() + segmentFrames - 1) / segmentFrames;
		std::atomic<size_t> nextSegment(0);
		std::vector<std::thread> workers;
		for (int threadIndex = 0; threadIndex < threadCount; threadIndex++)
		{
			workers.emplace_back([&]()
			{
				for (;;)
				{
					const size_t segment = nextSegment.fetch_add(1);
					if (segment >= segmentCount) return;
					const size_t outputStart = segment * segmentFrames;
					const size_t outputEnd = std::min(input.left.size(), outputStart + segmentFrames);
					const size_t renderStart = outputStart > contextFrames ? outputStart - contextFrames : 0;
					const size_t renderEnd = std::min(input.left.size(), outputEnd + contextFrames);
					const auto rendered = RenderExact(input, renderStart, renderEnd - renderStart, semitones);
					const size_t copyOffset = outputStart - renderStart;
					const size_t copyFrames = outputEnd - outputStart;
					std::copy_n(rendered.left.data() + copyOffset, copyFrames, output.left.data() + outputStart);
					std::copy_n(rendered.right.data() + copyOffset, copyFrames, output.right.data() + outputStart);
				}
			});
		}

		for (auto& worker : workers)
		{
			worker.join();
		}
		return output;
	}

	Comparison Compare(
		const StereoSignal& reference,
		const StereoSignal& candidate,
		size_t segmentFrames,
		size_t seamRadius)
	{
		double errorPower = 0;
		double referencePower = 0;
		double maximumError = 0;
		double maximumSeamError = 0;
		for (size_t frame = 0; frame < reference.left.size(); frame++)
		{
			const double errors[] =
			{
				candidate.left[frame] - reference.left[frame],
				candidate.right[frame] - reference.right[frame]
			};
			const double references[] = { reference.left[frame], reference.right[frame] };
			const size_t segmentOffset = frame % segmentFrames;
			const bool isNearSeam = segmentOffset < seamRadius || segmentFrames - segmentOffset <= seamRadius;
			for (int channel = 0; channel < CHANNELS; channel++)
			{
				errorPower += errors[channel] * errors[channel];
				referencePower += references[channel] * references[channel];
				maximumError = std::max(maximumError, std::abs(errors[channel]));
				if (isNearSeam)
				{
					maximumSeamError = std::max(maximumSeamError, std::abs(errors[channel]));
				}
			}
		}

		return {
			std::sqrt(errorPower / std::max(referencePower, 1e-30)),
			maximumError,
			maximumSeamError
		};
	}

}

int main(int argumentCount, char** arguments)
{
	if (argumentCount != 2)
	{
		std::fprintf(stderr, "Usage: speaker_pipeline_benchmark.exe <stereo 16-bit PCM WAV>\n");
		return 2;
	}

	try
	{
		const auto input = ReadWave(arguments[1]);
		const double duration = input.left.size() / static_cast<double>(input.sampleRate);
		std::printf(
			"Input: %.2f seconds, %u Hz, %zu frames, %u logical processors\n",
			duration,
			input.sampleRate,
			input.left.size(),
			GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

		const auto exactStarted = std::chrono::steady_clock::now();
		const auto exact = RenderExact(input, 0, input.left.size(), 1.0f);
		const double exactSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - exactStarted).count();
		std::printf("Signalsmith exact monolithic       %8.3f s (%6.1fx real-time)\n", exactSeconds, duration / exactSeconds);

		const size_t prefixFrames = static_cast<size_t>(input.sampleRate) * 10;
		const auto prefixStarted = std::chrono::steady_clock::now();
		const auto prefix = RenderProgressive(input, 1.0f, prefixFrames);
		const double prefixSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - prefixStarted).count();
		std::printf("Progressive first 10 seconds      %8.3f s\n", prefixSeconds);

		const auto progressiveStarted = std::chrono::steady_clock::now();
		const auto progressive = RenderProgressive(input, 1.0f);
		const double progressiveSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - progressiveStarted).count();
		const auto progressiveRepeat = RenderProgressive(input, 1.0f);
		const auto progressiveComparison = Compare(progressive, progressiveRepeat, input.left.size(), 0);
		const auto oneShotComparison = Compare(exact, progressive, input.left.size(), 0);
		const auto continuity = MeasureBoundaries(progressive, PROCESS_FRAMES);
		std::printf(
			"Progressive complete              %8.3f s | %zu exact-position frames\n",
			progressiveSeconds,
			progressive.left.size());
		std::printf(
			"Progressive deterministic                    | NRMSE %.8f max %.8f\n",
			progressiveComparison.normalizedRootMeanSquareError,
			progressiveComparison.maximumError);
		std::printf(
			"Streaming call boundaries                    | RMS %.6f vs all %.6f | max %.6f vs all %.6f\n",
			continuity.boundaryRootMeanSquareDelta,
			continuity.overallRootMeanSquareDelta,
			continuity.maximumBoundaryDelta,
			continuity.maximumDelta);
		std::printf(
			"One-call exact waveform delta                 | NRMSE %.6f (silence-block policy differs)\n",
			oneShotComparison.normalizedRootMeanSquareError);

		const size_t intervalFrames = static_cast<size_t>(input.sampleRate) * 30 / 1000;
		const size_t segmentFrames = intervalFrames * 256;
		const size_t contextFrames = intervalFrames * 32;
		for (const int threadCount : { 1, 2, 4, 8 })
		{
			const auto started = std::chrono::steady_clock::now();
			const auto segmented = RenderSegments(input, 1.0f, segmentFrames, contextFrames, threadCount);
			const double seconds = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - started).count();
			const auto comparison = Compare(exact, segmented, segmentFrames, intervalFrames);
			std::printf(
				"Segmented %d thread(s)             %8.3f s | NRMSE %.6f max %.6f seam %.6f\n",
				threadCount,
				seconds,
				comparison.normalizedRootMeanSquareError,
				comparison.maximumError,
				comparison.maximumSeamError);
		}

		const auto temporaryPath = std::filesystem::temp_directory_path()
			/ (L"rsmods-speaker-pipeline-" + std::to_wstring(GetCurrentProcessId()) + L".pcm");
		TemporaryMappedPcm mappedPcm(temporaryPath, input.left.size());
		const auto mappedStarted = std::chrono::steady_clock::now();
		RenderProgressiveToMappedPcm(input, 1.0f, mappedPcm.Data());
		const double mappedSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - mappedStarted).count();
		const auto mappedComparison = CompareMappedPcm(progressive, mappedPcm.Data());
		std::printf(
			"Progressive direct mapped PCM    %8.3f s | %zu mismatches, max %d LSB\n",
			mappedSeconds,
			mappedComparison.mismatchCount,
			mappedComparison.maximumDifference);
		if (mappedComparison.mismatchCount != 0)
		{
			throw std::runtime_error("mapped PCM differs from the progressive render");
		}
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "%s\n", exception.what());
		return 1;
	}
}
