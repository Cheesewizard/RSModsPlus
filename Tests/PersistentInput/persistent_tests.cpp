#include "../../DLL/Audio/PersistentCapture.cpp"
#include "../../DLL/Audio/PersistentDevice.cpp"
#include <future>

namespace Audio::SharedOutput
{
	std::wstring requestedOutput;
	HRESULT CreateClient(const std::wstring& endpoint, IAudioClient** client) { requestedOutput = endpoint; *client = nullptr; return E_NOTIMPL; }
}

namespace Audio::OutputTap
{
	HRESULT CreateTapEnumerator(IMMDeviceEnumerator*, IMMDeviceEnumerator** enumerator)
	{
		if (enumerator) *enumerator = nullptr;
		return E_NOTIMPL;
	}
}


namespace PersistentInputTests
{
	using namespace Audio::PersistentInput;
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	class EmptyDevices final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMMDeviceEnumerator>
	{
	public:
		ComPtr<IMMDevice> capture;
		ComPtr<IMMDevice> render;
		unsigned enumerationCalls = 0;
		EDataFlow lastEnumeratedFlow = eAll;
		std::vector<EDataFlow> enumeratedFlows;
		HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow flow, DWORD mask, IMMDeviceCollection** devices) override
		{
			++enumerationCalls;
			lastEnumeratedFlow = flow;
			enumeratedFlows.push_back(flow);
			auto empty = Microsoft::WRL::Make<CableCollection>();
			if (mask & DEVICE_STATE_ACTIVE)
			{
				if (capture && flow != eRender) empty->devices.push_back(capture);
				if (render && flow != eCapture) empty->devices.push_back(render);
			}
			return empty.CopyTo(devices);
		}
		HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole, IMMDevice** device) override
		{
			if (flow == eCapture && capture) return capture.CopyTo(device);
			if (flow == eRender && render) return render.CopyTo(device);
			*device = nullptr; return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
		}
		HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, IMMDevice** device) override
		{
			if (wcscmp(id, L"asio-input") == 0 && capture) return capture.CopyTo(device);
			if (wcscmp(id, L"realtek") == 0 && render) return render.CopyTo(device);
			*device = nullptr; return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
		}
		IMMNotificationClient* notification = nullptr;
		HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* client) override { notification = client; return S_OK; }
		HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* client) override
		{
			Require(notification == client, "Wrong notification unregistered");
			notification = nullptr;
			return S_OK;
		}
	};

	class NotificationObserver final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMMNotificationClient>
	{
	public:
		unsigned defaults = 0, changes = 0;
		HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { ++defaults; return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { ++changes; return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { ++changes; return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { ++changes; return S_OK; }
		HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { ++changes; return S_OK; }
	};

	class AsioInputDevice final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMMDevice, IMMEndpoint>
	{
	public:
		HRESULT STDMETHODCALLTYPE Activate(REFIID, DWORD, PROPVARIANT*, void**) override { return E_NOINTERFACE; }
		HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore** properties) override { if (properties) *properties = nullptr; return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetId(LPWSTR* id) override
		{
			if (!id) return E_POINTER;
			const wchar_t value[] = L"asio-input";
			*id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(value)));
			if (!*id) return E_OUTOFMEMORY;
			std::memcpy(*id, value, sizeof(value));
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetState(DWORD* state) override { if (!state) return E_POINTER; *state = DEVICE_STATE_ACTIVE; return S_OK; }
		HRESULT STDMETHODCALLTYPE GetDataFlow(EDataFlow* flow) override { if (!flow) return E_POINTER; *flow = eCapture; return S_OK; }
	};

	IMMDeviceEnumerator* chainedEnumerator = nullptr;
	unsigned chainedCalls = 0;
	HRESULT WINAPI CreateAsioFixture(REFCLSID classId, IUnknown*, DWORD, REFIID id, void** object)
	{
		++chainedCalls;
		*object = nullptr;
		if (classId != __uuidof(MMDeviceEnumerator) || id != __uuidof(IMMDeviceEnumerator)) return E_ACCESSDENIED;
		return chainedEnumerator->QueryInterface(id, object);
	}

	void CheckTimestampErrors()
	{
		CaptureSession session(L"timestamp-fixture", false);
		WAVEFORMATEX format{ WAVE_FORMAT_IEEE_FLOAT, 1, 48000, 192000, 4, 32, 0 };
		Require(session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000) == S_OK, "Timestamp fixture init failed");
		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		session.SetEvent(event); session.Start();
		std::array<float, 480> samples{};
		session.Publish(samples.data(), 240, 17, false, true);
		session.Publish(samples.data(), 240, 90000, false, false);
		BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 timestamp = 0;
		Require(session.GetBuffer(&data, &frames, &flags, nullptr, &timestamp) == S_OK && (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && timestamp == 17, "Timestamp error was replaced with invented time");
		session.ReleaseBuffer(frames);
		session.Publish(samples.data(), 480, 190000, false, false);
		Require(session.GetBuffer(&data, &frames, &flags, nullptr, &timestamp) == S_OK && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && timestamp == 190000, "Valid timestamp inherited invalid provenance");
		session.ReleaseBuffer(frames); session.Stop(); CloseHandle(event);
	}

	void CheckAsioBridgeEnumeration()
	{
		using namespace Microsoft::WRL;
		auto asio = Make<EmptyDevices>();
		asio->capture = Make<CableDevice>(L"asio-input");
		asio->render = Make<OutputDevice>(L"realtek");
		ComPtr<IMMDeviceEnumerator> enumerator;
		chainedEnumerator = asio.Get();
		originalCreateObject = CreateAsioFixture;
		configuredCapture = false;
		configuredOutput = L"mtrack";
		configuredOutputReplacement = true;
		Require(CreateGameObject(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)) == S_OK && chainedCalls == 1, "Original ASIO enumerator was bypassed");
		void* unrelated = nullptr;
		Require(CreateGameObject(GUID_NULL, nullptr, CLSCTX_ALL, __uuidof(IUnknown), &unrelated) == E_ACCESSDENIED && chainedCalls == 2, "Non-audio COM creation did not preserve original result");
		originalCreateObject = CoCreateInstance;
		configuredCapture = true;
		configuredOutput.clear();
		chainedEnumerator = nullptr;
		for (auto role : { eConsole, eMultimedia, eCommunications })
		{
			ComPtr<IMMDevice> input, output;
			Require(enumerator->GetDefaultAudioEndpoint(eCapture, role, &input) == S_OK && input.Get() == asio->capture.Get(), "ASIO default input replaced");
			Require(enumerator->GetDefaultAudioEndpoint(eRender, role, &output) == S_OK && output.Get() != asio->render.Get(), "Windows default bypassed bridge");
			ComPtr<IAudioClient> client;
			output->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
			Require(Audio::SharedOutput::requestedOutput == L"mtrack", "Saved M-Track output lost with Realtek default");
		}
		ComPtr<IMMDevice> input, output;
		Require(enumerator->GetDevice(L"asio-input", &input) == S_OK && input.Get() == asio->capture.Get(), "ASIO input identity lost");
		Require(enumerator->GetDevice(L"realtek", &output) == S_OK && output.Get() != asio->render.Get(), "Explicit physical render lookup bypassed bridge");
		for (auto flow : { eRender, eCapture, eAll })
		{
			ComPtr<IMMDeviceCollection> devices;
			Require(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices) == S_OK, "ASIO enumeration failed");
			UINT count = 0; devices->GetCount(&count);
			if (count != (flow == eAll ? 2u : 1u))
				throw std::runtime_error("ASIO enumeration count mismatch for flow " + std::to_string(flow)
					+ ": expected " + std::to_string(flow == eAll ? 2u : 1u) + ", got " + std::to_string(count));
			if (flow != eRender)
			{
				ComPtr<IMMDevice> original; devices->Item(0, &original);
				Require(original.Get() == asio->capture.Get(), "ASIO capture collection identity lost");
			}
		}
		auto observer = Make<NotificationObserver>();
		Require(enumerator->RegisterEndpointNotificationCallback(observer.Get()) == S_OK, "ASIO notifications unavailable");
		asio->notification->OnDefaultDeviceChanged(eRender, eMultimedia, L"realtek");
		Require(observer->defaults == 0, "Physical playback default leaked into ASIO bridge");
		asio->notification->OnDefaultDeviceChanged(eCapture, eMultimedia, L"asio-input");
		Require(observer->defaults == 1, "ASIO capture notification suppressed");
		Require(enumerator->UnregisterEndpointNotificationCallback(observer.Get()) == S_OK, "ASIO notifications not released");
		asio->capture.Reset(); asio->render.Reset();
		output.Reset();
		Require(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &output) == S_OK, "ASIO bridge output disappeared with hardware");
		input.Reset();
		Require(FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eMultimedia, &input)) && !input, "Missing ASIO input silently replaced");

		BYTE call[32]{};
		call[0] = 0xe8; call[5] = 0x90; call[6] = 0x85; call[7] = 0xc0;
		int32_t displacement = 11; std::memcpy(call + 1, &displacement, 4);
		const uintptr_t module = reinterpret_cast<uintptr_t>(call + 16);
		Require(ReadAsioCall(call, module, 16) == reinterpret_cast<CreateObject>(call + 16), "RS_ASIO relative call not resolved");
		Require(!ReadAsioCall(call, module + 1, 15), "Foreign call target accepted");
		call[5] = 0xcc;
		Require(!ReadAsioCall(call, module, 16), "Modified ASIO call accepted");
	}

	void CheckLivePlayerTwoCableInsertion()
	{
		using namespace Microsoft::WRL;
		auto asio = Make<EmptyDevices>();
		asio->capture = Make<AsioInputDevice>();
		asio->render = Make<OutputDevice>(L"realtek");
		ComPtr<IMMDeviceEnumerator> enumerator;
		SetCableForPlayerTwoEnabled(false);
		Require(CreateEnumerator(asio.Get(), L"", L"", false, false, &enumerator) == S_OK, "ASIO Player 2 wrapper failed");

		auto observer = Make<NotificationObserver>();
		Require(enumerator->RegisterEndpointNotificationCallback(observer.Get()) == S_OK, "Player 2 notifications unavailable");
		ComPtr<IMMDeviceCollection> devices;
		Require(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices) == S_OK, "Initial ASIO input enumeration failed");
		UINT count = 0;
		devices->GetCount(&count);
		Require(count == 1, "Disabled Player 2 cable changed the ASIO input list");
		ComPtr<IMMDevice> first;
		devices->Item(0, &first);
		Require(first.Get() == asio->capture.Get(), "ASIO Player 1 moved before cable insertion");

		SetCableForPlayerTwoEnabled(true);
		Require(observer->changes == 2, "Live cable enable did not publish add and active notifications");
		devices.Reset();
		enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices);
		devices->GetCount(&count);
		Require(count == 2, "Live cable enable did not append exactly one input");
		ComPtr<IMMDevice> second;
		devices->Item(0, &first);
		devices->Item(1, &second);
		Require(first.Get() == asio->capture.Get() && IsCable(second.Get()), "Player 2 cable was not appended after ASIO Player 1");
		SetCableForPlayerTwoEnabled(true);
		Require(observer->changes == 2, "Idempotent cable enable emitted duplicate notifications");

		ComPtr<IMMDevice> defaultOutput;
		Require(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultOutput) == S_OK
			&& defaultOutput.Get() == asio->render.Get(), "Player 2 wrapper replaced the RS_ASIO output");
		ComPtr<IPropertyStore> outputProperties;
		Require(defaultOutput->OpenPropertyStore(STGM_READ, &outputProperties) == S_OK && outputProperties,
			"RS_ASIO output property store was not preserved");
		DWORD outputPropertyCount = 0;
		Require(outputProperties->GetCount(&outputPropertyCount) == S_OK && outputPropertyCount == 3,
			"RS_ASIO output property enumeration was interrupted");
		SetCableForPlayerTwoEnabled(false);
		Require(observer->changes == 4, "Live cable disable did not publish disabled and removed notifications");
		devices.Reset();
		enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices);
		devices->GetCount(&count);
		Require(count == 1, "Live cable disable left the Player 2 input enumerated");
		Require(enumerator->UnregisterEndpointNotificationCallback(observer.Get()) == S_OK, "Player 2 notifications were not released");
	}

	void CheckCableModeBypassesAsioOutput()
	{
		using namespace Microsoft::WRL;
		auto asio = Make<EmptyDevices>();
		asio->capture = Make<CableDevice>(L"physical-cable");
		asio->render = Make<OutputDevice>(L"asio-proxy-output");
		ComPtr<IMMDeviceEnumerator> enumerator;
		chainedEnumerator = asio.Get();
		chainedCalls = 0;
		originalCreateObject = CreateAsioFixture;
		configuredEndpoint.clear();
		configuredOutput.clear();
		configuredCapture = true;
		configuredOutputReplacement = true;
		configuredTap = false;
		Require(CreateGameObject(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
			IID_PPV_ARGS(&enumerator)) == S_OK && chainedCalls == 1,
			"Cable-mode wrapper around RS_ASIO failed");
		originalCreateObject = CoCreateInstance;
		chainedEnumerator = nullptr;

		ComPtr<IMMDeviceCollection> inputs;
		Require(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &inputs) == S_OK,
			"Cable-mode input enumeration failed");
		Require(asio->enumerationCalls == 0,
			"Cable mode initialized RS_ASIO while enumerating its permanent input");
		UINT count = 0;
		inputs->GetCount(&count);
		Require(count == 1, "Cable mode exposed both physical and permanent Cable endpoints");
		ComPtr<IMMDevice> input;
		inputs->Item(0, &input);
		Require(IsCable(input.Get()) && input.Get() != asio->capture.Get(),
			"Cable mode did not replace the physical cable with the permanent endpoint");

		ComPtr<IMMDevice> output;
		Require(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &output) == S_OK
			&& output.Get() != asio->render.Get(), "Cable mode exposed the ASIO output instead of its permanent output");

		ComPtr<IMMDeviceCollection> allDevices;
		Require(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &allDevices) == S_OK,
			"Cable-mode combined enumeration failed");
		Require(asio->enumerationCalls == 0,
			"Cable mode initialized RS_ASIO while enumerating its permanent endpoints");
		UINT allCount = 0;
		allDevices->GetCount(&allCount);
		Require(allCount == 2, "Cable-mode combined enumeration did not expose exactly permanent output plus Cable");
		ComPtr<IMMDevice> hidden;
		Require(FAILED(enumerator->GetDevice(L"asio-proxy-output", &hidden)) && !hidden,
			"Cable mode resolved a physical ASIO endpoint behind its permanent endpoints");
	}

	void CheckEnumerationAndClock()
	{
		using namespace Microsoft::WRL;
		auto empty = Make<EmptyDevices>();
		ComPtr<IMMDeviceEnumerator> enumerator;
		Require(CreateEnumerator(empty.Get(), L"", L"missing-output", true, true, &enumerator) == S_OK, "Virtual enumerator needs physical device");
		auto observer = Make<NotificationObserver>();
		Require(enumerator->RegisterEndpointNotificationCallback(observer.Get()) == S_OK && empty->notification, "Notification registration failed");
		for (auto flow : { eRender, eCapture })
		{
			for (auto role : { eConsole, eMultimedia, eCommunications }) empty->notification->OnDefaultDeviceChanged(flow, role, L"Realtek");
		}
		Require(observer->defaults == 0, "Physical defaults escaped the permanent endpoint wrapper");
		empty->notification->OnDeviceStateChanged(L"device", DEVICE_STATE_ACTIVE);
		Require(observer->changes == 1, "Device state notification was lost");
		Require(enumerator->UnregisterEndpointNotificationCallback(observer.Get()) == S_OK && !empty->notification, "Notification unregister failed");
		ComPtr<IMMDeviceCollection> collection;
		Require(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection) == S_OK, "Capture enumeration failed");
		UINT count = 0; collection->GetCount(&count);
		Require(count == 1, "Cable-free boot lacks permanent input");
		ComPtr<IMMDevice> device; collection->Item(0, &device);
		Require(IsCable(device.Get()), "Game cannot identify virtual input as Cable");
		LPWSTR id = nullptr; device->GetId(&id);
		ComPtr<IMMDevice> repeated; enumerator->GetDevice(id, &repeated); CoTaskMemFree(id);
		Require(device.Get() == repeated.Get(), "Device identity changed across discovery calls");
		collection.Reset();
		enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
		collection->GetCount(&count);
		Require(count == 1, "Hardware-free boot lacks the permanent output");
		ComPtr<IMMDevice> output;
		Require(SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &output)), "Permanent default output missing");
		ComPtr<IMMEndpoint> direction; output.As(&direction);
		EDataFlow flow = eAll;
		Require(SUCCEEDED(direction->GetDataFlow(&flow)) && flow == eRender, "Permanent output has wrong direction");
		LPWSTR outputId = nullptr; output->GetId(&outputId);
		ComPtr<IMMDevice> sameOutput; enumerator->GetDevice(outputId, &sameOutput); CoTaskMemFree(outputId);
		Require(output.Get() == sameOutput.Get(), "Permanent output identity changed");
		ComPtr<IAudioEndpointVolume> volume;
		Require(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume) == S_OK, "Virtual volume activation failed");
		Require(volume->SetMasterVolumeLevelScalar(1.1f, nullptr) == E_INVALIDARG, "Invalid gain accepted");

		ComPtr<IAudioClient> client;
		// Explicitly nonexistent selection: real scheduler and COM, with no physical driver opened.
		Require(CreateCaptureClient(L"{RSModsPlus.Test.NonexistentEndpoint}", &client) == S_OK, "Device-free client construction failed");
		WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
		Require(client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 100000, &format, nullptr) == S_OK, "Device-free initialization failed");
		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		Require(client->SetEventHandle(event) == S_OK && client->Start() == S_OK, "Device-free clock start failed");
		ComPtr<IAudioCaptureClient> capture;
		Require(client->GetService(IID_PPV_ARGS(&capture)) == S_OK, "Capture service failed");
		ComPtr<IAudioCaptureClient> duplicate;
		client->GetService(IID_PPV_ARGS(&duplicate));
		Require(capture.Get() == duplicate.Get(), "GetService changed capture identity");
		IStream* marshaled = nullptr;
		Require(CoMarshalInterThreadInterfaceInStream(__uuidof(IAudioCaptureClient), capture.Get(), &marshaled) == S_OK, "Capture marshaling failed");
		auto consumer = std::async(std::launch::async, [marshaled, event, expected = capture.Get()]()
		{
			const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			bool valid = SUCCEEDED(initialized);
			{
				ComPtr<IAudioCaptureClient> incoming;
				valid = valid && SUCCEEDED(CoGetInterfaceAndReleaseStream(marshaled, IID_PPV_ARGS(&incoming)));
				valid = valid && incoming.Get() == expected;
				UINT64 previous = 0;
				for (int packet = 0; valid && packet < 20; ++packet)
				{
					valid = WaitForSingleObject(event, 1000) == WAIT_OBJECT_0;
					BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 position = 0;
					valid = valid && incoming->GetBuffer(&data, &frames, &flags, &position, nullptr) == S_OK;
					valid = valid && frames == 480 && (flags & AUDCLNT_BUFFERFLAGS_SILENT) && (packet == 0 || position > previous);
					previous = position;
					if (frames) incoming->ReleaseBuffer(frames);
				}
			}
			if (SUCCEEDED(initialized)) CoUninitialize();
			return valid;
		});
		Require(consumer.get(), "Cross-thread permanent silence/timing failed");
		Require(client->Stop() == S_OK && client->Reset() == S_OK, "Threaded stop/reset failed");
		CloseHandle(event);
	}

	void CheckCaptureDrivenDelivery()
	{
		CaptureSession session(L"", false);
		WAVEFORMATEX format{ WAVE_FORMAT_IEEE_FLOAT, 1, 48000, 192000, 4, 32, 0 };
		Require(session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000) == S_OK, "Capture-driven initialization failed");
		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		Require(session.SetEvent(event) == S_OK && session.Start() == S_OK, "Capture-driven start failed");
		std::array<float, 960> samples{};
		for (UINT32 index = 0; index < samples.size(); ++index) samples[index] = index < 480 ? 0.25f : 0.5f;
		BYTE* data = nullptr;
		UINT32 frames = 0;
		DWORD flags = 0;
		UINT64 timestamp = 0, position = 0;
		for (UINT32 iteration = 0; iteration < 100; ++iteration)
		{
			const UINT64 captureTimestamp = 100000 + UINT64(iteration) * 100000;
			session.Publish(samples.data(), 240, captureTimestamp, false);
			Require(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "Partial packet woke game");
			session.Publish(samples.data() + 240, 240, captureTimestamp + 50000, false);
			Require(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "Complete capture waited for silence timer");
			Require(session.GetBuffer(&data, &frames, &flags, &position, &timestamp) == S_OK, "Capture not immediately readable");
			Require(timestamp == captureTimestamp && position == UINT64(iteration) * 480, "Capture timestamp or timeline changed");
			Require(!(flags & AUDCLNT_BUFFERFLAGS_SILENT), "Captured audio replaced by silence");
			session.ReleaseBuffer(frames);
			session.Tick(captureTimestamp + 90000);
			Require(session.GetNextPacketSize(&frames) == S_OK && frames == 0, "Independent timer inserted a duplicate packet");
		}
		// A backend wake can supply two packets. Release must expose the second immediately.
		session.Publish(samples.data(), 960, 20000000, false);
		Require(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "Batch wake missing");
		Require(session.GetBuffer(&data, &frames, &flags, nullptr, &timestamp) == S_OK && timestamp == 20000000, "First batch packet missing");
		Require(*reinterpret_cast<float*>(data) == 0.25f, "First batch audio changed");
		session.ReleaseBuffer(frames);
		Require(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "Queued packet waited for next timer");
		Require(session.GetBuffer(&data, &frames, &flags, nullptr, &timestamp) == S_OK && timestamp == 20100000, "Second batch timestamp lost");
		Require(*reinterpret_cast<float*>(data) == 0.5f, "Second batch audio changed");
		session.ReleaseBuffer(frames);
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		session.Tick(21000000);
		Require(session.GetBuffer(&data, &frames, &flags, nullptr, nullptr) == S_OK && (flags & AUDCLNT_BUFFERFLAGS_SILENT), "Stalled capture did not resume silent clock");
		session.ReleaseBuffer(frames);
		session.Disconnect();
		session.Tick(22000000);
		Require(session.GetBuffer(&data, &frames, &flags, nullptr, nullptr) == S_OK && (flags & AUDCLNT_BUFFERFLAGS_SILENT), "Disconnect did not keep game alive");
		session.ReleaseBuffer(frames);
		session.Stop();
		CloseHandle(event);
	}

	void Run()
	{
		CheckAsioBridgeEnumeration();
		CheckLivePlayerTwoCableInsertion();
		Microsoft::WRL::ComPtr<IPropertyStore> identity;
		Require(SUCCEEDED(PSCreateMemoryPropertyStore(IID_PPV_ARGS(&identity))), "Identity store creation failed");
		PROPVARIANT hardware{};
		InitPropVariantFromString(L"{1}.ACP\\DEVTYPE_0007&VEN_1022", &hardware);
		identity->SetValue(DEVICE_INSTANCE, hardware);
		PropVariantClear(&hardware);
		Require(!HasCableIdentity(identity.Get()), "AMD controller incorrectly identified as cable");
		InitPropVariantFromString(L"{1}.USB\\VID_12BA&PID_00FF&MI_00\\8&18BAF738&1&0000", &hardware);
		identity->SetValue(USB_FUNCTION_INSTANCE, hardware);
		PropVariantClear(&hardware);
		Require(HasCableIdentity(identity.Get()), "AMD offloaded USB cable not identified");
		InitPropVariantFromString(L"USB\\VID_12BA&PID_00FF1", &hardware);
		identity->SetValue(USB_FUNCTION_INSTANCE, hardware);
		PropVariantClear(&hardware);
		Require(!HasCableIdentity(identity.Get()), "Different USB product accepted");
		CaptureSession session(L"", false);
		WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
		Require(session.Start() == AUDCLNT_E_NOT_INITIALIZED, "Uninitialized stream started");
		Require(session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000) == S_OK, "Initialize failed");
		Require(session.Initialize(&format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000) == AUDCLNT_E_ALREADY_INITIALIZED, "Double initialize accepted");
		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		Require(session.SetEvent(event) == S_OK, "Set event failed");
		CloseHandle(event);
		Require(session.Start() == S_OK, "Start failed");
		BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 position = 0, timestamp = 0;
		session.Tick(100000);
		Require(session.GetBuffer(&data, &frames, &flags, &position, &timestamp) == S_OK && frames == 480, "No-device stream stalled");
		Require((flags & AUDCLNT_BUFFERFLAGS_SILENT) && !session.IsPhysicalPacket(), "Placeholder counted as real input");
		Require(position == 0 && timestamp == 100000, "Wrong first timeline packet");
		Require(session.GetBuffer(&data, &frames, &flags, nullptr, nullptr) == AUDCLNT_E_OUT_OF_ORDER, "Double acquire accepted");
		Require(session.ReleaseBuffer(1) == AUDCLNT_E_INVALID_SIZE, "Partial packet release accepted");
		Require(session.ReleaseBuffer(0) == S_OK, "Unread packet release failed");
		Require(session.GetBuffer(&data, &frames, &flags, &position, nullptr) == S_OK && position == 0, "Unread packet not retained");
		Require(session.ReleaseBuffer(480) == S_OK, "Packet release failed");
		std::array<float, 480> samples{};
		samples.fill(0.5f);
		session.Publish(samples.data(), 480, 150000, false);
		session.Tick(200000);
		Require(session.GetBuffer(&data, &frames, &flags, &position, &timestamp) == S_OK, "Connected input failed");
		Require(!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && session.IsPhysicalPacket(), "Real packet missing readiness");
		Require(position == 480 && timestamp == 150000, "Hardware timestamp or virtual position lost");
		int16_t sample = 0; std::memcpy(&sample, data, sizeof(sample));
		Require(sample == 16384, "Captured sample conversion failed");
		const UINT64 generation = session.GetGeneration();
		session.Disconnect();
		Require(!session.IsPhysicalPacket() && session.GetGeneration() != generation, "Disconnect did not invalidate readiness");
		std::memcpy(&sample, data, sizeof(sample));
		Require(sample == 16384, "Disconnect corrupted outstanding packet storage");
		Require(session.ReleaseBuffer(480) == S_OK, "Disconnect broke release contract");
		session.Tick(300000);
		Require(session.GetBuffer(&data, &frames, &flags, &position, nullptr) == S_OK && position == 960 && (flags & AUDCLNT_BUFFERFLAGS_SILENT), "Disconnect broke timeline");
		session.ReleaseBuffer(480);
		for (int iteration = 0; iteration < 100; ++iteration)
		{
			session.Publish(samples.data(), 480, 400000 + iteration * 200000, false);
			session.Tick(400000 + iteration * 200000);
			Require(session.GetBuffer(&data, &frames, &flags, nullptr, nullptr) == S_OK && session.IsPhysicalPacket(), "Reconnect failed");
			session.ReleaseBuffer(480);
			session.Disconnect();
			session.Tick(500000 + iteration * 200000);
			Require(session.GetBuffer(&data, &frames, &flags, nullptr, nullptr) == S_OK && (flags & AUDCLNT_BUFFERFLAGS_SILENT), "Reconnect replayed stale audio");
			session.ReleaseBuffer(480);
		}
		Require(session.Reset() == AUDCLNT_E_NOT_STOPPED, "Running reset accepted");
		session.Stop();
		Require(session.Reset() == S_OK, "Stopped reset failed");
		session.Start(); session.Tick(40000000);
		Require(session.GetBuffer(&data, &frames, &flags, &position, nullptr) == S_OK && position == 0, "Reset did not reset position");
		session.ReleaseBuffer(480); session.Stop();

		auto properties = Microsoft::WRL::Make<CableProperties>();
		PROPVARIANT value{};
		Require(properties->GetValue(DEVICE_INSTANCE, &value) == S_OK && value.vt == VT_LPWSTR && wcsstr(value.pwszVal, L"VID_12BA&PID_00FF"), "Missing native cable identity");
		PropVariantClear(&value);
		Require(properties->GetValue(AUDIO_FORMAT_KEY, &value) == S_OK && value.vt == VT_BLOB, "Device format property is not a blob");
		PropVariantClear(&value);
		CheckTimestampErrors();
		CheckCableModeBypassesAsioOutput();
		CheckEnumerationAndClock();
		CheckCaptureDrivenDelivery();
	}
}

namespace Midi::Digitech::WhammyDT { void AutoTuning(int, float) {} }
namespace Midi::Digitech::BassWhammy { void AutoTuningAndTrueTuning(int, float) {} }
namespace Midi::Digitech::WhammyFour { void AutoTuningAndTrueTuning(int, float) {} }
namespace Midi::Digitech::WhammyFive { void AutoTuningAndTrueTuning(int, float) {} }
namespace Midi::Software { void AutoTuning(int, float) {} }

int main(int argc, char** argv)
{
	const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	try
	{
		if (argc == 2 && std::strcmp(argv[1], "--inspect-devices") == 0)
		{
			Microsoft::WRL::ComPtr<IMMDeviceEnumerator> physical;
			PersistentInputTests::Require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&physical))), "Physical enumeration unavailable");
			Microsoft::WRL::ComPtr<IMMDeviceCollection> devices;
			PersistentInputTests::Require(SUCCEEDED(physical->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices)), "Capture enumeration failed");
			UINT count = 0;
			devices->GetCount(&count);
			UINT cables = 0;
			for (UINT index = 0; index < count; ++index)
			{
				Microsoft::WRL::ComPtr<IMMDevice> device;
				devices->Item(index, &device);
				if (Audio::PersistentInput::IsCable(device.Get())) ++cables;
			}
			Microsoft::WRL::ComPtr<IMMDeviceEnumerator> wrapped;
			PersistentInputTests::Require(SUCCEEDED(Audio::PersistentInput::CreateEnumerator(physical.Get(), L"", L"missing-output", true, true, &wrapped)), "Wrapped enumerator failed");
			devices.Reset();
			PersistentInputTests::Require(SUCCEEDED(wrapped->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices)), "Wrapped capture enumeration failed");
			UINT presented = 0;
			devices->GetCount(&presented);
			PersistentInputTests::Require(presented == count - cables + 1, "Physical cable not replaced by persistent endpoint");
			std::cout << "Read-only enumeration: " << count << " active inputs, " << cables << " physical cables recognized; " << presented << " inputs presented including one persistent cable. No capture clients opened.\n";
		}
		else
		{
			PersistentInputTests::Run();
			std::cout << "PASS: persistent capture lifecycle, silence, ownership, timestamps, conversion, 100 reconnects, empty-device enumeration, stable COM identity and cross-thread clock\n";
		}
		if (SUCCEEDED(com)) CoUninitialize();
		return 0;
	}
	catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
