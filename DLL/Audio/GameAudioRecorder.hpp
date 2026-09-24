#pragma once

#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include "AudioPacketQueue.hpp"

namespace Audio
{
	class GameAudioRecorder
	{
	public:
		~GameAudioRecorder();
		// label tags the filename with the signal it captures ("dry" = pre-processing input
		// the detector reads, "wet" = the processed game output). Empty keeps the old name.
		// takeName is the shared "Rocksmith-YYYY-MM-DD_HH-MM-SS[_N]" stem; empty draws a fresh one.
		HRESULT Open(const std::filesystem::path& directory, uint32_t maximumFrames, const wchar_t* label = L"",
			const std::wstring& takeName = std::wstring());
		static std::wstring NewTakeName(const std::filesystem::path& directory);
		// Rescale the finished file on Close so its mean level matches loudnessReference (a mean
		// square on the 0..1 full-scale range, e.g. the paired wet take's GetMeanSquare()). Peak is
		// kept at or under -1 dBFS. Set before Open; used for the dry take.
		void SetNormalizeOnClose(bool enabled) noexcept { normalizeOnClose = enabled; }
		void SetLoudnessReference(double meanSquare) noexcept { loudnessReference = meanSquare; }
		// Mean square of everything recorded so far, full scale = 1.0 (0 when nothing recorded).
		double GetMeanSquare() const noexcept;
		void Submit(const float* stereo, uint32_t frames) noexcept;
		void SubmitMono(const float* mono, uint32_t frames, uint32_t sampleRate) noexcept;
		uint64_t GetFrames() const noexcept { return recordedFrames.load(); }
		uint64_t GetStarted() const noexcept { return recordingStarted.load(); }
		void Close();
		HRESULT GetError() const noexcept { return error.load(std::memory_order_relaxed); }
		const std::wstring& GetPath() const noexcept { return recordingPath; }

	private:
		void WritePackets();
		bool WriteHeader();
		void SubmitSamples(const float* samples, uint32_t frames, uint32_t channels) noexcept;
		void NormalizeFile();
		std::atomic<uint64_t> recordedFrames{ 0 }, recordingStarted{ 0 };
		AudioPacketQueue queue;
		std::atomic<HRESULT> error{ S_OK };
		std::atomic<bool> stopping{ false };
		std::atomic<int32_t> peakSample{ 0 };
		std::atomic<uint64_t> sumSquares{ 0 }, sampleCount{ 0 };
		bool normalizeOnClose = false;
		double loudnessReference = 0.0;
		HANDLE wakeEvent = nullptr;
		HANDLE file = INVALID_HANDLE_VALUE;
		std::thread writer;
		uint32_t dataBytes = 0;
		uint32_t packetLimit = 0;
		std::wstring recordingPath;
	};

	class RecordingSession
	{
	public:
		~RecordingSession();
		HRESULT Start(const std::filesystem::path& directory, uint32_t wetMaximumFrames);
		void SubmitWet(const float* stereo, uint32_t frames) noexcept;
		void SubmitDry(const float* mono, uint32_t frames, uint32_t sampleRate) noexcept;
		void Stop(std::wstring& wetPath, uint64_t& frames, uint64_t& started);
		bool IsRecording() const noexcept { return wetRecorder != nullptr; }
		HRESULT GetError() const noexcept;
		uint64_t GetFrames() const noexcept;
		uint64_t GetStarted() const noexcept;
		const std::wstring& GetWetPath() const noexcept;

	private:
		std::shared_ptr<GameAudioRecorder> wetRecorder;
		std::shared_ptr<GameAudioRecorder> dryRecorder;
		HRESULT lastError = S_OK;
		bool dryAttached = false;
	};
}
