#include "stdafx.h"
#include "SharedOutput.hpp"
#include "AudioPacketQueue.hpp"
#include "GameAudioRecorder.hpp"
#include "AudioControl.hpp"
#include "../Mods/VolumeControl.hpp"
#include <avrt.h>
#include <functional>
#include <memory>
#include <array>

#pragma comment(lib, "avrt.lib")

namespace Audio::SharedOutput
{
	UINT32 ReadSavedPeriod(const std::wstring& endpointId)
	{
		wchar_t executable[MAX_PATH]{};
		GetModuleFileNameW(nullptr, executable, MAX_PATH);
		const auto path = std::filesystem::path(executable).parent_path() / L"AudioRouting.ini";
		return GetPrivateProfileIntW((L"Output buffer " + endpointId).c_str(), L"PeriodFrames", 0, path.c_str());
	}

	bool IsEnginePeriod(UINT32 value, UINT32 minimum, UINT32 fundamental, UINT32 maximum)
	{
		return fundamental && value >= minimum && value <= maximum && value % fundamental == 0;
	}

	HRESULT PrimeExtraBuffer(IAudioRenderClient* client, UINT32 selected, UINT32 minimum)
	{
		if (selected <= minimum) return S_OK;
		BYTE* data = nullptr;
		const HRESULT result = client->GetBuffer(selected - minimum, &data);
		return FAILED(result) ? result : client->ReleaseBuffer(selected - minimum, AUDCLNT_BUFFERFLAGS_SILENT);
	}

	using EndpointFactory = std::function<HRESULT(const std::wstring&, IAudioClient3**)>;

	HRESULT OpenEndpoint(const std::wstring& endpointId, IAudioClient3** client)
	{
		IMMDeviceEnumerator* enumerator = nullptr;
		HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
			__uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
		if (FAILED(result)) return result;
		IMMDevice* endpoint = nullptr;
		result = enumerator->GetDevice(endpointId.c_str(), &endpoint);
		enumerator->Release();
		if (FAILED(result)) return result;
		IMMEndpoint* direction = nullptr;
		result = endpoint->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(&direction));
		EDataFlow flow = eAll;
		if (SUCCEEDED(result)) { result = direction->GetDataFlow(&flow); direction->Release(); }
		if (SUCCEEDED(result) && flow != eRender) result = E_INVALIDARG;
		if (SUCCEEDED(result)) result = endpoint->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client));
		endpoint->Release();
		return result;
	}

	bool IsGameFormat(const WAVEFORMATEX* format)
	{
		if (!format || format->nChannels != 2 || format->nSamplesPerSec != 48000) return false;
		WORD tag = format->wFormatTag;
		if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22)
		{
			const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
			if (extended->dwChannelMask != 0 && extended->dwChannelMask != (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)) return false;
			if (extended->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
			else if (extended->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
			else return false;
			if (extended->Samples.wValidBitsPerSample != format->wBitsPerSample) return false;
		}
		return ((tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32)
			|| (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16))
			&& format->nBlockAlign == format->nChannels * format->wBitsPerSample / 8
			&& format->nAvgBytesPerSec == format->nSamplesPerSec * format->nBlockAlign;
	}

	class OutputSession
	{
	public:
		explicit OutputSession(EndpointFactory factory = OpenEndpoint) : endpointFactory(std::move(factory))
		{
			commandEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			completedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			engineEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			packetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			gameTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			wakeFrequency = frequency.QuadPart;
			if (!commandEvent || !completedEvent || !engineEvent || !packetEvent || !gameTimer || !wakeFrequency)
			{
				CloseEvents();
				throw std::runtime_error("Could not create output synchronization events");
			}
			try { worker = std::thread(&OutputSession::Run, this); }
			catch (...) { CloseEvents(); throw; }
		}

		~OutputSession()
		{
			control.Stop();
			Invoke([this]() { quitting = true; return S_OK; });
			worker.join();
			CloseEvents();
		}

		HRESULT Invoke(std::function<HRESULT()> action)
		{
			std::lock_guard<std::mutex> guard(commandMutex);
			command = std::move(action);
			SetEvent(commandEvent);
			WaitForSingleObject(completedEvent, INFINITE);
			return commandResult;
		}

		HRESULT Open(const std::wstring& endpointId)
		{
			savedPeriod = ReadSavedPeriod(endpointId);
			activeEndpoint = endpointId;
			return Invoke([this, &endpointId]()
			{
				if (FAILED(comResult)) return comResult;
				HRESULT result = endpointFactory(endpointId, &backend);
				if (FAILED(result)) return result;
				return backend->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
			});
		}

		HRESULT Initialize(const WAVEFORMATEX* format, DWORD flags, LPCGUID sessionId)
		{
			if (!format) return E_POINTER;
			if (!IsGameFormat(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
			if (!(flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK)) return E_INVALIDARG;
			if (initialized.load()) return AUDCLNT_E_ALREADY_INITIALIZED;
			return Invoke([this, format, sessionId]()
			{
				gameFormat = *format;
				WAVEFORMATEX engineFormat{ WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0 };
				UINT32 defaultFrames = 0, fundamental = 0, minimum = 0, maximum = 0;
				HRESULT result = backend->GetSharedModeEnginePeriod(&engineFormat, &defaultFrames, &fundamental, &minimum, &maximum);
				if (FAILED(result)) return result;
				if (minimum == 0 || minimum > 48000) return E_UNEXPECTED;
				const UINT32 selected = savedPeriod ? savedPeriod : minimum;
				if (!IsEnginePeriod(selected, minimum, fundamental, maximum)) return E_INVALIDARG;
				result = backend->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, selected, &engineFormat, sessionId);
				if (FAILED(result)) return result;
				result = backend->SetEventHandle(engineEvent);
				if (FAILED(result)) return result;
				result = backend->GetBufferSize(&backendFrames);
				if (FAILED(result)) return result;
				if (backendFrames < selected) return E_UNEXPECTED;
				result = backend->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
				if (FAILED(result)) return result;
				result = backend->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&clock));
				if (FAILED(result)) return result;
				result = clock->GetFrequency(&clockFrequency);
				if (FAILED(result)) return result;
				if (!clockFrequency) return E_UNEXPECTED;
				result = PrimeExtraBuffer(render, selected, minimum);
				if (FAILED(result)) return result;
				backendFrequency = clockFrequency;
				result = backend->GetStreamLatency(&streamLatency);
				if (FAILED(result)) return result;
				periodFrames = minimum;
				gamePacketFrames.store(periodFrames);
				backendPeriodFrames = selected;
				engineMinimum = minimum; engineFundamental = fundamental; engineMaximum = maximum;
				queue.Initialize(periodFrames * gameFormat.nBlockAlign, 2);
				converted.resize(periodFrames * 2);
				initialized.store(true, std::memory_order_release);
				LOG_INFO("(AUDIO ROUTING) Shared output: 48 kHz stereo, game block " << periodFrames
					<< " frames; Windows period " << backendPeriodFrames << " frames; live recording ready" << std::endl);
				return S_OK;
			});
		}

		HRESULT StartControl()
		{
			return control.Start([this](const ControlRequest& request) { return HandleControl(request); });
		}

		ControlResponse HandleControl(const ControlRequest& request)
		{
			ControlResponse response;
			if (!initialized.load()) { response.result = AUDCLNT_E_NOT_INITIALIZED; return response; }
			if (request.operation == 2)
			{
				bool alreadyRecording = false;
				Invoke([&]() { alreadyRecording = recorder != nullptr; return S_OK; });
				if (alreadyRecording) response.result = HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
				else
				{
					auto next = std::make_shared<GameAudioRecorder>();
					const std::filesystem::path directory(request.value);
					response.result = directory.is_absolute() ? next->Open(directory, periodFrames) : E_INVALIDARG;
					if (SUCCEEDED(response.result)) Invoke([&]() { recorder = next; recordedFrames = 0; recordingStarted = 0; lastRecordingPath = next->GetPath(); recordingError = S_OK; return S_OK; });
				}
			}
			else if (request.operation == 3)
			{
				std::shared_ptr<GameAudioRecorder> finished;
				Invoke([&]() { finished.swap(recorder); return S_OK; });
				if (finished)
				{
					finished->Close();
					response.result = finished->GetError();
					Invoke([&]() { lastRecordingPath = finished->GetPath(); recordingError = finished->GetError(); return S_OK; });
				}
			}
			else if (request.operation == 4) response.result = SwitchOutput(request.value);
			else if (request.operation == 6)
			{
				wchar_t* end = nullptr;
				const unsigned long period = wcstoul(request.value, &end, 10);
				bool recording = false;
				std::wstring endpoint;
				Invoke([&]() { recording = recorder != nullptr; endpoint = activeEndpoint; return S_OK; });
				if (recording) response.result = HRESULT_FROM_WIN32(ERROR_BUSY);
				else if (end == request.value || *end != L'\n' || !end[1] || !period || period > 48000) response.result = E_INVALIDARG;
				else if (endpoint != end + 1) response.result = HRESULT_FROM_WIN32(ERROR_RETRY);
				else response.result = SwitchOutput(endpoint, static_cast<UINT32>(period));
			}
			else if (request.operation >= 7 && request.operation <= 13)
			{
				unsigned int volume = 0;
				bool valid = request.value[0] != 0;
				for (const wchar_t* digit = request.value; *digit && valid; ++digit)
				{
					valid = *digit >= L'0' && *digit <= L'9';
					if (valid) volume = volume * 10 + (*digit - L'0');
					valid = valid && volume <= 100;
				}
				response.result = !valid ? E_INVALIDARG
					: VolumeControl::SetPlaybackVolume(request.operation - 7, static_cast<float>(volume)) ? S_OK : E_FAIL;
			}
			else if (request.operation != 1 && request.operation != 5) response.result = E_INVALIDARG;
			for (unsigned int channel = 0; channel < 7; ++channel)
			{
				if (!VolumeControl::GetPlaybackVolume(channel, response.volumes[channel]))
				{
					response.mixerError = E_FAIL;
					response.volumes[channel] = -1.f;
				}
			}
			Invoke([&]()
			{
				response.outputError = streamError.load();
				response.recordingError = recorder ? recorder->GetError() : recordingError;
				response.recording = recorder && SUCCEEDED(response.recordingError) ? 1 : 0;
				response.recordedFrames = recordedFrames;
				response.recordingStarted = recordingStarted;
				response.peak = peak;
				wcsncpy_s(response.file, recorder ? recorder->GetPath().c_str() : lastRecordingPath.c_str(), _TRUNCATE);
				wcsncpy_s(response.endpoint, activeEndpoint.c_str(), _TRUNCATE);
				if (request.operation == 5)
				{
					response.recording = running ? 1 : 0;
					response.recordedFrames = submittedFrames;
					response.recordingStarted = clockPosition.load();
					const std::wstring trace = L"starts=" + std::to_wstring(startCalls.load())
						+ L" stops=" + std::to_wstring(stopCalls.load())
						+ L" lifecycleHr=" + std::to_wstring(lastLifecycleResult.load())
						+ L" acquireCalls=" + std::to_wstring(acquireCalls.load())
						+ L" acquireFailures=" + std::to_wstring(acquireFailures.load())
						+ L" acquireHr=" + std::to_wstring(lastAcquireResult.load())
						+ L" requestedFrames=" + std::to_wstring(lastRequestedFrames.load())
						+ L" releaseCalls=" + std::to_wstring(releaseCalls.load())
						+ L" releaseHr=" + std::to_wstring(lastReleaseResult.load())
						+ L" queuePackets=" + std::to_wstring(queue.Count())
						+ L" padding=" + std::to_wstring(lastPadding)
						+ L" repeatedGameBlocks=" + std::to_wstring(repeatedGameBlocks.load())
						+ L" inspectedGameBlocks=" + std::to_wstring(inspectedGameBlocks.load())
						+ L" emptyOutputObservations=" + std::to_wstring(emptyOutputObservations)
						+ L" longestPumpGapMs=" + std::to_wstring(longestPumpGapMs);
					const std::wstring tuning = L" engineMinimum=" + std::to_wstring(engineMinimum)
						+ L" engineFundamental=" + std::to_wstring(engineFundamental)
						+ L" engineMaximum=" + std::to_wstring(engineMaximum)
						+ L" enginePeriod=" + std::to_wstring(backendPeriodFrames);
					wcsncpy_s(response.file, (trace + tuning).c_str(), _TRUNCATE);
				}
				return S_OK;
			});
			return response;
		}

		HRESULT SwitchOutput(const std::wstring& endpointId, UINT32 requestedPeriod = 0)
		{
			if (endpointId.empty()) return E_INVALIDARG;
			const UINT32 desired = requestedPeriod ? requestedPeriod : ReadSavedPeriod(endpointId);
			return Invoke([&]()
			{
				IAudioClient3* next = nullptr;
				IAudioRenderClient* nextRender = nullptr;
				IAudioClock* nextClock = nullptr;
				UINT32 nextFrames = 0;
				UINT64 nextFrequency = 0;
				HRESULT result = endpointFactory(endpointId, &next);
				WAVEFORMATEX engineFormat{ WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0 };
				UINT32 normal = 0, fundamental = 0, minimum = 0, maximum = 0;
				if (SUCCEEDED(result)) result = next->GetSharedModeEnginePeriod(&engineFormat, &normal, &fundamental, &minimum, &maximum);
				if (SUCCEEDED(result) && (!minimum || !fundamental || minimum > 48000)) result = E_UNEXPECTED;
				const UINT32 selected = desired ? desired : minimum;
				if (SUCCEEDED(result) && !IsEnginePeriod(selected, minimum, fundamental, maximum)) result = E_INVALIDARG;
				if (SUCCEEDED(result)) result = next->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, selected, &engineFormat, nullptr);
				if (SUCCEEDED(result)) result = next->SetEventHandle(engineEvent);
				if (SUCCEEDED(result)) result = next->GetBufferSize(&nextFrames);
				if (SUCCEEDED(result) && nextFrames < std::max(periodFrames, selected)) result = AUDCLNT_E_BUFFER_SIZE_ERROR;
				if (SUCCEEDED(result)) result = next->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&nextRender));
				if (SUCCEEDED(result)) result = next->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&nextClock));
				if (SUCCEEDED(result)) result = nextClock->GetFrequency(&nextFrequency);
				if (SUCCEEDED(result) && !nextFrequency) result = E_UNEXPECTED;
				if (SUCCEEDED(result)) result = PrimeExtraBuffer(nextRender, selected, minimum);
				if (SUCCEEDED(result) && running) result = next->Start();
				if (FAILED(result))
				{
					if (nextClock) nextClock->Release();
					if (nextRender) nextRender->Release();
					if (next) next->Release();
					LOG_ERROR("(AUDIO ROUTING) Output switch rejected, HRESULT " << std::hex << result << std::dec << std::endl);
					return result;
				}
				if (running) backend->Stop();
				clock->Release(); render->Release(); backend->Release();
				backend = next; render = nextRender; clock = nextClock; backendFrames = nextFrames;
				backendPeriodFrames = selected;
				engineMinimum = minimum; engineFundamental = fundamental; engineMaximum = maximum;
				clockOffset = clockPosition.load();
				backendFrequency = nextFrequency;
				activeEndpoint = endpointId;
				streamError.store(S_OK);
				SetEvent(packetEvent);
				SignalGame();
				LOG_INFO("(AUDIO ROUTING) Output changed without reopening the game-facing client" << std::endl);
				return S_OK;
			});
		}

		HRESULT Start()
		{
			startCalls.fetch_add(1);
			if (!initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			const HRESULT started = Invoke([this]()
			{
				if (FAILED(streamError.load())) return streamError.load();
				if (!gameEvent) return AUDCLNT_E_EVENTHANDLE_NOT_SET;
				if (running) return AUDCLNT_E_NOT_STOPPED;
				HRESULT result = Pump();
				if (FAILED(result)) return result;
				result = backend->Start();
				if (SUCCEEDED(result)) { running = true; lastGameWakeTick = 0; SignalGame(); }
				return result;
			});
			lastLifecycleResult.store(started);
			LOG_INFO("(AUDIO ROUTING) Start result " << std::hex << started << std::dec << std::endl);
			return started;
		}

		HRESULT Stop()
		{
			stopCalls.fetch_add(1);
			if (!initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			const HRESULT stopped = Invoke([this]()
			{
				const HRESULT result = backend->Stop();
				if (SUCCEEDED(result))
				{
					running = false;
					CancelWaitableTimer(gameTimer);
					if (gameEvent) ResetEvent(gameEvent);
					gameWakeOutstanding.store(false);
				}
				return result;
			});
			lastLifecycleResult.store(stopped);
			LOG_INFO("(AUDIO ROUTING) Stop result " << std::hex << stopped << std::dec << std::endl);
			return stopped;
		}

		HRESULT Reset()
		{
			if (!initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			if (pendingFrames != 0) return AUDCLNT_E_BUFFER_OPERATION_PENDING;
			return Invoke([this]()
			{
				if (running) return AUDCLNT_E_NOT_STOPPED;
				const HRESULT result = backend->Reset();
				if (SUCCEEDED(result)) { queue.Reset(); clockPosition.store(0); clockOffset = 0; }
				return result;
			});
		}

		HRESULT SetGameEvent(HANDLE eventHandle)
		{
			if (!initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			if (!eventHandle) return E_INVALIDARG;
			return Invoke([this, eventHandle]()
			{
				if (gameEvent) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
				return DuplicateHandle(GetCurrentProcess(), eventHandle, GetCurrentProcess(), &gameEvent,
					0, FALSE, DUPLICATE_SAME_ACCESS) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
			});
		}

		HRESULT GetBuffer(UINT32 frames, BYTE** data)
		{
			acquireCalls.fetch_add(1, std::memory_order_relaxed);
			lastRequestedFrames.store(frames, std::memory_order_relaxed);
			const HRESULT result = AcquireBuffer(frames, data);
			lastAcquireResult.store(result, std::memory_order_relaxed);
			if (FAILED(result)) acquireFailures.fetch_add(1, std::memory_order_relaxed);
			return result;
		}

		HRESULT AcquireBuffer(UINT32 frames, BYTE** data)
		{
			if (!data) return E_POINTER;
			*data = nullptr;
			if (!initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			if (FAILED(streamError.load())) return streamError.load();
			if (pendingFrames) return AUDCLNT_E_OUT_OF_ORDER;
			if (frames == 0) return S_OK;
			if (frames > periodFrames) return AUDCLNT_E_BUFFER_SIZE_ERROR;
			BYTE* packet = queue.BeginWrite();
			if (!packet) return AUDCLNT_E_BUFFER_TOO_LARGE;
			gameWakeOutstanding.store(true);
			gamePacketFrames.store(frames);
			pendingFrames = frames;
			*data = packet;
			return S_OK;
		}

		HRESULT ReleaseBuffer(UINT32 frames, DWORD flags)
		{
			releaseCalls.fetch_add(1, std::memory_order_relaxed);
			const HRESULT result = CommitBuffer(frames, flags);
			lastReleaseResult.store(result, std::memory_order_relaxed);
			return result;
		}

		HRESULT CommitBuffer(UINT32 frames, DWORD flags)
		{
			if (!initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			if (FAILED(streamError.load())) return streamError.load();
			if (frames == 0)
			{
				pendingFrames = 0;
				gameWakeOutstanding.store(false);
				SetEvent(packetEvent);
				return S_OK;
			}
			if (!pendingFrames) return AUDCLNT_E_OUT_OF_ORDER;
			if (frames != pendingFrames) return AUDCLNT_E_INVALID_SIZE;
			if (flags & ~AUDCLNT_BUFFERFLAGS_SILENT) return E_INVALIDARG;
			if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::memset(queue.BeginWrite(), 0, frames * gameFormat.nBlockAlign);
			InspectGameBlocks(queue.BeginWrite(), frames * gameFormat.nBlockAlign);
			queue.CommitWrite(frames * gameFormat.nBlockAlign);
			pendingFrames = 0;
			gameWakeOutstanding.store(false);
			SetEvent(packetEvent);
			return S_OK;
		}

		std::atomic<bool> initialized{ false };
		bool exposeControl = false;
		std::atomic<HRESULT> streamError{ S_OK };
		std::atomic<UINT64> clockPosition{ 0 };
		std::atomic<UINT64> clockQpc{ 0 };
		std::atomic<uint32_t> clockSequence{ 0 };
		UINT64 clockFrequency = 0;
		UINT32 periodFrames = 0;
		REFERENCE_TIME defaultPeriod = 0;
		REFERENCE_TIME minimumPeriod = 0;
		REFERENCE_TIME streamLatency = 0;

	private:
		void InspectGameBlocks(const BYTE* data, UINT32 bytes)
		{
			const UINT32 blockBytes = 128 * gameFormat.nBlockAlign;
			while (bytes)
			{
				const UINT32 copied = std::min(bytes, blockBytes - diagnosticBytes);
				std::memcpy(diagnosticBlock.data() + diagnosticBytes, data, copied);
				diagnosticBytes += copied; data += copied; bytes -= copied;
				if (diagnosticBytes != blockBytes) continue;
				bool nonzero = false;
				for (UINT32 index = 0; index < blockBytes; ++index) nonzero |= diagnosticBlock[index] != 0;
				if (havePreviousBlock && nonzero && std::memcmp(diagnosticBlock.data(), previousBlock.data(), blockBytes) == 0)
					repeatedGameBlocks.fetch_add(1, std::memory_order_relaxed);
				inspectedGameBlocks.fetch_add(1, std::memory_order_relaxed);
				std::memcpy(previousBlock.data(), diagnosticBlock.data(), blockBytes);
				havePreviousBlock = true;
				diagnosticBytes = 0;
			}
		}

		void SignalGame()
		{
			if (!running || !gameEvent) return;
			bool outstanding = false;
			if (!gameWakeOutstanding.compare_exchange_strong(outstanding, true)) return;
			if (queue.Count() >= 2)
			{
				gameWakeOutstanding.store(false);
				return;
			}
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			const LONGLONG interval = wakeFrequency * gamePacketFrames.load() / 48000;
			const LONGLONG due = lastGameWakeTick + interval;
			if (lastGameWakeTick && now.QuadPart < due)
			{
				LARGE_INTEGER delay{};
				delay.QuadPart = -std::max<LONGLONG>(1, (due - now.QuadPart) * 10000000 / wakeFrequency);
				gameWakeOutstanding.store(false);
				if (!SetWaitableTimer(gameTimer, &delay, 0, nullptr, nullptr, FALSE))
				{
					streamError.store(HRESULT_FROM_WIN32(GetLastError()));
					LOG_ERROR("(AUDIO ROUTING) Could not schedule the game audio wake" << std::endl);
					SetEvent(gameEvent);
				}
				return;
			}
			// Keep the wake reserved while the producer holds its uncommitted slot.
			// Preserve the sample-rate cadence through timer jitter, but do not burst
			// through missed periods after a stall.
			lastGameWakeTick = !lastGameWakeTick || now.QuadPart - due >= interval ? now.QuadPart : due;
			SetEvent(gameEvent);
		}

		HRESULT Pump()
		{
			UINT32 padding = 0;
			HRESULT result = backend->GetCurrentPadding(&padding);
			if (FAILED(result)) return result;
			lastPadding = padding;
			const ULONGLONG now = GetTickCount64();
			if (running && submittedFrames)
			{
				if (padding == 0) ++emptyOutputObservations;
				if (lastPumpTick) longestPumpGapMs = std::max(longestPumpGapMs, now - lastPumpTick);
			}
			lastPumpTick = now;
			if (padding > backendFrames) return E_UNEXPECTED;
			uint32_t bytes = 0;
			const uint8_t* packet = queue.BeginRead(bytes);
			const UINT32 packetFrames = bytes / gameFormat.nBlockAlign;
			const UINT32 targetPadding = std::min(backendFrames, std::max(periodFrames, backendPeriodFrames) * 2);
			if (packet && packetFrames > 0 && padding <= targetPadding - packetFrames && backendFrames - padding >= packetFrames)
			{
				const size_t samples = packetFrames * 2;
				if (gameFormat.wBitsPerSample == 32) std::memcpy(converted.data(), packet, samples * sizeof(float));
				else
				{
					const auto* pcm = reinterpret_cast<const int16_t*>(packet);
					for (size_t index = 0; index < samples; ++index) converted[index] = pcm[index] / 32768.0f;
				}
				BYTE* destination = nullptr;
				result = render->GetBuffer(packetFrames, &destination);
				if (FAILED(result)) return result;
				std::memcpy(destination, converted.data(), samples * sizeof(float));
				result = render->ReleaseBuffer(packetFrames, 0);
				if (FAILED(result)) return result;
				submittedFrames += packetFrames;
				peak = 0;
				for (size_t index = 0; index < samples; ++index)
				{
					const float sample = converted[index];
					if (std::isfinite(sample)) peak = std::max(peak, static_cast<uint32_t>(std::min(1.0f, std::abs(sample)) * 1000));
				}
				if (!reportedFirstPacket)
				{
					LOG_INFO("(AUDIO ROUTING) First game output submitted: " << packetFrames << " frames; Windows period " << backendPeriodFrames << " frames" << std::endl);
					reportedFirstPacket = true;
				}
				if (recorder)
				{
					if (!recordingStarted)
					{
						FILETIME time; GetSystemTimePreciseAsFileTime(&time);
						recordingStarted = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
					}
					recorder->Submit(converted.data(), packetFrames); recordedFrames += packetFrames;
				}
				queue.CommitRead();
			}
			UINT64 position = 0, qpc = 0;
			result = clock->GetPosition(&position, &qpc);
			if (SUCCEEDED(result))
			{
				clockSequence.fetch_add(1);
				clockPosition.store(clockOffset + position * clockFrequency / backendFrequency);
				clockQpc.store(qpc);
				clockSequence.fetch_add(1);
			}
			return S_OK;
		}

		void Run()
		{
			comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			DWORD taskIndex = 0;
			HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
			HANDLE events[] = { commandEvent, engineEvent, packetEvent, gameTimer };
			while (!quitting)
			{
				const DWORD wake = WaitForMultipleObjects(4, events, FALSE, INFINITE);
				if (wake == WAIT_OBJECT_0)
				{
					try { commandResult = command(); }
					catch (const std::bad_alloc&) { commandResult = E_OUTOFMEMORY; }
					catch (...) { commandResult = E_FAIL; }
					SetEvent(completedEvent);
				}
				else if (initialized.load() && SUCCEEDED(streamError.load()))
				{
					const HRESULT result = Pump();
					if (FAILED(result))
					{
						streamError.store(result);
						LOG_ERROR("(AUDIO ROUTING) Output stopped, HRESULT " << std::hex << result << std::dec << std::endl);
						if (gameEvent) SetEvent(gameEvent);
					}
					else
					{
						SignalGame();
					}
				}
			}
			if (backend && running) backend->Stop();
			if (recorder) recorder->Close();
			if (clock) clock->Release();
			if (render) render->Release();
			if (backend) backend->Release();
			if (task) AvRevertMmThreadCharacteristics(task);
			if (SUCCEEDED(comResult)) CoUninitialize();
		}

		void CloseEvents()
		{
			for (HANDLE handle : { commandEvent, completedEvent, engineEvent, packetEvent, gameEvent, gameTimer })
			{
				if (handle) CloseHandle(handle);
			}
		}

		EndpointFactory endpointFactory;
		AudioPacketQueue queue;
		std::shared_ptr<GameAudioRecorder> recorder;
		AudioControlServer control;
		std::wstring activeEndpoint;
		std::wstring lastRecordingPath;
		HRESULT recordingError = S_OK;
		uint64_t recordedFrames = 0;
		uint64_t recordingStarted = 0;
		uint32_t peak = 0;
		bool reportedFirstPacket = false;
		uint64_t submittedFrames = 0;
		UINT32 lastPadding = 0;
		std::array<BYTE, 1024> diagnosticBlock{}, previousBlock{};
		UINT32 diagnosticBytes = 0;
		bool havePreviousBlock = false;
		std::atomic<uint64_t> repeatedGameBlocks{ 0 }, inspectedGameBlocks{ 0 };
		uint64_t emptyOutputObservations = 0;
		ULONGLONG lastPumpTick = 0, longestPumpGapMs = 0;
		std::atomic<uint32_t> startCalls{ 0 }, stopCalls{ 0 }, acquireCalls{ 0 }, acquireFailures{ 0 }, releaseCalls{ 0 }, lastRequestedFrames{ 0 };
		std::atomic<HRESULT> lastLifecycleResult{ S_OK }, lastAcquireResult{ S_OK }, lastReleaseResult{ S_OK };
		UINT64 backendFrequency = 48000;
		UINT64 clockOffset = 0;
		WAVEFORMATEX gameFormat{};
		std::vector<float> converted;
		UINT32 pendingFrames = 0;
		std::atomic<bool> gameWakeOutstanding{ false };
		std::atomic<UINT32> gamePacketFrames{ 0 };
		LONGLONG wakeFrequency = 0, lastGameWakeTick = 0;
		UINT32 backendFrames = 0;
		UINT32 backendPeriodFrames = 0;
		UINT32 savedPeriod = 0, engineMinimum = 0, engineFundamental = 0, engineMaximum = 0;
		bool running = false;
		bool quitting = false;
		IAudioClient3* backend = nullptr;
		IAudioRenderClient* render = nullptr;
		IAudioClock* clock = nullptr;
		HANDLE commandEvent = nullptr;
		HANDLE completedEvent = nullptr;
		HANDLE engineEvent = nullptr;
		HANDLE packetEvent = nullptr;
		HANDLE gameTimer = nullptr;
		HANDLE gameEvent = nullptr;
		std::mutex commandMutex;
		std::function<HRESULT()> command;
		HRESULT commandResult = E_PENDING;
		HRESULT comResult = E_PENDING;
		std::thread worker;
	};

	class RenderClient final : public IAudioRenderClient
	{
	public:
		explicit RenderClient(std::shared_ptr<OutputSession> output) : output(std::move(output))
		{
			CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(this), &marshaler);
		}
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (id == __uuidof(IUnknown) || id == __uuidof(IAudioRenderClient)) { *object = this; AddRef(); return S_OK; }
			if (id == __uuidof(IMarshal) && marshaler) return marshaler->QueryInterface(id, object);
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
		ULONG STDMETHODCALLTYPE Release() override { const ULONG count = --references; if (!count) delete this; return count; }
		HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames, BYTE** data) override { return output->GetBuffer(frames, data); }
		HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames, DWORD flags) override { return output->ReleaseBuffer(frames, flags); }
	private:
		~RenderClient() { if (marshaler) marshaler->Release(); }
		std::atomic<ULONG> references{ 1 };
		std::shared_ptr<OutputSession> output;
		IUnknown* marshaler = nullptr;
	};

	class OutputClock final : public IAudioClock
	{
	public:
		explicit OutputClock(std::shared_ptr<OutputSession> output) : output(std::move(output))
		{
			CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(this), &marshaler);
		}
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (id == __uuidof(IUnknown) || id == __uuidof(IAudioClock)) { *object = this; AddRef(); return S_OK; }
			if (id == __uuidof(IMarshal) && marshaler) return marshaler->QueryInterface(id, object);
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
		ULONG STDMETHODCALLTYPE Release() override { const ULONG count = --references; if (!count) delete this; return count; }
		HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* frequency) override
		{
			if (!frequency) return E_POINTER;
			*frequency = output->clockFrequency;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetPosition(UINT64* position, UINT64* qpc) override
		{
			if (!position) return E_POINTER;
			for (;;)
			{
				const uint32_t before = output->clockSequence.load();
				if (before & 1) continue;
				*position = output->clockPosition.load();
				const UINT64 timestamp = output->clockQpc.load();
				if (before == output->clockSequence.load())
				{
					if (qpc) *qpc = timestamp;
					break;
				}
			}
			return output->streamError.load();
		}
		HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* characteristics) override
		{
			if (!characteristics) return E_POINTER;
			*characteristics = AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ;
			return S_OK;
		}
	private:
		~OutputClock() { if (marshaler) marshaler->Release(); }
		std::atomic<ULONG> references{ 1 };
		std::shared_ptr<OutputSession> output;
		IUnknown* marshaler = nullptr;
	};

	class AudioClient final : public IAudioClient
	{
	public:
		explicit AudioClient(std::shared_ptr<OutputSession> output) : output(std::move(output))
		{
			CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(this), &marshaler);
		}
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (id == __uuidof(IUnknown) || id == __uuidof(IAudioClient)) { *object = this; AddRef(); return S_OK; }
			if (id == __uuidof(IMarshal) && marshaler) return marshaler->QueryInterface(id, object);
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
		ULONG STDMETHODCALLTYPE Release() override { const ULONG count = --references; if (!count) delete this; return count; }
		HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME, REFERENCE_TIME,
			const WAVEFORMATEX* format, LPCGUID session) override
		{
			if (mode != AUDCLNT_SHAREMODE_EXCLUSIVE) return E_INVALIDARG;
			const HRESULT result = output->Initialize(format, flags, session);
			if (SUCCEEDED(result) && output->exposeControl)
			{
				const HRESULT controlResult = output->StartControl();
				if (FAILED(controlResult)) LOG_ERROR("(AUDIO ROUTING) Live controls unavailable, HRESULT " << std::hex << controlResult << std::dec << std::endl);
			}
			if (FAILED(result)) LOG_ERROR("(AUDIO ROUTING) Shared output initialization failed, HRESULT " << std::hex << result << std::dec << "; requires shared 48 kHz stereo support" << std::endl);
			return result;
		}
		HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override
		{
			if (!frames) return E_POINTER;
			if (!output->initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			*frames = output->periodFrames;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* latency) override
		{
			if (!latency) return E_POINTER;
			if (!output->initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			*latency = output->streamLatency + 20000000ll * output->periodFrames / 48000;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* padding) override
		{
			if (!padding) return E_POINTER;
			if (!output->initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			*padding = 0; // Exclusive event clients process one complete buffer per signal.
			return output->streamError.load();
		}
		HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX* format, WAVEFORMATEX** closest) override
		{
			if (closest) *closest = nullptr;
			return IsGameFormat(format) ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
		}
		HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override
		{
			if (!format) return E_POINTER;
			*format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
			if (!*format) return E_OUTOFMEMORY;
			**format = { WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0 };
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* normal, REFERENCE_TIME* minimum) override
		{
			if (!normal && !minimum) return E_POINTER;
			if (normal) *normal = output->defaultPeriod;
			if (minimum) *minimum = output->minimumPeriod;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE Start() override { return output->Start(); }
		HRESULT STDMETHODCALLTYPE Stop() override { return output->Stop(); }
		HRESULT STDMETHODCALLTYPE Reset() override { return output->Reset(); }
		HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE eventHandle) override { return output->SetGameEvent(eventHandle); }
		HRESULT STDMETHODCALLTYPE GetService(REFIID id, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (!output->initialized.load()) return AUDCLNT_E_NOT_INITIALIZED;
			try
			{
				if (id == __uuidof(IAudioRenderClient)) *object = new RenderClient(output);
				else if (id == __uuidof(IAudioClock)) *object = new OutputClock(output);
				else return E_NOINTERFACE;
			}
			catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
			return S_OK;
		}
	private:
		~AudioClient() { if (marshaler) marshaler->Release(); }
		std::atomic<ULONG> references{ 1 };
		std::shared_ptr<OutputSession> output;
		IUnknown* marshaler = nullptr;
	};

	Configuration ReadConfiguration()
	{
		wchar_t executable[MAX_PATH]{};
		GetModuleFileNameW(nullptr, executable, MAX_PATH);
		const auto directory = std::filesystem::path(executable).parent_path();
		const auto path = directory / L"AudioRouting.ini";
		Configuration configuration;
		configuration.enabled = GetPrivateProfileIntW(L"Audio", L"Enabled", 0, path.c_str()) == 1;
		wchar_t value[2048]{};
		GetPrivateProfileStringW(L"Audio", L"InputDevice", L"", value, 2048, path.c_str());
		configuration.inputDeviceId = value;
		GetPrivateProfileStringW(L"Audio", L"OutputDevice", L"", value, 2048, path.c_str());
		configuration.outputDeviceId = value;
		return configuration;
	}

	HRESULT CreateClient(const std::wstring& endpointId, IAudioClient** client)
	{
		if (!client) return E_POINTER;
		*client = nullptr;
		try
		{
			auto session = std::make_shared<OutputSession>();
			session->exposeControl = true;
			const HRESULT result = session->Open(endpointId);
			if (FAILED(result)) return result;
			*client = new AudioClient(std::move(session));
			return S_OK;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		catch (...) { return E_FAIL; }
	}
}
