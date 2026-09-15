#include "VirtualCableCapture.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace AsioProxy
{
	using Microsoft::WRL::ComPtr;

	namespace
	{
		constexpr PROPERTYKEY DEVICE_INSTANCE = { { 0xb3f8fa53, 0x0004, 0x438e, { 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc } }, 2 };
		constexpr PROPERTYKEY USB_FUNCTION_INSTANCE = { { 0xb3f8fa53, 0x0004, 0x438e, { 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc } }, 39 };
		constexpr wchar_t CABLE_IDENTITY[] = L"VID_12BA&PID_00FF";

		bool HasCableIdentity(IPropertyStore* properties)
		{
			if (!properties) return false;
			for (const PROPERTYKEY& key : { DEVICE_INSTANCE, USB_FUNCTION_INSTANCE })
			{
				PROPVARIANT value{};
				const HRESULT result = properties->GetValue(key, &value);
				bool matches = false;
				if (SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal)
				{
					constexpr size_t identityLength = _countof(CABLE_IDENTITY) - 1;
					for (const wchar_t* candidate = value.pwszVal; *candidate; ++candidate)
					{
						if (_wcsnicmp(candidate, CABLE_IDENTITY, identityLength) != 0) continue;
						const wchar_t following = candidate[identityLength];
						matches = following == L'\0' || following == L'&' || following == L'\\' || following == L'#';
						if (matches) break;
					}
				}
				PropVariantClear(&value);
				if (matches) return true;
			}
			return false;
		}

		class Notifications final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMMNotificationClient>
		{
		public:
			explicit Notifications(HANDLE wake) : wake(wake) {}
			HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { SetEvent(wake); return S_OK; }
			HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { SetEvent(wake); return S_OK; }
			HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { SetEvent(wake); return S_OK; }
			HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { SetEvent(wake); return S_OK; }

		private:
			HANDLE wake;
		};
	}

	VirtualCableCapture::VirtualCableCapture()
	{
		quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!quitEvent || !wakeEvent || !captureEvent)
		{
			if (quitEvent) CloseHandle(quitEvent);
			if (wakeEvent) CloseHandle(wakeEvent);
			if (captureEvent) CloseHandle(captureEvent);
			quitEvent = wakeEvent = captureEvent = nullptr;
		}
	}

	VirtualCableCapture::~VirtualCableCapture()
	{
		Stop();
		if (quitEvent) CloseHandle(quitEvent);
		if (wakeEvent) CloseHandle(wakeEvent);
		if (captureEvent) CloseHandle(captureEvent);
	}

	bool VirtualCableCapture::Start()
	{
		if (!quitEvent || !wakeEvent || !captureEvent) return false;
		if (running.exchange(true, std::memory_order_acq_rel)) return true;
		ResetEvent(quitEvent);
		Clear();
		if (testMode.load(std::memory_order_acquire)) return true;
		try { worker = std::thread(&VirtualCableCapture::Run, this); }
		catch (...) { running.store(false, std::memory_order_release); return false; }
		SetEvent(wakeEvent);
		return true;
	}

	void VirtualCableCapture::Stop()
	{
		if (!running.exchange(false, std::memory_order_acq_rel)) return;
		if (!worker.joinable())
		{
			Clear();
			return;
		}
		SetEvent(quitEvent);
		SetEvent(wakeEvent);
		if (worker.joinable()) worker.join();
		ResetEvent(quitEvent);
		CloseDevice();
		Clear();
	}

	void VirtualCableCapture::Clear()
	{
		const uint64_t write = writeFrame.load(std::memory_order_acquire);
		readFrame.store(write, std::memory_order_release);
	}

	void VirtualCableCapture::Read(int32_t* stereoSamples, uint32_t frames)
	{
		if (!stereoSamples || !frames) return;
		if (ringBusy.test_and_set(std::memory_order_acquire))
		{
			std::memset(stereoSamples, 0, static_cast<size_t>(frames) * 2 * sizeof(int32_t));
			return;
		}
		const uint64_t read = readFrame.load(std::memory_order_relaxed);
		const uint64_t write = writeFrame.load(std::memory_order_acquire);
		const uint64_t available = write > read ? write - read : 0;
		const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(available, frames));
		for (uint32_t frame = 0; frame < count; ++frame)
		{
			const uint32_t slot = static_cast<uint32_t>((read + frame) % RING_FRAMES) * 2;
			stereoSamples[frame * 2] = ring[slot];
			stereoSamples[frame * 2 + 1] = ring[slot + 1];
		}
		if (count < frames) std::memset(stereoSamples + count * 2, 0, static_cast<size_t>(frames - count) * 2 * sizeof(int32_t));
		readFrame.store(read + count, std::memory_order_release);
		ringBusy.clear(std::memory_order_release);
	}

	void VirtualCableCapture::InjectForTest(const int32_t* stereoSamples, uint32_t frames)
	{
		if (stereoSamples && frames) Push(stereoSamples, frames);
	}

	void VirtualCableCapture::Push(const int32_t* stereoSamples, uint32_t frames)
	{
		if (!stereoSamples || !frames) return;
		if (ringBusy.test_and_set(std::memory_order_acquire)) return;
		if (frames > RING_FRAMES) { stereoSamples += static_cast<size_t>(frames - RING_FRAMES) * 2; frames = RING_FRAMES; }
		const uint64_t write = writeFrame.load(std::memory_order_relaxed);
		const uint64_t read = readFrame.load(std::memory_order_acquire);
		if (write + frames - read > RING_FRAMES) readFrame.store(write + frames - RING_FRAMES, std::memory_order_release);
		for (uint32_t frame = 0; frame < frames; ++frame)
		{
			const uint32_t slot = static_cast<uint32_t>((write + frame) % RING_FRAMES) * 2;
			ring[slot] = stereoSamples[frame * 2]; ring[slot + 1] = stereoSamples[frame * 2 + 1];
		}
		writeFrame.store(write + frames, std::memory_order_release);
		ringBusy.clear(std::memory_order_release);
	}

	void VirtualCableCapture::PushFloat(const float* samples, uint32_t frames, bool silent)
	{
		uint32_t offset = 0;
		while (offset < frames)
		{
		const uint32_t count = (std::min)(MAX_PACKET_FRAMES, frames - offset);
			for (uint32_t frame = 0; frame < count; ++frame)
			{
				float value = silent ? 0.0f : samples[offset + frame];
				if (!std::isfinite(value)) value = 0.0f;
				value = std::clamp(value, -1.0f, 1.0f);
				const int32_t pcm = value >= 1.0f ? INT32_MAX : static_cast<int32_t>(value * 2147483647.0f);
				packet[frame * 2] = pcm; packet[frame * 2 + 1] = pcm;
			}
			Push(packet.data(), count);
			offset += count;
		}
	}

	bool VirtualCableCapture::OpenDevice()
	{
		ComPtr<IMMDeviceEnumerator> enumerator;
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return false;
		ComPtr<IMMDeviceCollection> devices;
		if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) return false;
		ComPtr<IMMDevice> cable;
		UINT count = 0;
		if (FAILED(devices->GetCount(&count))) return false;
		for (UINT index = 0; index < count; ++index)
		{
			ComPtr<IMMDevice> candidate;
			if (FAILED(devices->Item(index, &candidate))) continue;
			ComPtr<IPropertyStore> properties;
			if (SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, &properties)) && HasCableIdentity(properties.Get()))
			{
				if (cable) return false;
				cable = candidate;
			}
		}
		if (!cable) return false;
		LPWSTR id = nullptr;
		if (FAILED(cable->GetId(&id))) return false;
		activeEndpoint = id; CoTaskMemFree(id);
		ComPtr<IAudioClient> client;
		if (FAILED(cable->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return false;
		WAVEFORMATEX format{ WAVE_FORMAT_IEEE_FLOAT, 1, 48000, 48000 * 4, 4, 32, 0 };
		HRESULT result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
			100000, 0, &format, nullptr);
		if (SUCCEEDED(result)) result = client->SetEventHandle(captureEvent);
		ComPtr<IAudioCaptureClient> captureClient;
		if (SUCCEEDED(result)) result = client->GetService(IID_PPV_ARGS(&captureClient));
		if (SUCCEEDED(result)) result = client->Start();
		if (FAILED(result)) return false;
		backend = client;
		capture = captureClient;
		physicalCapture.store(true, std::memory_order_release);
		return true;
	}

	void VirtualCableCapture::CloseDevice()
	{
		if (backend) backend->Stop();
		capture.Reset();
		backend.Reset();
		physicalCapture.store(false, std::memory_order_release);
		activeEndpoint.clear();
	}

	bool VirtualCableCapture::DrainDevice()
	{
		auto* captureClient = capture.Get();
		for (uint32_t attempt = 0; attempt < 32; ++attempt)
		{
			BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
			const HRESULT result = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
			if (result == AUDCLNT_S_BUFFER_EMPTY) return true;
			if (FAILED(result)) return false;
			if (frames)
			{
				if (!data && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) { captureClient->ReleaseBuffer(frames); return false; }
				PushFloat(reinterpret_cast<const float*>(data), frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
			}
			if (FAILED(captureClient->ReleaseBuffer(frames))) return false;
		}
		return true;
	}

	void VirtualCableCapture::Run()
	{
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		ComPtr<IMMDeviceEnumerator> enumerator;
		ComPtr<Notifications> notifications;
		if (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE)
		{
			if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
			{
				notifications = Microsoft::WRL::Make<Notifications>(wakeEvent);
				if (notifications) enumerator->RegisterEndpointNotificationCallback(notifications.Get());
			}
		}
		while (running.load(std::memory_order_acquire))
		{
			const HANDLE events[] = { quitEvent, wakeEvent, captureEvent };
			const DWORD wait = WaitForMultipleObjects(3, events, FALSE, 250);
			if (wait == WAIT_OBJECT_0 || !running.load(std::memory_order_acquire)) break;
			if (backend && enumerator)
			{
				ComPtr<IMMDevice> device; DWORD state = 0;
				if (FAILED(enumerator->GetDevice(activeEndpoint.c_str(), &device)) || FAILED(device->GetState(&state)) || !(state & DEVICE_STATE_ACTIVE)) { CloseDevice(); nextOpenTick = 0; }
			}
			const uint64_t now = GetTickCount64();
			if (!testMode.load(std::memory_order_acquire) && !backend && enumerator && now >= nextOpenTick) { if (OpenDevice()) nextOpenTick = 0; else nextOpenTick = now + 1000; }
			if (capture && !DrainDevice()) { CloseDevice(); nextOpenTick = now + 1000; }
		}
		if (enumerator && notifications) enumerator->UnregisterEndpointNotificationCallback(notifications.Get());
		CloseDevice();
		if (SUCCEEDED(com)) CoUninitialize();
	}
}
