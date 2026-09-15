#include "stdafx.h"
#include "GameAudioRecorder.hpp"

namespace Audio
{
	GameAudioRecorder::~GameAudioRecorder()
	{
		Close();
	}

	HRESULT GameAudioRecorder::Open(const std::filesystem::path& directory, uint32_t maximumFrames, const wchar_t* label)
	{
		if (writer.joinable() || file != INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
		if (maximumFrames == 0 || maximumFrames > 48000) return E_INVALIDARG;
		std::error_code directoryError;
		std::filesystem::create_directories(directory, directoryError);
		if (directoryError) return HRESULT_FROM_WIN32(directoryError.value());
		SYSTEMTIME time{};
		GetLocalTime(&time);
		wchar_t name[160];
		static std::atomic<uint32_t> sequence{ 0 };
		const wchar_t* separator = (label && label[0]) ? L"-" : L"";
		swprintf_s(name, L"Rocksmith-%04u%02u%02u-%02u%02u%02u-%u-%u%ls%ls.wav", time.wYear, time.wMonth,
			time.wDay, time.wHour, time.wMinute, time.wSecond, GetCurrentProcessId(), sequence.fetch_add(1),
			separator, (label ? label : L""));
		recordingPath = (directory / name).wstring();
		file = CreateFileW(recordingPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
		packetLimit = maximumFrames;
		// Buffer enough packets to ride out disk contention (e.g. simultaneous video capture) without
		// overrunning. Input blocks can be far smaller than maximumFrames (ASIO delivers ~128 frames),
		// and each block consumes one slot, so a slot count tied only to maximumFrames left ~60 ms of
		// headroom - too shallow when video is also writing. Guarantee a deeper floor of packets.
		queue.Initialize(maximumFrames * 2 * sizeof(int16_t), std::max<uint32_t>(256, 96000 / maximumFrames));
		wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!wakeEvent) { const HRESULT result = HRESULT_FROM_WIN32(GetLastError()); Close(); return result; }
		dataBytes = 0;
		recordedFrames.store(0);
		recordingStarted.store(0);
		error.store(S_OK);
		stopping.store(false);
		if (!WriteHeader()) { const HRESULT result = HRESULT_FROM_WIN32(GetLastError()); Close(); return result; }
		writer = std::thread(&GameAudioRecorder::WritePackets, this);
		LOG_INFO("(AUDIO ROUTING) Recording game audio to " << (directory / name).string() << std::endl);
		return S_OK;
	}

	void GameAudioRecorder::Submit(const float* stereo, uint32_t frames) noexcept
	{
		SubmitSamples(stereo, frames, 2);
	}

	void GameAudioRecorder::SubmitMono(const float* mono, uint32_t frames, uint32_t sampleRate) noexcept
	{
		if (sampleRate != 48000) { error.store(AUDCLNT_E_UNSUPPORTED_FORMAT); return; }
		SubmitSamples(mono, frames, 1);
	}

	void GameAudioRecorder::SubmitSamples(const float* samples, uint32_t frames, uint32_t channels) noexcept
	{
		if (!wakeEvent || FAILED(error.load(std::memory_order_relaxed))) return;
		if (!samples || !frames || frames > packetLimit) { error.store(E_INVALIDARG); return; }
		auto* destination = reinterpret_cast<int16_t*>(queue.BeginWrite());
		if (!destination)
		{
			error.store(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
			SetEvent(wakeEvent);
			return;
		}
		for (uint32_t sample = 0; sample < frames * 2; ++sample)
		{
			const float input = samples[channels == 1 ? sample / 2 : sample];
			const float value = std::isfinite(input) ? std::clamp(input, -1.0f, 1.0f) : 0.0f;
			destination[sample] = static_cast<int16_t>(std::lround(value * 32767.0f));
		}
		queue.CommitWrite(frames * 2 * sizeof(int16_t));
		if (!recordingStarted.load())
		{
			FILETIME time{};
			GetSystemTimePreciseAsFileTime(&time);
			recordingStarted.store((static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime);
		}
		recordedFrames.fetch_add(frames);
		SetEvent(wakeEvent);
	}

	bool GameAudioRecorder::WriteHeader()
	{
		uint8_t header[44]{};
		std::memcpy(header, "RIFF", 4);
		std::memcpy(header + 8, "WAVEfmt ", 8);
		std::memcpy(header + 36, "data", 4);
		const uint32_t values[] = { dataBytes + 36, 16, 48000, 192000, dataBytes };
		const size_t offsets[] = { 4, 16, 24, 28, 40 };
		for (size_t index = 0; index < 5; ++index) std::memcpy(header + offsets[index], &values[index], 4);
		const uint16_t format = 1, channels = 2, alignment = 4, bits = 16;
		std::memcpy(header + 20, &format, 2);
		std::memcpy(header + 22, &channels, 2);
		std::memcpy(header + 32, &alignment, 2);
		std::memcpy(header + 34, &bits, 2);
		LARGE_INTEGER position{};
		if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
		DWORD written = 0;
		if (!WriteFile(file, header, sizeof(header), &written, nullptr) || written != sizeof(header)) return false;
		position.QuadPart = 44ull + dataBytes;
		return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE;
	}

	void GameAudioRecorder::WritePackets()
	{
		for (;;)
		{
			WaitForSingleObject(wakeEvent, 1000);
			uint32_t bytes = 0;
			while (const uint8_t* packet = queue.BeginRead(bytes))
			{
				if (dataBytes > UINT32_MAX - 36 - bytes)
				{
					error.store(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
					break;
				}
				DWORD written = 0;
				if (!WriteFile(file, packet, bytes, &written, nullptr) || written != bytes)
				{
					error.store(HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_WRITE_FAULT));
					break;
				}
				dataBytes += bytes;
				queue.CommitRead();
			}
			if (!WriteHeader() && SUCCEEDED(error.load())) error.store(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
			if (FAILED(error.load()) || stopping.load()) break;
		}
		if (FAILED(error.load())) LOG_ERROR("(AUDIO ROUTING) Recording stopped, HRESULT " << std::hex << error.load() << std::dec << std::endl);
		FlushFileBuffers(file);
	}

	void GameAudioRecorder::Close()
	{
		stopping.store(true);
		if (wakeEvent) SetEvent(wakeEvent);
		if (writer.joinable()) writer.join();
		if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; }
		if (wakeEvent) { CloseHandle(wakeEvent); wakeEvent = nullptr; }
	}
}
