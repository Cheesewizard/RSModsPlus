#include "../../DLL/Audio/SharedOutput.cpp"
#include "../../DLL/Audio/GameAudioRecorder.cpp"
#include <fstream>

namespace VolumeControl
{
	float testVolumes[7] = {100, 100, 100, 100, 100, 100, 100};
	bool testMixerAvailable = true;

	bool GetPlaybackVolume(unsigned int channel, float& volume)
	{
		volume = testVolumes[channel];
		return testMixerAvailable;
	}

	bool SetPlaybackVolume(unsigned int channel, float volume)
	{
		if (!testMixerAvailable) return false;
		(testVolumes[channel]) = volume;
		return true;
	}
}

namespace AudioRoutingTests
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	class FakeEndpoint final : public IAudioClient3, public IAudioRenderClient, public IAudioClock
	{
	public:
		FakeEndpoint(UINT32 period = 128) : owner(GetCurrentThreadId()), hardwarePeriod(period) { committed = CreateEventW(nullptr, FALSE, FALSE, nullptr); samples.resize(std::max<UINT32>(512, period * 2) * 2); }
		~FakeEndpoint() { CloseHandle(committed); }
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override
		{
			*value = nullptr;
			if (id == __uuidof(IAudioRenderClient)) *value = static_cast<IAudioRenderClient*>(this);
			else if (id == __uuidof(IAudioClock)) *value = static_cast<IAudioClock*>(this);
			else return E_NOINTERFACE;
			AddRef();
			return S_OK;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
		ULONG STDMETHODCALLTYPE Release() override { const ULONG count = --references; if (!count) delete this; return count; }
		HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* value) override { *value = std::max<UINT32>(512, hardwarePeriod * 2); return CheckThread(); }
		HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* value) override { *value = 10000; return CheckThread(); }
		HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* value) override
		{
			*value = blocked.load() ? std::max<UINT32>(512, hardwarePeriod * 2) : 0;
			if (invalidated.load()) return AUDCLNT_E_DEVICE_INVALIDATED;
			return CheckThread();
		}
		HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX**) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX**) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* normal, REFERENCE_TIME* minimum) override { if (normal) *normal = 100000; if (minimum) *minimum = 26666; return CheckThread(); }
		HRESULT STDMETHODCALLTYPE Start() override { running = true; return CheckThread(); }
		HRESULT STDMETHODCALLTYPE Stop() override { running = false; return CheckThread(); }
		HRESULT STDMETHODCALLTYPE Reset() override { return CheckThread(); }
		HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE value) override { engineEvent = value; return CheckThread(); }
		HRESULT STDMETHODCALLTYPE GetService(REFIID id, void** value) override { Require(SUCCEEDED(CheckThread()), "Service crossed COM thread"); return QueryInterface(id, value); }
		HRESULT STDMETHODCALLTYPE IsOffloadCapable(AUDIO_STREAM_CATEGORY, BOOL*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE SetClientProperties(const AudioClientProperties*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetBufferSizeLimits(const WAVEFORMATEX*, BOOL, REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetSharedModeEnginePeriod(const WAVEFORMATEX* format, UINT32* normal, UINT32* fundamental, UINT32* minimum, UINT32* maximum) override
		{
			Require(format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->nChannels == 2, "Wrong backend format");
			*normal = 480; *fundamental = 16; *minimum = hardwarePeriod; *maximum = 960;
			return CheckThread();
		}
		HRESULT STDMETHODCALLTYPE GetCurrentSharedModeEnginePeriod(WAVEFORMATEX**, UINT32*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE InitializeSharedAudioStream(DWORD flags, UINT32 period, const WAVEFORMATEX*, LPCGUID) override
		{
			Require(flags == AUDCLNT_STREAMFLAGS_EVENTCALLBACK && period >= hardwarePeriod && period <= 960 && period % 16 == 0, "Wrong shared initialization");
			return CheckThread();
		}
		HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames, BYTE** value) override
		{
			Require(frames > 0 && frames * 2 <= samples.size() && !acquired, "Invalid backend buffer request");
			acquired = true;
			acquiredFrames = frames;
			*value = reinterpret_cast<BYTE*>(samples.data());
			return CheckThread();
		}
		HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames, DWORD) override
		{
			Require(frames == acquiredFrames && acquired, "Invalid backend buffer release");
			acquired = false;
			{
				std::lock_guard<std::mutex> guard(sampleMutex);
				received.insert(received.end(), samples.begin(), samples.begin() + frames * 2);
			}
			SetEvent(committed);
			return CheckThread();
		}
		HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* value) override { *value = 48000; return CheckThread(); }
		HRESULT STDMETHODCALLTYPE GetPosition(UINT64* value, UINT64* qpc) override { *value = 128; *qpc = 123456; return CheckThread(); }
		HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* value) override { *value = AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ; return CheckThread(); }
		HRESULT CheckThread() const { return GetCurrentThreadId() == owner ? S_OK : E_UNEXPECTED; }
		std::atomic<bool> blocked{ false };
		std::atomic<bool> invalidated{ false };
		HANDLE engineEvent = nullptr;
		HANDLE committed = nullptr;
		std::vector<float> received;
		std::mutex sampleMutex;
	private:
		std::atomic<ULONG> references{ 1 };
		DWORD owner;
		UINT32 hardwarePeriod;
		bool running = false;
		bool acquired = false;
		std::vector<float> samples;
		UINT32 acquiredFrames = 0;
	};

	void QueueConcurrency()
	{
		Audio::AudioPacketQueue queue;
		queue.Initialize(sizeof(uint32_t), 4);
		std::thread producer([&queue]()
		{
			for (uint32_t index = 0; index < 100000; ++index)
			{
				uint8_t* packet = nullptr;
				while (!(packet = queue.BeginWrite())) std::this_thread::yield();
				std::memcpy(packet, &index, sizeof(index));
				queue.CommitWrite(sizeof(index));
			}
		});
		for (uint32_t index = 0; index < 100000; ++index)
		{
			uint32_t bytes = 0, value = 0;
			const uint8_t* packet = nullptr;
			while (!(packet = queue.BeginRead(bytes))) std::this_thread::yield();
			std::memcpy(&value, packet, sizeof(value));
			Require(bytes == sizeof(value) && value == index, "Concurrent queue reordered audio");
			queue.CommitRead();
		}
		producer.join();
		Require(queue.Count() == 0, "Queue did not drain");
	}

	void OutputAndRecording(const std::filesystem::path& directory)
	{
		FakeEndpoint* endpoint = nullptr;
		auto session = std::make_shared<Audio::SharedOutput::OutputSession>([&endpoint](const std::wstring&, IAudioClient3** client)
		{
			endpoint = new FakeEndpoint();
			*client = endpoint;
			return S_OK;
		});
		Require(SUCCEEDED(session->Open(L"fixture")), "Open failed");
		auto* client = new Audio::SharedOutput::AudioClient(session);
		WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
		Require(SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, &format, nullptr)), "Initialize failed");
		Audio::ControlRequest recordRequest; recordRequest.operation = 2; wcscpy_s(recordRequest.value, directory.c_str());
		Require(SUCCEEDED(session->HandleControl(recordRequest).result), "Recording did not start");
		UINT32 frames = 0;
		Require(SUCCEEDED(client->GetBufferSize(&frames)) && frames == 128, "Windows period replaced the requested game block size");
		IAudioRenderClient* render = nullptr;
		Require(SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))), "Render service unavailable");
		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		Require(SUCCEEDED(client->SetEventHandle(event)), "Event registration failed");
		Require(SUCCEEDED(client->Start()), "Start failed");
		Require(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "Game startup was not signaled");
		BYTE* data = nullptr;
		Require(render->GetBuffer(129, &data) == AUDCLNT_E_BUFFER_SIZE_ERROR, "Oversized packet accepted");
		Require(SUCCEEDED(render->GetBuffer(frames, &data)), "GetBuffer failed");
		for (size_t index = 0; index < 256; ++index) reinterpret_cast<int16_t*>(data)[index] = index % 2 ? -8192 : 16384;
		BYTE* second = nullptr;
		Require(render->GetBuffer(frames, &second) == AUDCLNT_E_OUT_OF_ORDER, "Overlapping acquire accepted");
		Require(SUCCEEDED(render->ReleaseBuffer(frames, 0)), "ReleaseBuffer failed");
		Require(WaitForSingleObject(endpoint->committed, 2000) == WAIT_OBJECT_0, "Output did not reach backend");
		Require(SUCCEEDED(render->GetBuffer(frames, &data)), "Silent buffer acquire failed");
		std::memset(data, 0xff, frames * 4);
		Require(SUCCEEDED(render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT)), "Silence release failed");
		Require(WaitForSingleObject(endpoint->committed, 2000) == WAIT_OBJECT_0, "Silence did not reach backend");
		{
			std::lock_guard<std::mutex> guard(endpoint->sampleMutex);
			Require(endpoint->received.size() == 512, "Unexpected output frame count");
			for (size_t index = 0; index < 256; ++index) Require(endpoint->received[index] == (index % 2 ? -0.25f : 0.5f), "Channels or PCM conversion changed");
			for (size_t index = 256; index < 512; ++index) Require(endpoint->received[index] == 0.0f, "Silent flag leaked samples");
		}
		Require(SUCCEEDED(client->Stop()), "Stop failed");
		Require(SUCCEEDED(client->Reset()), "Reset failed");
		CloseHandle(event);
		client->Release();
		render->Release();
		session.reset();
		std::filesystem::path recording;
		for (const auto& entry : std::filesystem::directory_iterator(directory)) if (entry.path().extension() == ".wav") recording = entry.path();
		Require(!recording.empty(), "No recording created");
		std::ifstream file(recording, std::ios::binary);
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		Require(bytes.size() == 44 + 1024, "Recording length is wrong");
		uint32_t length = 0;
		std::memcpy(&length, bytes.data() + 40, 4);
		Require(length == 1024 && std::memcmp(bytes.data(), "RIFF", 4) == 0, "WAV header is wrong");
		int16_t left = 0, right = 0;
		std::memcpy(&left, bytes.data() + 44, 2);
		std::memcpy(&right, bytes.data() + 46, 2);
		Require(left == 16384 && right == -8192, "Recorded channels are wrong");
		for (size_t index = 44 + 512; index < bytes.size(); ++index) Require(bytes[index] == 0, "Recording silence was corrupted");
	}

	void BackpressureAndInvalidation()
	{
		FakeEndpoint* endpoint = nullptr;
		auto session = std::make_shared<Audio::SharedOutput::OutputSession>([&endpoint](const std::wstring&, IAudioClient3** client)
		{
			endpoint = new FakeEndpoint();
			*client = endpoint;
			return S_OK;
		});
		Require(SUCCEEDED(session->Open(L"fixture")), "Fixture open failed");
		WAVEFORMATEX format{ WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0 };
		Require(SUCCEEDED(session->Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, nullptr)), "Float initialization failed");
		endpoint->blocked.store(true);
		for (int packet = 0; packet < 2; ++packet)
		{
			BYTE* data = nullptr;
			Require(SUCCEEDED(session->GetBuffer(128, &data)), "Queue did not accept its capacity");
			for (int index = 0; index < 256; ++index) reinterpret_cast<float*>(data)[index] = packet == 0 ? 0.125f : -0.75f;
			Require(SUCCEEDED(session->ReleaseBuffer(128, 0)), "Queue commit failed");
		}
		BYTE* data = nullptr;
		Require(session->GetBuffer(128, &data) == AUDCLNT_E_BUFFER_TOO_LARGE, "Full queue overwrote pending audio");
		endpoint->blocked.store(false);
		for (int packet = 0; packet < 2; ++packet)
		{
			SetEvent(endpoint->engineEvent);
			Require(WaitForSingleObject(endpoint->committed, 2000) == WAIT_OBJECT_0, "Queue failed to drain");
		}
		{
			std::lock_guard<std::mutex> guard(endpoint->sampleMutex);
			Require(endpoint->received.size() == 512, "Queued output size changed");
			for (size_t index = 0; index < 512; ++index) Require(endpoint->received[index] == (index < 256 ? 0.125f : -0.75f), "Queued float samples changed order");
		}
		endpoint->invalidated.store(true);
		SetEvent(endpoint->engineEvent);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (SUCCEEDED(session->streamError.load()) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		Require(session->GetBuffer(128, &data) == AUDCLNT_E_DEVICE_INVALIDATED, "Device loss was hidden");
		Require(session->Start() == AUDCLNT_E_DEVICE_INVALIDATED, "Start ignored device loss");
	}

	void MarshalClient()
	{
		const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		Require(SUCCEEDED(initialized), "Test COM initialization failed");
		auto session = std::make_shared<Audio::SharedOutput::OutputSession>([](const std::wstring&, IAudioClient3** client)
		{
			*client = new FakeEndpoint();
			return S_OK;
		});
		Require(SUCCEEDED(session->Open(L"fixture")), "Marshal fixture open failed");
		auto* client = new Audio::SharedOutput::AudioClient(session);
		IStream* marshaled = nullptr;
		Require(SUCCEEDED(CoMarshalInterThreadInterfaceInStream(__uuidof(IAudioClient), client, &marshaled)), "Client could not be marshaled");
		std::atomic<HRESULT> result{ E_PENDING };
		std::thread gameThread([&result, marshaled]()
		{
			const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			IAudioClient* acquired = nullptr;
			HRESULT current = CoGetInterfaceAndReleaseStream(marshaled, __uuidof(IAudioClient), reinterpret_cast<void**>(&acquired));
			if (SUCCEEDED(current))
			{
				WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
				current = acquired->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, &format, nullptr);
				acquired->Release();
			}
			result.store(current);
			if (SUCCEEDED(com)) CoUninitialize();
		});
		gameThread.join();
		Require(SUCCEEDED(result.load()), "Cross-thread client initialization failed");
		client->Release();
		session.reset();
		CoUninitialize();
	}

	void SilentDeviceSmoke(const std::filesystem::path& directory)
	{
		Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM initialization failed");
		IMMDeviceEnumerator* enumerator = nullptr;
		Require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
			__uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))), "Endpoint enumeration failed");
		IMMDevice* device = nullptr;
		Require(SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)), "No default Windows output");
		LPWSTR id = nullptr;
		Require(SUCCEEDED(device->GetId(&id)), "Could not read output identity");
		IAudioClient* client = nullptr;
		const HRESULT created = Audio::SharedOutput::CreateClient(id, &client);
		CoTaskMemFree(id);
		device->Release();
		enumerator->Release();
		Require(SUCCEEDED(created), "Physical output could not be opened");
		WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
		const HRESULT initialized = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, &format, nullptr);
		Require(SUCCEEDED(initialized), "Physical output does not support this shared configuration");
		wchar_t pipe[128]; swprintf_s(pipe, L"\\\\.\\pipe\\RSModsPlus.Audio.%u", GetCurrentProcessId());
		Audio::ControlRequest recordRequest; recordRequest.operation = 2; wcscpy_s(recordRequest.value, directory.c_str());
		Audio::ControlResponse response; DWORD responseBytes = 0;
		Require(CallNamedPipeW(pipe, &recordRequest, sizeof(recordRequest), &response, sizeof(response), &responseBytes, 3000) && SUCCEEDED(response.result), "Physical recording control failed");
		UINT32 frames = 0;
		Require(SUCCEEDED(client->GetBufferSize(&frames)), "No period available");
		IAudioRenderClient* render = nullptr;
		Require(SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))), "No render service");
		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		Require(SUCCEEDED(client->SetEventHandle(event)) && SUCCEEDED(client->Start()), "Physical output did not start");
		for (int packet = 0; packet < 300; ++packet)
		{
			Require(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "Physical output stopped signaling");
			BYTE* data = nullptr;
			const HRESULT acquired = render->GetBuffer(frames, &data);
			if (FAILED(acquired)) std::cerr << "GetBuffer HRESULT " << std::hex << acquired << std::dec << '\n';
			Require(SUCCEEDED(acquired), "Physical output rejected a packet");
			Require(SUCCEEDED(render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT)), "Physical output rejected silence");
		}
		Require(SUCCEEDED(client->Stop()), "Physical output did not stop");
		render->Release();
		client->Release();
		CloseHandle(event);
		CoUninitialize();
		std::cout << "PASS: 300 silent packets on the default physical output, period " << frames << " frames; shared backend and recording enabled\n";
	}
}

void TestLiveControls(const std::filesystem::path& directory)
{
	using namespace AudioRoutingTests;
	FakeEndpoint* endpoint = nullptr;
	Audio::SharedOutput::OutputSession session([&](const std::wstring& id, IAudioClient3** value)
	{
		if (id == L"missing") return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
		endpoint = new FakeEndpoint(id == L"second" ? 480 : 128); *value = endpoint; return S_OK;
	});
	Require(SUCCEEDED(session.Open(L"first")), "Live open failed");
	WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
	Require(SUCCEEDED(session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, nullptr)), "Live initialize failed");
	Require(SUCCEEDED(session.StartControl()), "Control pipe failed");
	HANDLE gameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	Require(SUCCEEDED(session.SetGameEvent(gameEvent)) && SUCCEEDED(session.Start()), "Live start failed");
	wchar_t pipe[128]; swprintf_s(pipe, L"\\\\.\\pipe\\RSModsPlus.Audio.%u", GetCurrentProcessId());
	auto send = [&](uint32_t operation, const std::wstring& value = L"")
	{
		Audio::ControlRequest request; request.operation = operation; wcscpy_s(request.value, value.c_str());
		Audio::ControlResponse response; DWORD bytes = 0;
		Require(CallNamedPipeW(pipe, &request, sizeof(request), &response, sizeof(response), &bytes, 3000) && bytes == sizeof(response), "Live pipe transaction failed");
		return response;
	};
	Require(send(1).recording == 0, "Recorded before Record was pressed");
	Require(SUCCEEDED(send(7, L"73").result), "Song volume command failed");
	auto mixer = send(8, L"35");
	Require(SUCCEEDED(mixer.result) && mixer.volumes[0] == 73 && mixer.volumes[1] == 35, "Mixer channels are not independent");
	Require(SUCCEEDED(send(8, L"0").result) && send(1).volumes[1] == 0, "Guitar mute rejected");
	Require(SUCCEEDED(send(7, L"100").result) && send(1).volumes[0] == 100, "Maximum song volume rejected");
	for (const auto* invalid : { L"", L"-1", L"101", L"NaN", L"5.5", L" 25", L"25x", L"999999999999999999" })
		Require(FAILED(send(8, invalid).result) && send(1).volumes[1] == 0, "Invalid mixer volume changed playback");
	VolumeControl::testMixerAvailable = false;
	Require(FAILED(send(8, L"50").result) && FAILED(send(1).mixerError), "Unavailable mixer reported success");
	VolumeControl::testMixerAvailable = true;
	for (int take = 0; take < 2; ++take)
	{
		Require(SUCCEEDED(send(2, directory.wstring()).result), "Record command failed");
		Require(FAILED(send(6, L"256\nfirst").result), "Period tuning interrupted a recording");
		Require(FAILED(send(2, directory.wstring()).result), "Duplicate Record accepted");
		BYTE* buffer = nullptr;
		Require(SUCCEEDED(session.GetBuffer(128, &buffer)), "Live get buffer failed");
		memset(buffer, 0, 512);
		Require(SUCCEEDED(session.ReleaseBuffer(128, 0)), "Live release failed");
		Require(WaitForSingleObject(endpoint->committed, 2000) == WAIT_OBJECT_0, "Live packet not played");
		auto stopped = send(3);
		Require(SUCCEEDED(stopped.result) && !stopped.recording && stopped.recordedFrames == 128 && stopped.recordingStarted != 0, "Take did not finalize correctly");
		Require(std::filesystem::file_size(stopped.file) == 44 + 128 * 4, "Live WAV length wrong");
	}
	Require(FAILED(send(4, L"missing").result), "Invalid output switch accepted");
	Require(FAILED(send(6, L"256\nwrong-device").result), "Tuning changed an unexpected device");
	Require(FAILED(send(6, L"129\nfirst").result), "Unaligned output period accepted");
	Require(SUCCEEDED(send(6, L"256\nfirst").result), "Supported output period rejected");
	Require(session.periodFrames == 128 && std::wstring(send(5).file).find(L"enginePeriod=256") != std::wstring::npos,
		"Period tuning changed the game contract or failed to report its selected period");
	Require(std::wstring(send(1).endpoint) == L"first", "Failed switch changed route");
	Require(SUCCEEDED(send(4, L"second").result), "Live output switch failed");
	Require(session.periodFrames == 128 && std::wstring(send(1).endpoint) == L"second", "Switch changed game contract");
	BYTE* switchedBuffer = nullptr;
	Require(SUCCEEDED(session.GetBuffer(128, &switchedBuffer)), "Changed output rejected game packet size");
	Require(SUCCEEDED(session.ReleaseBuffer(128, AUDCLNT_BUFFERFLAGS_SILENT)), "Changed output rejected packet");
	Require(WaitForSingleObject(endpoint->committed, 2000) == WAIT_OBJECT_0, "Changed output did not play");
	wchar_t executable[MAX_PATH]; GetModuleFileNameW(nullptr, executable, MAX_PATH);
	std::wstring clientPath = (std::filesystem::path(executable).parent_path() / L"control_client_tests.exe").wstring();
	std::wstring command = L"\"" + clientPath + L"\" " + std::to_wstring(GetCurrentProcessId()) + L" \"" + directory.wstring() + L"\"";
	STARTUPINFOW startup{}; startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	Require(CreateProcessW(clientPath.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process), "Could not start C# bridge client test");
	const ULONGLONG deadline = GetTickCount64() + 10000;
	while (WaitForSingleObject(process.hProcess, 3) == WAIT_TIMEOUT && GetTickCount64() < deadline)
	{
		BYTE* packet = nullptr;
		if (SUCCEEDED(session.GetBuffer(128, &packet))) session.ReleaseBuffer(128, AUDCLNT_BUFFERFLAGS_SILENT);
	}
	DWORD exitCode = STILL_ACTIVE; GetExitCodeProcess(process.hProcess, &exitCode);
	if (exitCode == STILL_ACTIVE) TerminateProcess(process.hProcess, 1);
	CloseHandle(process.hThread); CloseHandle(process.hProcess);
	Require(exitCode == 0, "Production C# bridge client failed or timed out");
	Require(SUCCEEDED(session.Stop()), "Live stop failed");
	CloseHandle(gameEvent);
	std::cout << "PASS: real control pipe, repeated takes, finalized WAVs, duplicate recording rejection and live output changes\n";
}

void TestCappedOutput(const std::filesystem::path& directory)
{
	using namespace AudioRoutingTests;
	FakeEndpoint* endpoint = nullptr;
	Audio::SharedOutput::OutputSession session([&](const std::wstring&, IAudioClient3** value)
	{
		endpoint = new FakeEndpoint(480); *value = endpoint; return S_OK;
	});
	Require(SUCCEEDED(session.Open(L"480-frame-device")), "Capped fixture open failed");
	WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
	Require(SUCCEEDED(session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, nullptr)), "Capped fixture init failed");
	Audio::ControlRequest request; request.operation = 2; wcscpy_s(request.value, directory.c_str());
	Require(SUCCEEDED(session.HandleControl(request).result), "Capped recording start failed");
	UINT32 totalFrames = 0;
	std::vector<float> expected;
	for (UINT32 frames : { 480u, 144u, 128u, 17u })
	{
		BYTE* data = nullptr;
		Require(SUCCEEDED(session.GetBuffer(frames, &data)), "Game's capped output request was rejected");
		auto* samples = reinterpret_cast<int16_t*>(data);
		for (UINT32 index = 0; index < frames * 2; ++index)
		{
			samples[index] = frames == 17 ? 0 : (index % 2 ? -8192 : 16384);
			expected.push_back(samples[index] / 32768.0f);
		}
		Require(SUCCEEDED(session.ReleaseBuffer(frames, 0)), "Capped output release failed");
		Require(WaitForSingleObject(endpoint->committed, 2000) == WAIT_OBJECT_0, "Capped output was not submitted");
		totalFrames += frames;
	}
	request.operation = 3;
	const auto response = session.HandleControl(request);
	Require(SUCCEEDED(response.result) && response.recordedFrames == totalFrames, "Recorded count used engine period instead of submitted size");
	Require(response.peak == 0, "Short silent packet reused stale peak samples");
	Require(std::filesystem::file_size(response.file) == 44 + totalFrames * 4, "Capped WAV length wrong");
	request.operation = 5;
	const std::wstring diagnostics = session.HandleControl(request).file;
	Require(diagnostics.find(L"repeatedGameBlocks=4 ") != std::wstring::npos,
		"Repeated game blocks were not detected across host packet boundaries");
	Require(diagnostics.find(L"inspectedGameBlocks=6 ") != std::wstring::npos,
		"Game block inspection lost partial blocks");
	std::lock_guard<std::mutex> guard(endpoint->sampleMutex);
	Require(endpoint->received == expected, "Capped output was padded, truncated or reordered");
	std::cout << "PASS: 144-frame Rocksmith cap on a 480-frame endpoint; variable packet sizes, stereo samples, peak and recording length\n";
}

void TestOutstandingWake()
{
	using namespace AudioRoutingTests;
	FakeEndpoint* endpoint = nullptr;
	Audio::SharedOutput::OutputSession session([&](const std::wstring&, IAudioClient3** value)
	{
		endpoint = new FakeEndpoint(480); *value = endpoint; return S_OK;
	});
	Require(SUCCEEDED(session.Open(L"wake-fixture")), "Wake fixture open failed");
	WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
	Require(SUCCEEDED(session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, nullptr)), "Wake fixture init failed");
	endpoint->blocked.store(true);
	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	Require(SUCCEEDED(session.SetGameEvent(event)) && SUCCEEDED(session.Start()), "Wake fixture start failed");
	Require(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "Startup wake missing");
	BYTE* data = nullptr;
	Require(SUCCEEDED(session.GetBuffer(144, &data)), "First wake acquire failed");
	SetEvent(endpoint->engineEvent);
	Require(WaitForSingleObject(event, 100) == WAIT_TIMEOUT, "Engine tick issued a duplicate wake while the game held a buffer");
	Require(SUCCEEDED(session.ReleaseBuffer(144, AUDCLNT_BUFFERFLAGS_SILENT)), "First wake release failed");
	Require(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "Second queue slot was not signaled");
	Require(SUCCEEDED(session.GetBuffer(144, &data)), "Second wake acquire failed");
	SetEvent(endpoint->engineEvent);
	Require(WaitForSingleObject(event, 100) == WAIT_TIMEOUT, "Held second slot generated a stale wake");
	Require(SUCCEEDED(session.ReleaseBuffer(144, AUDCLNT_BUFFERFLAGS_SILENT)), "Second wake release failed");
	SetEvent(endpoint->engineEvent);
	Require(WaitForSingleObject(event, 100) == WAIT_TIMEOUT, "Full queue generated a wake");
	endpoint->blocked.store(false);
	SetEvent(endpoint->engineEvent);
	Require(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "Draining did not resume the game");
	Require(SUCCEEDED(session.GetBuffer(144, &data)), "Drain wake did not reserve a usable slot");
	Require(SUCCEEDED(session.ReleaseBuffer(0, 0)), "Cancellation failed");
	Require(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "Cancellation stranded the wake reservation");
	Require(SUCCEEDED(session.Stop()), "Wake fixture stop failed");
	Require(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "Stop retained a stale event");
	Require(SUCCEEDED(session.Start()), "Wake fixture restart failed");
	Require(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "Restart stranded the wake reservation");
	Require(SUCCEEDED(session.Stop()), "Wake fixture final stop failed");
	CloseHandle(event);
	std::cout << "PASS: outstanding wake reservation, held buffers, full queue, cancellation and restart\n";
}

void TestGameWakePacing()
{
	using namespace AudioRoutingTests;
	Audio::SharedOutput::OutputSession session([](const std::wstring&, IAudioClient3** value)
	{
		*value = new FakeEndpoint(480); return S_OK;
	});
	Require(SUCCEEDED(session.Open(L"pacing")), "Pacing fixture open failed");
	WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
	Require(SUCCEEDED(session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, nullptr)), "Pacing init failed");
	BYTE* data = nullptr;
	Require(SUCCEEDED(session.GetBuffer(144, &data)), "Pacing prefill acquire failed");
	Require(SUCCEEDED(session.ReleaseBuffer(144, AUDCLNT_BUFFERFLAGS_SILENT)), "Pacing prefill failed");
	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	Require(SUCCEEDED(session.SetGameEvent(event)) && SUCCEEDED(session.Start()), "Pacing start failed");
	LARGE_INTEGER first{}, last{}, frequency{};
	QueryPerformanceFrequency(&frequency);
	for (int packet = 0; packet < 65; ++packet)
	{
		Require(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "Paced wake missing");
		QueryPerformanceCounter(&last);
		if (packet == 0) first = last;
		Require(SUCCEEDED(session.GetBuffer(144, &data)), "Paced acquire failed");
		Require(SUCCEEDED(session.ReleaseBuffer(144, AUDCLNT_BUFFERFLAGS_SILENT)), "Paced release failed");
	}
	Require(SUCCEEDED(session.Stop()), "Pacing stop failed");
	CloseHandle(event);
	const double elapsed = static_cast<double>(last.QuadPart - first.QuadPart) / frequency.QuadPart;
	Require(elapsed >= 0.185, "Bridge requested 192 ms of game audio in a burst instead of pacing callbacks");
	std::cout << "PASS: 144-frame game wake pacing independent of 480-frame engine and immediately available queue space\n";
}

int main(int argc, char** argv)
{
	try
	{
		if (argc == 3 && std::string(argv[1]) == "--silent-device")
		{
			AudioRoutingTests::SilentDeviceSmoke(std::filesystem::path(argv[2]));
			return 0;
		}
		if (argc != 2) throw std::runtime_error("Pass an empty output directory");
		AudioRoutingTests::QueueConcurrency();
		TestGameWakePacing();
		TestOutstandingWake();
		AudioRoutingTests::OutputAndRecording(std::filesystem::path(argv[1]));
		AudioRoutingTests::BackpressureAndInvalidation();
		AudioRoutingTests::MarshalClient();
		TestLiveControls(std::filesystem::path(argv[1]) / "live");
		TestCappedOutput(std::filesystem::path(argv[1]) / "capped");
		std::cout << "PASS: queue ordering, exclusive contract, backend thread ownership, stereo PCM, silence, teardown, WAV recording, backpressure, float parity, invalidation and COM marshaling\n";
		return 0;
	}
	catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
