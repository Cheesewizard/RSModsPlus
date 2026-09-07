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
		HRESULT Open(const std::filesystem::path& directory, uint32_t maximumFrames);
		void Submit(const float* stereo, uint32_t frames) noexcept;
		void Close();
		HRESULT GetError() const noexcept { return error.load(std::memory_order_relaxed); }
		const std::wstring& GetPath() const noexcept { return recordingPath; }

	private:
		void WritePackets();
		bool WriteHeader();
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
