#pragma once

#include <Windows.h>
#include <atomic>
#include <filesystem>
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
}
