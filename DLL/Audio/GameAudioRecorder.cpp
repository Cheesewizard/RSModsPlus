#include "stdafx.h"
#include "GameAudioRecorder.hpp"

namespace Audio
{
	GameAudioRecorder::~GameAudioRecorder()
	{
		Close();
	}

	HRESULT GameAudioRecorder::Open(const std::filesystem::path& directory, uint32_t maximumFrames)
	{
		if (writer.joinable() || file != INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
		if (maximumFrames == 0 || maximumFrames > 48000) return E_INVALIDARG;
		std::error_code directoryError;
		std::filesystem::create_directories(directory, directoryError);
		if (directoryError) return HRESULT_FROM_WIN32(directoryError.value());
		SYSTEMTIME time{};
		GetLocalTime(&time);
		wchar_t name[128];
		static std::atomic<uint32_t> sequence{ 0 };
		swprintf_s(name, L"Rocksmith-%04u%02u%02u-%02u%02u%02u-%u-%u.wav", time.wYear, time.wMonth,
			time.wDay, time.wHour, time.wMinute, time.wSecond, GetCurrentProcessId(), sequence.fetch_add(1));
		recordingPath = (directory / name).wstring();
		file = CreateFileW(recordingPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
		packetLimit = maximumFrames;
		queue.Initialize(maximumFrames * 2 * sizeof(int16_t), std::max<uint32_t>(4, 96000 / maximumFrames));
		wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!wakeEvent) { const HRESULT result = HRESULT_FROM_WIN32(GetLastError()); Close(); return result; }
		dataBytes = 0;
		error.store(S_OK);
		stopping.store(false);
		if (!WriteHeader()) { const HRESULT result = HRESULT_FROM_WIN32(GetLastError()); Close(); return result; }
		writer = std::thread(&GameAudioRecorder::WritePackets, this);
		LOG_INFO("(AUDIO ROUTING) Recording game audio to " << (directory / name).string() << std::endl);
		return S_OK;
	}

	void GameAudioRecorder::Submit(const float* stereo, uint32_t frames) noexcept
	{
		if (!wakeEvent || FAILED(error.load(std::memory_order_relaxed))) return;
		if (!stereo || frames > packetLimit) { error.store(E_INVALIDARG); return; }
		auto* destination = reinterpret_cast<int16_t*>(queue.BeginWrite());
		if (!destination)
		{
			error.store(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
			SetEvent(wakeEvent);
			return;
		}
		for (uint32_t sample = 0; sample < frames * 2; ++sample)
		{
			const float value = std::isfinite(stereo[sample]) ? std::clamp(stereo[sample], -1.0f, 1.0f) : 0.0f;
			destination[sample] = static_cast<int16_t>(std::lround(value * 32767.0f));
		}
		queue.CommitWrite(frames * 2 * sizeof(int16_t));
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
