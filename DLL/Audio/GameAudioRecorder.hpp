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
		HRESULT Open(const std::filesystem::path& directory, uint32_t maximumFrames, const wchar_t* label = L"");
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
		std::atomic<uint64_t> recordedFrames{ 0 }, recordingStarted{ 0 };
		AudioPacketQueue queue;
		std::atomic<HRESULT> error{ S_OK };
		std::atomic<bool> stopping{ false };
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
