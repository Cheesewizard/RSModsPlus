// Compile the production implementation in this test translation unit so its private
// registration and callback paths can be exercised without a game or an ASIO driver.
#define RSMODS_AUDIO_LIFECYCLE_TRACE
#include "../../DLL/Audio/AsioHook.cpp"
#include "../../DLL/Audio/DelayLinePitchShifter.cpp"

#include <stdexcept>
#include <future>

namespace AudioCaptureTests
{
	struct FakeCapture
	{
		void** vtable;
		std::array<int32_t, 8> samples{ 123456789, -123456789, 1, -1, 2147483647, -2147483647, 101, -101 };
		unsigned calls = 0;
		HRESULT result = S_OK;
		UINT32 frames = 4;
		DWORD flags = 0;
	};

	unsigned observedPackets = 0;

	struct FakeAudioClient
	{
		void** vtable;
		unsigned calls = 0;
		HRESULT result = E_FAIL;
	};

	HRESULT STDMETHODCALLTYPE ClientCall(IAudioClient* object)
	{
		auto& client = *reinterpret_cast<FakeAudioClient*>(object);
		++client.calls;
		return client.result;
	}

	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void LifecycleForwarding()
	{
		using namespace Audio::LifecycleTrace;
		void* table[15]{};
		for (size_t index = START_SLOT; index <= RESET_SLOT; ++index)
			table[index] = reinterpret_cast<void*>(&ClientCall);
		FakeAudioClient first{ table }, second{ table };
		Attach(reinterpret_cast<IAudioClient*>(&first), &first);
		Attach(reinterpret_cast<IAudioClient*>(&second), &second);
		Require(tableCount.load() == 1, "Shared table patched more than once");
		for (size_t index = START_SLOT; index <= RESET_SLOT; ++index)
		{
			const auto invoke = reinterpret_cast<ClientMethod>(table[index]);
			Require(invoke(reinterpret_cast<IAudioClient*>(&first)) == E_FAIL, "Lifecycle failure result changed");
			second.result = S_FALSE;
			Require(invoke(reinterpret_cast<IAudioClient*>(&second)) == S_FALSE, "Lifecycle success result changed");
		}
		Require(first.calls == 3 && second.calls == 3, "Lifecycle call swallowed or repeated");
		unsigned begins = 0;
		for (const auto& slot : slots)
		{
			if (slot.state.load() != 2) continue;
			const auto& event = slot.event;
			if (event.kind == Kind::StartBegin || event.kind == Kind::StopBegin || event.kind == Kind::ResetBegin)
			{
				Require(event.context != nullptr && event.thread != 0, "Missing lifecycle caller identity");
				++begins;
			}
		}
		Require(begins == 6, "Missing lifecycle events");
		Attach(nullptr, nullptr);
		Require(tableCount.load() == 1, "Rejected client changed table registry");
	}

	void LifecycleOverflow()
	{
		using namespace Audio::LifecycleTrace;
		for (size_t index = 0; index < CAPACITY + 7; ++index)
			Record(Kind::StopBegin, nullptr, nullptr);
		Require(dropped.load() == 7, "Ring overflow not counted");
		for (size_t index = 0; index < CAPACITY; ++index)
			Require(slots[index].state.load() == 2 && slots[index].event.sequence == index, "Unread event overwritten");
	}

	HRESULT STDMETHODCALLTYPE GetBuffer(IAudioCaptureClient* object, BYTE** data,
		UINT32* frames, DWORD* flags, UINT64* device, UINT64* counter)
	{
		auto& capture = *reinterpret_cast<FakeCapture*>(object);
		++capture.calls;
		*data = reinterpret_cast<BYTE*>(capture.samples.data());
		*frames = capture.frames;
		*flags = capture.flags;
		if (device) *device = 42;
		if (counter) *counter = 123;
		return capture.result;
	}

	void Attach(FakeCapture& capture, uint32_t sampleRate = 48000, uint16_t bits = 32,
		uint16_t formatTag = WAVE_FORMAT_PCM)
	{
		Audio::AsioHook::PaWasapiStreamPrefix stream{};
		stream.input.clientParent = reinterpret_cast<IUnknown*>(&capture);
		stream.captureClient = reinterpret_cast<IAudioCaptureClient*>(&capture);
		stream.input.waveFormat.Format.wFormatTag = formatTag;
		stream.input.waveFormat.Format.nChannels = 2;
		stream.input.waveFormat.Format.nSamplesPerSec = sampleRate;
		stream.input.waveFormat.Format.wBitsPerSample = bits;
		stream.input.waveFormat.Format.nBlockAlign = 2 * bits / 8;
		Audio::AsioHook::RegisterCaptureStream(&stream);
	}

	void Read(FakeCapture& capture)
	{
		BYTE* data = nullptr;
		UINT32 frames = 0;
		DWORD flags = 0;
		UINT64 position = 0;
		Require(Audio::AsioHook::Hook_CaptureGetBuffer(reinterpret_cast<IAudioCaptureClient*>(&capture),
			&data, &frames, &flags, &position, nullptr) == S_OK, "GetBuffer result changed");
		Require(data == reinterpret_cast<BYTE*>(capture.samples.data()) && frames == 4 && position == 42,
			"GetBuffer outputs changed");
	}

	void StartupLiveness()
	{
		void* table[4] = { nullptr, nullptr, nullptr, reinterpret_cast<void*>(&GetBuffer) };
		FakeCapture original{ table };
		FakeCapture newcomer{ table };
		Audio::AsioHook::configuredInputs[0] = true;
		Attach(original);
		const auto originalSamples = original.samples;
		Read(original);
		Require(original.samples == originalSamples, "Disabled hook modified samples");
		Require(original.calls == 1, "Capture called more than once");
		Attach(newcomer);
		Require(Audio::AsioHook::FindCaptureRoute(reinterpret_cast<IAudioCaptureClient*>(&original)) == 0,
			"A delivering startup stream was stolen before processing became ready");
	}

	void NeutralPassthrough(uint16_t bits = 32, uint16_t formatTag = WAVE_FORMAT_PCM)
	{
		void* table[4] = { nullptr, nullptr, nullptr, reinterpret_cast<void*>(&GetBuffer) };
		FakeCapture capture{ table };
		Audio::DelayLinePitchShifter shifter(0);
		Audio::AsioHook::configuredInputs[0] = true;
		Audio::AsioHook::SetProcessor(0, &shifter);
		Attach(capture, 48000, bits, formatTag);
		Audio::AsioHook::Poll();
		const auto before = capture.samples;
		Read(capture);
		Require(capture.samples == before, "Zero-shift processing rewrote the original PCM or channels");
		Require(observedPackets == 1, "Neutral passthrough lost input observation");
	}

	void StoppedReadiness()
	{
		void* table[4] = { nullptr, nullptr, nullptr, reinterpret_cast<void*>(&GetBuffer) };
		FakeCapture capture{ table };
		Audio::DelayLinePitchShifter shifter(0);
		Audio::AsioHook::configuredInputs[0] = true;
		Audio::AsioHook::SetProcessor(0, &shifter);
		Attach(capture);
		Audio::AsioHook::Poll();
		Read(capture);
		Require(Audio::AsioHook::IsInputReady(0), "Delivering input is not ready");
		Audio::AsioHook::routeLastBufferTick[0].store(GetTickCount64() - 5000);
		Require(!Audio::AsioHook::IsInputReady(0), "Stopped capture is still reported ready");
		Require(!Audio::AsioHook::IsProcessingEnabled(), "Stopped input is advertised as active processing");
		Read(capture);
		Require(Audio::AsioHook::IsInputReady(0), "Returning packets did not restore liveness");
	}

	class BlockingProcessor final : public Audio::IInputProcessor
	{
	public:
		std::atomic<bool> entered{ false };
		std::atomic<bool> release{ false };
		std::atomic<bool> active{ false };
		unsigned preparations = 0;
		uint32_t sampleRate = 0;

		void Prepare(const Audio::CaptureFormat& format) override
		{
			Require(!active.load(), "Prepare overlapped Process");
			++preparations;
			sampleRate = format.sampleRate;
		}

		bool Process(float*, uint32_t) override
		{
			active.store(true);
			entered.store(true);
			while (!release.load()) std::this_thread::yield();
			active.store(false);
			return false;
		}

		uint32_t GetLatencyFrames() const override { return 0; }
	};

	template<typename Predicate>
	bool WaitUntil(Predicate predicate)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!predicate())
		{
			if (std::chrono::steady_clock::now() >= deadline) return false;
			std::this_thread::yield();
		}
		return true;
	}

	void ConcurrentRebind()
	{
		void* table[4] = { nullptr, nullptr, nullptr, reinterpret_cast<void*>(&GetBuffer) };
		FakeCapture original{ table };
		FakeCapture replacement{ table };
		BlockingProcessor processor;
		Audio::AsioHook::configuredInputs[0] = true;
		Audio::AsioHook::SetProcessor(0, &processor);
		Attach(original);
		Audio::AsioHook::Poll();
		auto callback = std::async(std::launch::async, [&] { Read(original); });
		if (!WaitUntil([&] { return processor.entered.load(); }))
		{
			processor.release.store(true);
			callback.get();
			throw std::runtime_error("Callback did not enter processor");
		}
		// Force the replacement eligibility boundary without sleeping two seconds.
		Audio::AsioHook::routeLastBufferTick[0].store(GetTickCount64() - 5000);
		auto registration = std::async(std::launch::async, [&] { Attach(replacement, 44100); });
		const bool closed = WaitUntil([] { return !Audio::AsioHook::processingGate.IsOpen(); });
		const bool retained = Audio::AsioHook::FindCaptureRoute(reinterpret_cast<IAudioCaptureClient*>(&original)) == 0;
		const bool waiting = registration.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
		// Release before asserting so even a failing test cannot leave a join blocked.
		processor.release.store(true);
		callback.get();
		registration.get();
		Require(closed && retained && waiting, "Rebind published new state while the old callback was using it");
		Audio::AsioHook::Poll();
		Require(processor.preparations == 2 && processor.sampleRate == 44100, "Replacement format was not prepared");
		Read(replacement);
		Require(Audio::AsioHook::IsInputReady(0), "Replacement stream not ready after receiving buffers");
	}

	void MultipleRoutes()
	{
		void* table[4] = { nullptr, nullptr, nullptr, reinterpret_cast<void*>(&GetBuffer) };
		FakeCapture first{ table }, second{ table }, unrelated{ table };
		Audio::DelayLinePitchShifter firstProcessor(0), secondProcessor(0);
		Audio::AsioHook::configuredInputs = { true, true };
		Audio::AsioHook::SetProcessor(0, &firstProcessor);
		Audio::AsioHook::SetProcessor(1, &secondProcessor);
		Attach(first);
		Audio::AsioHook::Poll();
		Require(!Audio::AsioHook::processingGate.IsOpen(), "Enabled before all configured inputs attached");
		Attach(second);
		Audio::AsioHook::Poll();
		Read(first);
		Read(second);
		Attach(unrelated);
		Require(Audio::AsioHook::FindCaptureRoute(reinterpret_cast<IAudioCaptureClient*>(&first)) == 0
			&& Audio::AsioHook::FindCaptureRoute(reinterpret_cast<IAudioCaptureClient*>(&second)) == 1,
			"Additional stream stole a configured player");
		Require(Audio::AsioHook::IsProcessingEnabled(), "Two delivering inputs are not ready");
	}

	void RejectedFormat()
	{
		void* table[4] = { nullptr, nullptr, nullptr, reinterpret_cast<void*>(&GetBuffer) };
		FakeCapture original{ table }, invalid{ table };
		Audio::DelayLinePitchShifter processor(0);
		Audio::AsioHook::configuredInputs[0] = true;
		Audio::AsioHook::SetProcessor(0, &processor);
		Attach(original);
		Audio::AsioHook::Poll();
		Attach(invalid, 48000, 8);
		Require(Audio::AsioHook::FindCaptureRoute(reinterpret_cast<IAudioCaptureClient*>(&original)) == 0,
			"Unsupported newcomer destroyed the previous binding");
		Read(original);
		Require(Audio::AsioHook::IsInputReady(0), "Valid original cannot resume after rejected format");
	}

	void BufferResults()
	{
		void* table[4] = { nullptr, nullptr, nullptr, reinterpret_cast<void*>(&GetBuffer) };
		FakeCapture capture{ table };
		Audio::DelayLinePitchShifter processor(0);
		Audio::AsioHook::configuredInputs[0] = true;
		Audio::AsioHook::SetProcessor(0, &processor);
		Attach(capture);
		Audio::AsioHook::Poll();
		for (HRESULT result : { E_FAIL, AUDCLNT_E_DEVICE_INVALIDATED, AUDCLNT_S_BUFFER_EMPTY })
		{
			capture.result = result;
			capture.frames = 0;
			BYTE* data = nullptr;
			UINT32 frames = 55;
			DWORD flags = 55;
			const HRESULT returned = Audio::AsioHook::Hook_CaptureGetBuffer(
				reinterpret_cast<IAudioCaptureClient*>(&capture), &data, &frames, &flags, nullptr, nullptr);
			Require(returned == result && frames == 0 && flags == 0, "Backend error/empty result was changed");
		}
		Require(capture.calls == 3 && observedPackets == 0, "Error/empty packet was processed or fetched twice");
		capture.result = S_OK;
		capture.frames = 4;
		capture.flags = AUDCLNT_BUFFERFLAGS_SILENT | AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
		const auto before = capture.samples;
		Read(capture);
		Require(capture.samples == before, "Silent buffer memory was changed without a source");
	}

	void ActiveShift()
	{
		void* table[4] = { nullptr, nullptr, nullptr, reinterpret_cast<void*>(&GetBuffer) };
		FakeCapture capture{ table };
		Audio::DelayLinePitchShifter processor(-1);
		Audio::AsioHook::configuredInputs[0] = true;
		Audio::AsioHook::SetProcessor(0, &processor);
		Attach(capture);
		Audio::AsioHook::Poll();
		const auto before = capture.samples;
		Read(capture);
		Require(capture.samples != before, "Active shifter output was discarded");
		Require(observedPackets == 1, "Active shift lost observation");
	}

	void ConcurrentPlayers()
	{
		void* table[4] = { nullptr, nullptr, nullptr, reinterpret_cast<void*>(&GetBuffer) };
		FakeCapture first{ table }, second{ table };
		BlockingProcessor firstProcessor, secondProcessor;
		Audio::AsioHook::configuredInputs = { true, true };
		Audio::AsioHook::SetProcessor(0, &firstProcessor);
		Audio::AsioHook::SetProcessor(1, &secondProcessor);
		Attach(first);
		Attach(second);
		Audio::AsioHook::Poll();
		auto firstCallback = std::async(std::launch::async, [&] { Read(first); });
		auto secondCallback = std::async(std::launch::async, [&] { Read(second); });
		const bool bothEntered = WaitUntil([&] { return firstProcessor.entered.load() && secondProcessor.entered.load(); });
		firstProcessor.release.store(true);
		secondProcessor.release.store(true);
		firstCallback.get();
		secondCallback.get();
		Require(bothEntered, "Concurrent player callbacks blocked or suppressed each other");
	}

	void LateAttachment()
	{
		void* table[4] = { nullptr, nullptr, nullptr, reinterpret_cast<void*>(&GetBuffer) };
		FakeCapture capture{ table };
		Audio::DelayLinePitchShifter processor(0);
		Audio::AsioHook::configuredInputs[0] = true;
		Audio::AsioHook::SetProcessor(0, &processor);
		Audio::AsioHook::Poll();
		Require(!Audio::AsioHook::IsInputReady(0), "Input ready before attachment");
		Attach(capture);
		Read(capture);
		Require(!Audio::AsioHook::IsInputReady(0), "Input ready before preparation");
		Audio::AsioHook::Poll();
		Require(Audio::AsioHook::IsInputReady(0), "Late attachment did not become ready");
		Attach(capture);
		Require(Audio::AsioHook::IsInputReady(0), "Duplicate attachment reset a live input");
	}
}

// Test boundaries: patch only the fake writable vtable; replace observers with counters.
// No game memory, hardware, installed INI files, or drivers are accessed.
bool MemUtil::PatchAdr(LPVOID address, LPVOID replacement, size_t length)
{
	std::memcpy(address, replacement, length);
	return true;
}

namespace Audio::CableInput
{
	bool IsAsioPath() { return true; }
	void ReportTapPacket(float, bool, uint32_t, uint32_t) { ++AudioCaptureTests::observedPackets; }
	void ReportMeasuredInputRaw(int64_t, uint32_t) {}
	void ReportMeasuredInputAge(double) {}
}

void RawPitchVerifier::Observe(uint32_t, const float*, uint32_t, uint32_t) {}
void MlAudioExporter::Observe(uint32_t, const float*, uint32_t, uint32_t) {}

// The repository precompiled-header graph registers MIDI commands at static startup.
// Their addresses are required to link; invoking them is a test failure.
namespace Midi::Digitech::WhammyDT { void AutoTuning(int, float) { throw std::logic_error("Unexpected MIDI access"); } }
namespace Midi::Digitech::BassWhammy { void AutoTuningAndTrueTuning(int, float) { throw std::logic_error("Unexpected MIDI access"); } }
namespace Midi::Digitech::WhammyFour { void AutoTuningAndTrueTuning(int, float) { throw std::logic_error("Unexpected MIDI access"); } }
namespace Midi::Digitech::WhammyFive { void AutoTuningAndTrueTuning(int, float) { throw std::logic_error("Unexpected MIDI access"); } }
namespace Midi::Software { void AutoTuning(int, float) { throw std::logic_error("Unexpected MIDI access"); } }

int main(int argc, char** argv)
{
	try
	{
		const std::string test = argc > 1 ? argv[1] : "startup-liveness";
		if (test == "startup-liveness") AudioCaptureTests::StartupLiveness();
		else if (test == "neutral-passthrough") AudioCaptureTests::NeutralPassthrough();
		else if (test == "neutral-int16") AudioCaptureTests::NeutralPassthrough(16);
		else if (test == "neutral-int24") AudioCaptureTests::NeutralPassthrough(24);
		else if (test == "neutral-float32") AudioCaptureTests::NeutralPassthrough(32, WAVE_FORMAT_IEEE_FLOAT);
		else if (test == "stopped-readiness") AudioCaptureTests::StoppedReadiness();
		else if (test == "concurrent-rebind") AudioCaptureTests::ConcurrentRebind();
		else if (test == "multiple-routes") AudioCaptureTests::MultipleRoutes();
		else if (test == "rejected-format") AudioCaptureTests::RejectedFormat();
		else if (test == "buffer-results") AudioCaptureTests::BufferResults();
		else if (test == "active-shift") AudioCaptureTests::ActiveShift();
		else if (test == "concurrent-players") AudioCaptureTests::ConcurrentPlayers();
		else if (test == "late-attachment") AudioCaptureTests::LateAttachment();
		else if (test == "lifecycle-forwarding") AudioCaptureTests::LifecycleForwarding();
		else if (test == "lifecycle-overflow") AudioCaptureTests::LifecycleOverflow();
		else throw std::invalid_argument("Unknown test");
		std::cout << "PASS " << test << '\n';
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
