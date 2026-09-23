#include "../../DLL/Audio/SharedOutput.cpp"
#include "../../DLL/Audio/GameAudioRecorder.cpp"
#include <fstream>

// The focused routing harness embeds SharedOutput.cpp without the game Settings translation unit.
// Keep the native overlay command linkable here; Settings.cpp owns the real persisted implementation.
namespace Settings
{
	bool SetNoteByNoteDetectionVisible(bool)
	{
		return true;
	}
}

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

// Inert stubs for DLL subsystems the harness does not compile in (OutputTap.cpp, AsioHook.cpp, RocksmithGate.cpp).
// SharedOutput.cpp calls into these from HandleControl/PassthroughStatus/RouteStart; this harness exercises the
// output session, routing decisions and control pipe, not recording, the proxy tap, or input conditioning (those
// have their own suites). Signatures mirror the headers, whose declarations are already visible via SharedOutput.cpp.
namespace Audio::OutputTap
{
	int testProxyOutputMode = 2;
	int testProxyInputMode = 0;
	bool testRecording = false;
	HRESULT testRecordingError = S_OK;
	HRESULT StartRecording(const std::wstring&, bool) { return S_OK; }
	HRESULT StopRecording(std::wstring&, uint64_t&, uint64_t&) { return S_OK; }
	bool IsRecording() { return testRecording; }
	bool IsRecordingDry() { return false; }
	HRESULT RecordingError() { return testRecordingError; }
	uint64_t RecordedFrames() { return 0; }
	uint64_t RecordingStarted() { return 0; }
	bool ProxyAvailable() { return false; }
	int ProxyInputMode() { return testProxyInputMode; }
	int ProxyOutputMode() { return testProxyOutputMode; }
	bool TryPromoteProxyOutput(const std::wstring&) { return false; }
	bool TryDemoteProxyOutput() { return false; }
	bool TryRebindProxyOutput(const std::wstring&) { return false; }
	bool ConfigureOutputGuard(bool, float, bool, float) { return true; }
	void ReadOutputLevels(float* peak, float* rms, int maxCh) { for (int i = 0; i < maxCh; ++i) { if (peak) peak[i] = 0.0f; if (rms) rms[i] = 0.0f; } }
	int ArmLatencyProbe() { return 0; }
	bool AddProxySink(ProxySinkFn) { return false; }
	void RemoveProxySink(ProxySinkFn) {}
	bool SetProxyForwardMuted(bool) { return true; }
}

namespace Audio::PersistentInput
{
	bool testCableForPlayerTwoEnabled = false;
	bool testCableForPlayerTwoAvailable = true;
	void SetCableForPlayerTwoEnabled(bool enabled) { testCableForPlayerTwoEnabled = enabled; }
	bool IsCableForPlayerTwoEnabled() { return testCableForPlayerTwoEnabled; }
	bool IsCableForPlayerTwoAvailable() { return testCableForPlayerTwoAvailable; }
}

void TestProxyStatusTruth()
{
	auto require = [](bool condition, const char* message) { if (!condition) throw std::runtime_error(message); };
	Audio::ControlRequest request;
	request.operation = 1;
	Audio::OutputTap::testProxyOutputMode = 1;
	require(std::wstring(Audio::SharedOutput::PassthroughStatus(request).endpoint) == L"(silent)", "Virtual proxy was reported as real ASIO passthrough");
	Audio::OutputTap::testProxyOutputMode = 0;
	require(std::wstring(Audio::SharedOutput::PassthroughStatus(request).endpoint) == L"(starting)", "Uninitialized proxy was reported as active output");
	Audio::OutputTap::testProxyOutputMode = 2;
	require(std::wstring(Audio::SharedOutput::PassthroughStatus(request).endpoint) == L"(passthrough)", "Live real ASIO was not reported as passthrough");
	Audio::OutputTap::testProxyOutputMode = 3;
	require(std::wstring(Audio::SharedOutput::PassthroughStatus(request).endpoint) == L"(downgrading)", "Stalled real ASIO was not reported as downgrading");
	Audio::OutputTap::testProxyInputMode = 2;
	require(Audio::SharedOutput::PassthroughStatus(request).proxyInputMode == 2,
		"Control status did not expose the live Real Tone Cable fallback");
	Audio::OutputTap::testProxyInputMode = 0;
	Audio::OutputTap::testRecording = true;
	require(Audio::SharedOutput::IsRecording(), "Unified recording state did not expose the ASIO proxy recorder");
	Audio::OutputTap::testRecordingError = E_FAIL;
	require(!Audio::SharedOutput::IsRecording() && FAILED(Audio::SharedOutput::RecordingError()),
		"Unified recording state displayed a failed ASIO proxy recorder as active");
	Audio::OutputTap::testRecording = false;
	Audio::OutputTap::testRecordingError = S_OK;
	std::cout << "PASS: proxy status distinguishes starting, silent, real ASIO and automatic downgrade\n";
}

void TestLivePlayerTwoCableControl()
{
	Audio::ControlRequest request;
	request.operation = 26;
	wcscpy_s(request.value, L"1");
	auto response = Audio::SharedOutput::PassthroughStatus(request);
	if (FAILED(response.result) || !Audio::PersistentInput::IsCableForPlayerTwoEnabled())
		throw std::runtime_error("Passthrough control did not enable the Player 2 Cable");

	wcscpy_s(request.value, L"invalid");
	response = Audio::SharedOutput::PassthroughStatus(request);
	if (response.result != E_INVALIDARG || !Audio::PersistentInput::IsCableForPlayerTwoEnabled())
		throw std::runtime_error("Invalid Player 2 Cable value changed the live state");

	Audio::PersistentInput::testCableForPlayerTwoAvailable = false;
	wcscpy_s(request.value, L"0");
	response = Audio::SharedOutput::PassthroughStatus(request);
	if (response.result != HRESULT_FROM_WIN32(ERROR_NOT_READY) || !Audio::PersistentInput::IsCableForPlayerTwoEnabled())
		throw std::runtime_error("Unavailable Player 2 Cable bypass changed the live state");
	Audio::PersistentInput::testCableForPlayerTwoAvailable = true;

	Audio::SharedOutput::OutputSession session([](const std::wstring&, IAudioClient3**) { return E_NOTIMPL; });
	response = session.HandleControl(request);
	if (FAILED(response.result) || Audio::PersistentInput::IsCableForPlayerTwoEnabled())
		throw std::runtime_error("Active-session control did not disable the Player 2 Cable");
	std::cout << "PASS: Player 2 Cable control applies live and rejects invalid values\n";
}

void TestProxyRouteFailureMatrix()
{
	using Audio::SharedOutput::PlanProxyRoute;
	using Audio::SharedOutput::ProxyRouteAction;
	auto expect = [](ProxyRouteAction actual, ProxyRouteAction expected, const char* message)
	{
		if (actual != expected) throw std::runtime_error(message);
	};
	expect(PlanProxyRoute(0, false, false, false, false), ProxyRouteAction::None,
		"Uninitialized proxy tried to mutate routing");
	expect(PlanProxyRoute(1, false, false, false, false), ProxyRouteAction::None,
		"Silent startup without an endpoint did not stay available");
	expect(PlanProxyRoute(1, false, false, false, true), ProxyRouteAction::StartManagedRoute,
		"Endpoint arrival did not start shared playback");
	expect(PlanProxyRoute(1, true, true, true, true), ProxyRouteAction::None,
		"Healthy fallback was replaced when a preferred device returned");
	expect(PlanProxyRoute(1, true, true, false, true), ProxyRouteAction::ReplaceLostManagedRoute,
		"Lost managed route did not move to the available fallback");
	expect(PlanProxyRoute(1, true, true, false, false), ProxyRouteAction::StopManagedRoute,
		"Lost last output did not return the proxy to silent mode");
	expect(PlanProxyRoute(1, true, false, false, true), ProxyRouteAction::None,
		"Automatic recovery overrode an explicit user route");
	expect(PlanProxyRoute(2, true, true, true, true), ProxyRouteAction::StopManagedRoute,
		"Explicit ASIO promotion retained the managed shared route");
	expect(PlanProxyRoute(2, true, false, true, true), ProxyRouteAction::None,
		"Real ASIO stopped an explicit user route without the apply transaction");
	expect(PlanProxyRoute(3, true, true, true, true), ProxyRouteAction::DemoteProxy,
		"Stalled ASIO callbacks did not request virtual demotion");
	std::cout << "PASS: proxy route failure matrix covers silent boot, endpoint arrival/loss, stable fallback, explicit ownership and ASIO stall\n";
}

namespace Audio::CableInput
{
	void SetOverlayEnabled(bool) {}
}

namespace Audio::AsioHook
{
	void SetInputGainDb(float) {}
	void SetNoiseGateThresholdDb(float) {}
	void SetCompressorStrength(float) {}
	void SetHumFilterBaseHz(float) {}
	void StartLatencyCapture(int) {}
	bool IsLatencyCaptureDone() { return false; }
	int GetLatencyCapture(const float** out) { if (out) *out = nullptr; return 0; }
}

namespace RocksmithGate
{
	void SetOverride(bool, float) {}
}

// The real Enumeration.cpp pokes the game's DLC service flags in memory; the harness only needs the
// SharedOutput control path to treat a force-enumeration request as handled.
namespace Enumeration
{
	void ForceEnumeration() {}
}

namespace AudioRoutingTests
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void AwaitOutput(Audio::SharedOutput::OutputSession& session)
	{
		Audio::ControlRequest status; status.operation = 1;
		const auto deadline = GetTickCount64() + 2000;
		while (session.HandleControl(status).outputError == E_PENDING && GetTickCount64() < deadline) Sleep(1);
		Require(SUCCEEDED(session.HandleControl(status).outputError), "Physical output did not become ready");
	}

	// The control pipe moved from a per-session OutputSession::StartControl() to one global server that routes to
	// whichever session is registered active (or PassthroughStatus otherwise). Register this session and host the
	// server. The server logs its own start failures and returns void, so the real verification is the named-pipe
	// round-trips that follow. Idempotent across tests: StartControlServer no-ops once the server is up, and each
	// call just re-points the active session.
	void RegisterAndHost(const std::shared_ptr<Audio::SharedOutput::OutputSession>& session)
	{
		{ std::lock_guard<std::mutex> guard(Audio::SharedOutput::g_sessionMutex); Audio::SharedOutput::g_activeSession = session; }
		Audio::SharedOutput::StartControlServer();
	}

	// A stack-owned session is registered through a non-owning alias so g_activeSession's weak_ptr resolves for the
	// life of the caller's session without taking ownership. The caller must keep the returned alias alive until the
	// session is done (assign it to a local); when both go out of scope the weak_ptr expires cleanly.
	std::shared_ptr<Audio::SharedOutput::OutputSession> HostStackControl(Audio::SharedOutput::OutputSession& session)
	{
		auto alias = std::shared_ptr<Audio::SharedOutput::OutputSession>(&session, [](Audio::SharedOutput::OutputSession*) {});
		RegisterAndHost(alias);
		return alias;
	}

	class FakeEndpoint final : public IAudioClient3, public IAudioRenderClient, public IAudioClock
	{
	public:
		UINT32 primedFrames = 0;
		std::atomic<int> failurePoint{ 0 };
		HANDLE stallEntered = nullptr, stallRelease = nullptr;
		UINT32 defaultEngineFrames = 480;
		UINT32 bufferCapacity = 0;
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
		HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* value) override { *value = bufferCapacity ? bufferCapacity : std::max<UINT32>(512, hardwarePeriod * 2); return CheckThread(); }
		HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* value) override { *value = 10000; return CheckThread(); }
		HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* value) override
		{
			if (stallRelease) { SetEvent(stallEntered); WaitForSingleObject(stallRelease, 5000); }
			*value = blocked.load() ? std::max<UINT32>(512, hardwarePeriod * 2) : 0;
			if (invalidated.load() || failurePoint.load() == 1) return AUDCLNT_E_DEVICE_INVALIDATED;
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
			*normal = defaultEngineFrames; *fundamental = 16; *minimum = hardwarePeriod; *maximum = 960;
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
			if (failurePoint.load() == 2) return AUDCLNT_E_DEVICE_INVALIDATED;
			Require(frames > 0 && frames * 2 <= samples.size() && !acquired, "Invalid backend buffer request");
			acquired = true;
			acquiredFrames = frames;
			*value = reinterpret_cast<BYTE*>(samples.data());
			return CheckThread();
		}
		HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames, DWORD flags) override
		{
			Require(frames == acquiredFrames && acquired, "Invalid backend buffer release");
			acquired = false;
			if (failurePoint.load() == 3) return AUDCLNT_E_DEVICE_INVALIDATED;
			if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
			{
				primedFrames += frames;
				return CheckThread();
			}
			{
				std::lock_guard<std::mutex> guard(sampleMutex);
				received.insert(received.end(), samples.begin(), samples.begin() + frames * 2);
			}
			SetEvent(committed);
			return CheckThread();
		}
		HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* value) override { *value = 48000; return CheckThread(); }
		HRESULT STDMETHODCALLTYPE GetPosition(UINT64* value, UINT64* qpc) override { *value = 128; *qpc = 123456; return failurePoint.load() == 4 ? AUDCLNT_E_DEVICE_INVALIDATED : CheckThread(); }
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
		AwaitOutput(*session);
		Audio::ControlRequest recordRequest; recordRequest.operation = 2; wcscpy_s(recordRequest.value, directory.c_str());
		Require(SUCCEEDED(session->HandleControl(recordRequest).result), "Recording did not start");
		UINT32 frames = 0;
		Require(SUCCEEDED(client->GetBufferSize(&frames)) && frames == 480, "Permanent game buffer capacity changed");
		frames = 128;
		IAudioRenderClient* render = nullptr;
		Require(SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))), "Render service unavailable");
		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		Require(SUCCEEDED(client->SetEventHandle(event)), "Event registration failed");
		Require(SUCCEEDED(client->Start()), "Start failed");
		Require(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "Game startup was not signaled");
		BYTE* data = nullptr;
		Require(render->GetBuffer(481, &data) == AUDCLNT_E_BUFFER_SIZE_ERROR, "Oversized packet accepted");
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
		AwaitOutput(*session);
		endpoint->blocked.store(true);
		for (int packet = 0; packet < 2; ++packet)
		{
			BYTE* data = nullptr;
			Require(SUCCEEDED(session->GetBuffer(128, &data)), "Queue did not accept its capacity");
			for (int index = 0; index < 256; ++index) reinterpret_cast<float*>(data)[index] = packet == 0 ? 0.125f : -0.75f;
			Require(SUCCEEDED(session->ReleaseBuffer(128, 0)), "Queue commit failed");
		}
		BYTE* data = nullptr;
		Sleep(5);
		endpoint->blocked.store(false);
		SetEvent(endpoint->engineEvent);
		Require(WaitForSingleObject(endpoint->committed, 2000) == WAIT_OBJECT_0, "Queue failed to drain");
		{
			std::lock_guard<std::mutex> guard(endpoint->sampleMutex);
			Require(endpoint->received.size() == 512, "Queued output size changed");
			for (size_t index = 0; index < 512; ++index) Require(endpoint->received[index] == (index < 256 ? 0.125f : -0.75f), "Queued float samples changed order");
		}
		endpoint->invalidated.store(true);
		SetEvent(endpoint->engineEvent);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		Audio::ControlRequest statusRequest; statusRequest.operation = 1;
		while (SUCCEEDED(session->HandleControl(statusRequest).outputError) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		Require(FAILED(session->HandleControl(statusRequest).outputError), "Physical output loss was not reported");
		Require(SUCCEEDED(session->GetBuffer(128, &data)), "Physical output loss escaped to the game");
		Require(SUCCEEDED(session->ReleaseBuffer(128, AUDCLNT_BUFFERFLAGS_SILENT)), "Detached game submission failed");
		HANDLE detachedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		Require(SUCCEEDED(session->SetGameEvent(detachedEvent)) && SUCCEEDED(session->Start()), "Detached output cannot start");
		CloseHandle(detachedEvent);
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
	AwaitOutput(session);
	Require(endpoint->primedFrames == 480, "Default period was not primed above the 128-frame minimum");
	auto controlAlias = HostStackControl(session);   // host the global control pipe for this session
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
	Require(SUCCEEDED(send(16, L"1,0.17782794,0,0.10").result), "Cable output limiter command required the absent ASIO proxy");
	Require(send(16, L"1,not-a-number,0,0.10").result == E_INVALIDARG, "Invalid limiter command was accepted");
	Require(SUCCEEDED(send(16, L"0,0.17782794,0,0.10").result), "Cable output limiter could not be disabled live");
	Require(FAILED(send(14, directory.wstring()).result), "Dry recording accepted an unavailable input");
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
	float drySamples[128];
	std::fill(std::begin(drySamples), std::end(drySamples), 0.25f);
	Audio::DrySignalRecording::Observe(drySamples, 128, 44100);
	Require(FAILED(send(14, directory.wstring()).result), "Dry recording accepted the wrong sample rate");
	for (int take = 0; take < 2; ++take)
	{
		Audio::DrySignalRecording::Observe(drySamples, 128, 48000);
		auto started = send(14, directory.wstring());
		Require(SUCCEEDED(started.result) && started.recordingSource == 1 && started.dryInputReady, "Dry source was not acknowledged");
		Require(FAILED(send(2, directory.wstring()).result), "Tone recording replaced an active dry take");
		Audio::DrySignalRecording::Observe(drySamples, 128, 48000);
		BYTE* buffer = nullptr;
		Require(SUCCEEDED(session.GetBuffer(128, &buffer)), "Dry take playback buffer failed");
		std::fill_n(reinterpret_cast<int16_t*>(buffer), 256, static_cast<int16_t>(-16384));
		Require(SUCCEEDED(session.ReleaseBuffer(128, 0)), "Dry take playback submission failed");
		Require(WaitForSingleObject(endpoint->committed, 2000) == WAIT_OBJECT_0, "Dry recording interrupted playback");
		auto stopped = send(3);
		Require(SUCCEEDED(stopped.result) && stopped.recordedFrames == 128 && stopped.recordingStarted != 0, "Dry take counters or timestamp wrong");
		Audio::DrySignalRecording::Observe(drySamples, 128, 48000);
		std::ifstream wave(stopped.file, std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(wave)), {});
		Require(bytes.size() == 44 + 128 * 4, "Dry take included playback or samples outside the take");
		for (size_t index = 44; index < bytes.size(); index += 2)
		{
			int16_t sample = 0; std::memcpy(&sample, bytes.data() + index, sizeof(sample));
			Require(sample == 8192, "Dry mono signal was contaminated or channels differ");
		}
	}
	std::cout << "PASS: dry input availability, rate validation, repeated dry takes, isolated samples, timestamps and uninterrupted playback\n";
	Require(SUCCEEDED(send(4, L"missing").result), "Output request was not accepted");
	Require(FAILED(send(1).outputError) && std::wstring(send(1).endpoint) == L"missing", "Missing selection was not retained");
	Require(SUCCEEDED(send(4, L"first").result), "Restore request failed");
	AwaitOutput(session);
	Require(FAILED(send(6, L"256\nwrong-device").result), "Tuning changed an unexpected device");
	Require(FAILED(send(6, L"129\nfirst").result), "Unaligned output period accepted");
	Require(SUCCEEDED(send(6, L"256\nfirst").result), "Supported output period rejected");
	Require(session.periodFrames == 480 && std::wstring(send(5).file).find(L"enginePeriod=256") != std::wstring::npos,
		"Period tuning changed the game contract or failed to report its selected period");
	Require(SUCCEEDED(send(6, L"0\nfirst").result), "Automatic output buffer rejected");
	Require(session.periodFrames == 480 && std::wstring(send(5).file).find(L"enginePeriod=480") != std::wstring::npos,
		"Automatic did not restore the device default while preserving the game buffer");
	Require(FAILED(send(6, L"0\nwrong-device").result), "Automatic changed an unexpected device");
	Require(std::wstring(send(1).endpoint) == L"first", "Failed switch changed route");
	Require(SUCCEEDED(send(4, L"second").result), "Live output switch failed");
	AwaitOutput(session);
	Require(session.periodFrames == 480 && std::wstring(send(1).endpoint) == L"second", "Switch changed game contract");
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
		Audio::DrySignalRecording::Observe(drySamples, 128, 48000);
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
	AwaitOutput(session);
	Audio::ControlRequest request; request.operation = 2; wcscpy_s(request.value, directory.c_str());
	Require(SUCCEEDED(session.HandleControl(request).result), "Capped recording start failed");
	UINT32 totalFrames = 0;
	std::vector<float> expected;
	for (UINT32 frames : { 480u, 144u, 128u, 17u })
	{
		if (frames == 17)
		{
			request.operation = 1;
			Require(session.HandleControl(request).peak == 500, "Status poll lost the peak accumulated across packets");
			Require(session.HandleControl(request).peak == 0, "Status poll did not clear the accumulated peak");
		}
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
	AwaitOutput(session);
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
	Require(WaitForSingleObject(event, 100) == WAIT_OBJECT_0, "Blocked physical output stopped game wakes");
	endpoint->blocked.store(false);
	SetEvent(endpoint->engineEvent);
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
	AwaitOutput(session);
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



void TestSmallPhysicalBuffer()
{
	using namespace AudioRoutingTests;
	FakeEndpoint* endpoint = nullptr;
	Audio::SharedOutput::OutputSession session([&](const std::wstring&, IAudioClient3** value)
	{
		endpoint = new FakeEndpoint(128);
		endpoint->defaultEngineFrames = 128;
		endpoint->bufferCapacity = 256;
		*value = endpoint; return S_OK;
	});
	Require(SUCCEEDED(session.Open(L"small-output")), "Small output bridge open failed");
	WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
	Require(SUCCEEDED(session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, nullptr)), "Small output init failed");
	AwaitOutput(session);
	Audio::ControlRequest request; request.operation = 1;
	Require(SUCCEEDED(session.HandleControl(request).outputError), "Small physical buffer refused a permanent game client");
	BYTE* data = nullptr;
	Require(SUCCEEDED(session.GetBuffer(480, &data)), "Permanent buffer acquire failed");
	for (int index = 0; index < 960; ++index) reinterpret_cast<int16_t*>(data)[index] = static_cast<int16_t>(index);
	Require(SUCCEEDED(session.ReleaseBuffer(480, 0)), "Permanent buffer release failed");
	Require(WaitForSingleObject(endpoint->committed, 2000) == WAIT_OBJECT_0, "First physical portion missing");
	SetEvent(endpoint->engineEvent);
	Require(WaitForSingleObject(endpoint->committed, 2000) == WAIT_OBJECT_0, "Second physical portion missing");
	std::lock_guard<std::mutex> guard(endpoint->sampleMutex);
	Require(endpoint->received.size() == 960, "Splitting changed the output frame count");
	for (int index = 0; index < 960; ++index) Require(endpoint->received[index] == index / 32768.0f, "Splitting changed sample order");
	std::cout << "PASS: 480-frame game packet preserved across a 256-frame physical buffer\n";
}

void TestDetachedOutput(const std::filesystem::path& directory)
{
	using namespace AudioRoutingTests;
	std::filesystem::create_directories(directory);
	std::atomic<bool> available{ false };
	FakeEndpoint* endpoint = nullptr;
	auto session = std::make_shared<Audio::SharedOutput::OutputSession>([&](const std::wstring& id, IAudioClient3** value)
	{
		if (!available.load() || id == L"missing") return AUDCLNT_E_DEVICE_INVALIDATED;
		endpoint = new FakeEndpoint(480); *value = endpoint; return S_OK;
	});
	Require(SUCCEEDED(session->Open(L"usb-output")), "Missing output prevented bridge creation");
	auto* client = new Audio::SharedOutput::AudioClient(session);
	WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
	Require(SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, &format, nullptr)), "Missing output prevented initialization");
	RegisterAndHost(session);   // shared_ptr session: register it and host the global control pipe
	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	Require(SUCCEEDED(session->SetGameEvent(event)) && SUCCEEDED(session->Start()), "Detached output did not start");
	Audio::ControlRequest recordRequest;
	recordRequest.operation = 2;
	wcscpy_s(recordRequest.value, directory.c_str());
	Require(SUCCEEDED(session->HandleControl(recordRequest).result), "Detached-output recording did not start");
	Require(Audio::SharedOutput::IsRecording(), "Unified recording state did not expose the managed recorder");
	Audio::ControlRequest request; request.operation = 1;
	Require(FAILED(session->HandleControl(request).outputError), "Missing output reported ready");
	auto submit = [&]()
	{
		Require(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "Detached game callback stalled");
		BYTE* data = nullptr;
		Require(SUCCEEDED(session->GetBuffer(144, &data)), "Detached game buffer failed");
		Require(SUCCEEDED(session->ReleaseBuffer(144, AUDCLNT_BUFFERFLAGS_SILENT)), "Detached game release failed");
	};
	for (int index = 0; index < 20; ++index) submit();
	const auto detachedPosition = session->clockPosition.load();
	Require(detachedPosition > 0, "Detached game clock stopped");
	available.store(true);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (FAILED(session->HandleControl(request).outputError) && std::chrono::steady_clock::now() < deadline) submit();
	Require(SUCCEEDED(session->HandleControl(request).outputError), "Same device did not reconnect automatically");
	Require(session->clockPosition.load() >= detachedPosition, "Reconnect moved clock backwards");

	BYTE* held = nullptr;
	Require(SUCCEEDED(session->GetBuffer(144, &held)), "Held buffer acquire failed");
	const HANDLE engineEvent = endpoint->engineEvent;
	available.store(false);
	endpoint->invalidated.store(true);
	SetEvent(engineEvent);
	const auto lossDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (SUCCEEDED(session->HandleControl(request).outputError) && std::chrono::steady_clock::now() < lossDeadline) std::this_thread::yield();
	Require(FAILED(session->HandleControl(request).outputError), "Unplug was not reported");
	Require(SUCCEEDED(session->ReleaseBuffer(144, AUDCLNT_BUFFERFLAGS_SILENT)), "Unplug invalidated the held game buffer");
	for (int index = 0; index < 10; ++index) submit();
	available.store(true);
	Require(SUCCEEDED(session->SwitchOutput(L"new-usb-port")), "Replacement output failed to connect live");
	AwaitOutput(*session);
	Require(std::wstring(session->HandleControl(request).endpoint) == L"new-usb-port", "Replacement identity was not reported");
	Audio::ControlRequest stopRequest;
	stopRequest.operation = 3;
	const auto stopped = session->HandleControl(stopRequest);
	Require(SUCCEEDED(stopped.result) && stopped.recordedFrames >= 4320,
		"Output disconnect interrupted or failed to finalize the active recording");
	Require(!Audio::SharedOutput::IsRecording(), "Unified recording state remained active after Stop");
	std::ifstream recordedFile(stopped.file, std::ios::binary);
	char riff[4]{};
	recordedFile.read(riff, sizeof(riff));
	Require(recordedFile.good() && std::memcmp(riff, "RIFF", sizeof(riff)) == 0,
		"Output disconnect left the recording WAV unfinalized");
	Require(SUCCEEDED(session->Stop()) && SUCCEEDED(session->Reset()) && SUCCEEDED(session->Start()), "Recovered client lifecycle failed");
	client->Release();
	CloseHandle(event);
	std::cout << "PASS: missing-output boot, recording through unplug, finalized WAV, automatic reconnect and stable game clock\n";
}

void TestDriverIsolation()
{
	using namespace AudioRoutingTests;
	HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	std::atomic<bool> stall{ false };
	std::atomic<unsigned> opened{ 0 };
	Audio::SharedOutput::OutputSession session([&](const std::wstring& id, IAudioClient3** value)
	{
		if (stall.load() && id == L"B")
		{
			SetEvent(entered);
			WaitForSingleObject(release, 5000);
		}
		++opened;
		*value = new FakeEndpoint(480);
		return S_OK;
	});
	session.Open(L"A");
	WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
	Require(SUCCEEDED(session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, nullptr)), "Isolation init failed");
	AwaitOutput(session);
	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	Require(SUCCEEDED(session.SetGameEvent(event)) && SUCCEEDED(session.Start()), "Isolation start failed");
	auto controlAlias = HostStackControl(session);   // host the global control pipe for this session
	stall.store(true);
	const auto requestedAt = GetTickCount64();
	Require(SUCCEEDED(session.SwitchOutput(L"B")) && GetTickCount64() - requestedAt < 100, "Switch blocked on opening");
	Require(WaitForSingleObject(entered, 2000) == WAIT_OBJECT_0, "Delayed open was not entered");
	const auto before = session.clockPosition.load();
	wchar_t pipe[128]; swprintf_s(pipe, L"\\\\.\\pipe\\RSModsPlus.Audio.%u", GetCurrentProcessId());
	for (unsigned index = 0; index < 30; ++index)
	{
		Require(WaitForSingleObject(event, 200) == WAIT_OBJECT_0, "Delayed open blocked game pacing");
		BYTE* data = nullptr;
		Require(SUCCEEDED(session.GetBuffer(144, &data)) && SUCCEEDED(session.ReleaseBuffer(144, AUDCLNT_BUFFERFLAGS_SILENT)), "Delayed open invalidated game buffers");
		Audio::ControlRequest request; request.operation = 5;
		Audio::ControlResponse response; DWORD bytes = 0;
		Require(CallNamedPipeW(pipe, &request, sizeof(request), &response, sizeof(response), &bytes, 200) && response.outputError == E_PENDING, "Delayed open blocked status pipe");
	}
	Require(session.clockPosition.load() > before, "Delayed open stopped virtual clock");
	Require(SUCCEEDED(session.SwitchOutput(L"A")), "Latest route request blocked");
	SetEvent(release);
	AwaitOutput(session);
	Audio::ControlRequest status; status.operation = 1;
	Require(std::wstring(session.HandleControl(status).endpoint) == L"A" && opened.load() == 3, "Late B replaced latest A");
	session.Stop();
	CloseHandle(event); CloseHandle(entered); CloseHandle(release);
	std::cout << "PASS: stalled open, responsive real status pipe, uninterrupted game buffers/clock, A-B-A generations\n";
}

void TestRenderFailures()
{
	using namespace AudioRoutingTests;
	for (int point = 1; point <= 4; ++point)
	{
		FakeEndpoint* endpoint = nullptr;
		Audio::SharedOutput::OutputSession session([&](const std::wstring&, IAudioClient3** value)
		{
			endpoint = new FakeEndpoint(480); endpoint->AddRef(); *value = endpoint; return S_OK;
		});
		session.Open(L"failure");
		WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
		session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, nullptr);
		AwaitOutput(session);
		endpoint->failurePoint.store(point);
		BYTE* data = nullptr;
		Require(SUCCEEDED(session.GetBuffer(144, &data)) && SUCCEEDED(session.ReleaseBuffer(144, AUDCLNT_BUFFERFLAGS_SILENT)), "Failure submission failed");
		SetEvent(endpoint->engineEvent);
		Audio::ControlRequest status; status.operation = 1;
		const auto deadline = GetTickCount64() + 500;
		while (SUCCEEDED(session.HandleControl(status).outputError) && GetTickCount64() < deadline) Sleep(1);
		Require(FAILED(session.HandleControl(status).outputError) && SUCCEEDED(session.streamError.load()), "Physical failure escaped or was ignored");
		endpoint->Release();
	}
	std::cout << "PASS: invalidation at padding, acquire, release and clock reads stays physical\n";
}

void TestConfiguredPeriodReset()
{
	using namespace AudioRoutingTests;
	Audio::SharedOutput::OutputSession session([](const std::wstring&, IAudioClient3** value)
	{
		*value = new FakeEndpoint(128); return S_OK;
	});
	session.Open(L"configured-period");
	WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
	session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, nullptr);
	AwaitOutput(session);
	Require(SUCCEEDED(session.SwitchOutput(L"configured-period", 256, false)), "Custom period rejected");
	AwaitOutput(session);
	Require(SUCCEEDED(session.Reset()), "Custom period reset failed");
	AwaitOutput(session);
	Audio::ControlRequest request; request.operation = 5;
	Require(std::wstring(session.HandleControl(request).file).find(L"enginePeriod=256 ") != std::wstring::npos, "Reset lost selected period");
	std::cout << "PASS: game reset preserves selected physical period without rewriting settings\n";
}

void TestStalledRender()
{
	using namespace AudioRoutingTests;
	HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	FakeEndpoint* replacement = nullptr;
	{
		Audio::SharedOutput::OutputSession session([&](const std::wstring& id, IAudioClient3** value)
		{
			auto* endpoint = new FakeEndpoint(480);
			if (id == L"stalled") { endpoint->stallEntered = entered; endpoint->stallRelease = release; }
			else replacement = endpoint;
			*value = endpoint; return S_OK;
		});
		session.Open(L"stalled");
		WAVEFORMATEX format{ WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 384000, 8, 32, 0 };
		session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, nullptr);
		AwaitOutput(session);
		Require(WaitForSingleObject(entered, 2000) == WAIT_OBJECT_0, "Render did not enter stall");
		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		session.SetGameEvent(event); session.Start();
		const auto startedAt = GetTickCount64();
		while (GetTickCount64() - startedAt < 650)
		{
			Require(WaitForSingleObject(event, 200) == WAIT_OBJECT_0, "Stalled render stopped game wake");
			BYTE* data = nullptr;
			Require(SUCCEEDED(session.GetBuffer(144, &data)), "Stalled render blocked game buffer");
			std::fill_n(reinterpret_cast<float*>(data), 288, 0.75f);
			session.ReleaseBuffer(144, 0);
		}
		Audio::ControlRequest status; status.operation = 5;
		const auto snapshot = session.HandleControl(status);
		Require(snapshot.outputError == HRESULT_FROM_WIN32(ERROR_TIMEOUT), "Stalled render was reported healthy");
		Require(std::wstring(snapshot.file).find(L"expiredFrames=0 ") == std::wstring::npos, "Stall retained old audio");
		BYTE* held = nullptr;
		Require(WaitForSingleObject(event, 200) == WAIT_OBJECT_0 && SUCCEEDED(session.GetBuffer(144, &held)), "Pre-switch held packet failed");
		std::fill_n(reinterpret_cast<float*>(held), 288, 0.5f);
		session.SwitchOutput(L"replacement");
		Require(SUCCEEDED(session.ReleaseBuffer(144, 0)), "Switch invalidated a held game buffer");
		SetEvent(release);
		AwaitOutput(session);
		BYTE* data = nullptr;
		Require(WaitForSingleObject(event, 200) == WAIT_OBJECT_0 && SUCCEEDED(session.GetBuffer(144, &data)), "Replacement game wake failed");
		session.ReleaseBuffer(144, AUDCLNT_BUFFERFLAGS_SILENT);
		Require(WaitForSingleObject(replacement->committed, 2000) == WAIT_OBJECT_0, "Replacement did not receive current audio");
		{
			std::lock_guard<std::mutex> guard(replacement->sampleMutex);
			for (float sample : replacement->received) Require(sample == 0, "Old sound replayed after stalled render");
		}
		session.Stop(); CloseHandle(event);
	}
	CloseHandle(entered); CloseHandle(release);
	std::cout << "PASS: stalled padding call, timeout diagnostics, bounded queue age, virtual cadence and no stale replay\n";
}

void TestRateMatching()
{
	using namespace AudioRoutingTests;
	for (double skew : { -300.0, 300.0 })
	{
		Audio::SharedOutput::OutputRateMatcher matcher;
		for (int index = 0; index < 20; ++index) matcher.Measure(skew, 0);
		uint64_t source = 0, output = 0;
		std::array<float, 960> samples{};
		double squareSum = 0;
		for (unsigned block = 0; block < 1000; ++block)
		{
			unsigned consumed = 0;
			const unsigned produced = matcher.Read([&](unsigned frame, unsigned channel)
			{
				const float value = static_cast<float>(std::sin((source + frame) * 6.283185307179586 * 1000 / 48000));
				return channel ? -value : value;
			}, 1024, samples.data(), 480, consumed);
			Require(produced == 480, "Rate matcher stranded available frames");
			for (unsigned frame = 0; frame < produced; ++frame)
			{
				Require(std::abs(samples[frame * 2] + samples[frame * 2 + 1]) < 1e-6, "Rate matcher changed stereo relationship");
				if (block) squareSum += samples[frame * 2] * samples[frame * 2];
			}
			source += consumed; output += produced;
		}
		Require(std::abs(double(source) - output * (1 - skew / 1000000)) < 2, "Clock skew was not compensated fractionally");
		Require(std::abs(std::sqrt(squareSum / (999 * 480)) - std::sqrt(0.5)) < 0.002, "Resampler changed passband gain");
	}
	std::cout << "PASS: positive/negative clock skew, fractional frame conservation, stereo and passband gain\n";
}

void TestWindowsOutputChange()
{
	using namespace AudioRoutingTests;
	using namespace Audio::SharedOutput;
	for (const auto& selected : { std::wstring(L"M-Track"), std::wstring() })
	{
		auto notifications = Microsoft::WRL::Make<OutputNotifications>();
		std::atomic<bool> speakersDefault{ false };
		auto backend = std::make_shared<OutputBackend>([&](std::wstring& id, IAudioClient3** client)
		{
			if (id.empty()) id = speakersDefault.load() ? L"Realtek" : L"M-Track";
			*client = new FakeEndpoint(128);
			return S_OK;
		}, false, notifications);
		backend->Request(selected, 128);
		backend->SetRunning(true);
		backend->Launch();
		auto await = [&](uint64_t after)
		{
			const auto deadline = GetTickCount64() + 2000;
			while (GetTickCount64() < deadline)
			{
				const auto status = backend->GetStatus();
				if (status.generation > after && status.state == BackendState::Running) return status;
				Sleep(1);
			}
			throw std::runtime_error("Windows output change did not recover");
		};
		const auto original = await(0);
		speakersDefault.store(true);
		notifications->OnDefaultDeviceChanged(eCapture, eConsole, L"input");
		notifications->OnDefaultDeviceChanged(eRender, eCommunications, L"Realtek");
		Sleep(30);
		Require(backend->GetStatus().generation == original.generation, "Unrelated default role reopened output");
		notifications->OnDefaultDeviceChanged(eRender, eConsole, L"Realtek");
		const auto recovered = await(original.generation);
		Require(recovered.requestedEndpoint == selected, "Windows default rewrote selected output");
		Require(recovered.activeEndpoint == (selected.empty() ? L"Realtek" : L"M-Track"), "Recovery selected the wrong output");
		Require(recovered.period == 128, "Recovery lost configured output period");
		backend->Shutdown();
	}
	std::cout << "PASS: Windows default change preserves explicit M-Track output and follows default only when selected\n";
}

void TestSharedOutputLimiter()
{
	using namespace AudioRoutingTests;
	using namespace Audio::SharedOutput;
	FakeEndpoint* endpoint = nullptr;
	auto backend = std::make_shared<OutputBackend>([&](std::wstring& id, IAudioClient3** client)
	{
		id = L"speakers";
		endpoint = new FakeEndpoint(128);
		endpoint->AddRef();
		*client = endpoint;
		return S_OK;
	});
	Require(!backend->ConfigureOutputGuard(true, 0.0f, false, 0.10f), "Zero limiter ceiling was accepted");
	Require(backend->ConfigureOutputGuard(true, 0.25f, false, 0.10f), "Shared-output limiter configuration failed");
	backend->Request(L"speakers", 128);
	backend->SetRunning(true);
	backend->Launch();
	const auto deadline = GetTickCount64() + 2000;
	while (backend->GetStatus().state != BackendState::Running && GetTickCount64() < deadline) Sleep(1);
	Require(backend->GetStatus().state == BackendState::Running, "Limiter test output did not start");
	std::array<float, 960> loud;
	loud.fill(1.0f);
	backend->Submit(loud.data(), 480, GetTickCount64());
	Require(WaitForSingleObject(endpoint->committed, 2000) == WAIT_OBJECT_0, "Limited output was not rendered");
	bool heardSignal = false;
	{
		std::lock_guard<std::mutex> guard(endpoint->sampleMutex);
		for (const float sample : endpoint->received)
		{
			Require(std::abs(sample) <= 0.25001f, "Shared-output limiter exceeded its ceiling");
			if (std::abs(sample) > 0.001f) heardSignal = true;
		}
	}
	Require(heardSignal, "Shared-output limiter produced only silence");
	backend->Shutdown();
	endpoint->Release();
	std::cout << "PASS: Cable shared-output limiter accepts live control and holds the configured ceiling\n";
}

void TestConfiguredOutputFallsBackAfterStreamOpenFailure()
{
	using namespace AudioRoutingTests;
	using namespace Audio::SharedOutput;
	std::atomic<int> configuredAttempts{ 0 };
	std::atomic<int> defaultAttempts{ 0 };
	std::atomic<bool> configuredAvailable{ false };
	auto notifications = Microsoft::WRL::Make<OutputNotifications>();
	auto backend = std::make_shared<OutputBackend>([&](std::wstring& id, IAudioClient3** client)
	{
		auto* endpoint = new FakeEndpoint(128);
		if (id == L"stale" && !configuredAvailable.load())
		{
			++configuredAttempts;
			endpoint->failurePoint.store(2);
		}
		else if (id == L"stale")
		{
			++configuredAttempts;
		}
		else
		{
			++defaultAttempts;
			id = L"current-default";
		}
		*client = endpoint;
		return S_OK;
	}, false, notifications, true);
	backend->Request(L"stale", 128);
	backend->SetRunning(true);
	backend->Launch();
	const auto deadline = GetTickCount64() + 2000;
	OutputBackendStatus status;
	do
	{
		status = backend->GetStatus();
		if (status.state == BackendState::Running) break;
		Sleep(1);
	} while (GetTickCount64() < deadline);
	Require(status.state == BackendState::Running && SUCCEEDED(status.error), "Default output fallback did not recover the backend");
	Require(status.requestedEndpoint == L"current-default", "Default output fallback did not become the active user-visible selection");
	Require(status.activeEndpoint == L"current-default", "Default output fallback did not report its live endpoint");
	Require(configuredAttempts.load() == 1 && defaultAttempts.load() == 1, "Default output fallback did not make exactly one recovery attempt");
	Require(status.period == 480, "Default output fallback reused the stale endpoint period");
	const auto fallbackGeneration = status.generation;
	configuredAvailable.store(true);
	notifications->OnDeviceAdded(L"stale");
	const auto reconnectDeadline = GetTickCount64() + 2000;
	do
	{
		status = backend->GetStatus();
		if (status.generation > fallbackGeneration && status.state == BackendState::Running) break;
		Sleep(1);
	} while (GetTickCount64() < reconnectDeadline);
	Require(status.requestedEndpoint == L"current-default" && status.activeEndpoint == L"current-default",
		"Reconnected preferred output replaced the healthy fallback without user approval");
	backend->Request(L"stale", 128);
	const auto explicitDeadline = GetTickCount64() + 2000;
	do
	{
		status = backend->GetStatus();
		if (status.state == BackendState::Running && status.activeEndpoint == L"stale") break;
		Sleep(1);
	} while (GetTickCount64() < explicitDeadline);
	Require(status.requestedEndpoint == L"stale" && status.activeEndpoint == L"stale",
		"Explicit output selection did not promote the reconnected device");
	backend->Shutdown();
	std::cout << "PASS: fallback remains selected after reconnect and only explicit Apply promotes the device\n";
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
		TestDriverIsolation();
		TestProxyStatusTruth();
		TestLivePlayerTwoCableControl();
		TestProxyRouteFailureMatrix();
		TestWindowsOutputChange();
		TestConfiguredOutputFallsBackAfterStreamOpenFailure();
		TestSharedOutputLimiter();
		TestRenderFailures();
		TestStalledRender();
		TestConfiguredPeriodReset();
		TestRateMatching();
		TestSmallPhysicalBuffer();
		TestDetachedOutput(std::filesystem::path(argv[1]) / "detached");
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
