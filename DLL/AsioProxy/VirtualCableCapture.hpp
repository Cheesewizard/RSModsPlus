#pragma once

#include <Windows.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace AsioProxy
{
	// Rocksmith cable input for the proxy's Virtual mode (no real ASIO device bound).
	//
	// A capture thread drains the cable's WASAPI shared stream (10 ms packets on the cable's own clock)
	// into a lock-free single-producer / single-consumer ring. The proxy's virtual clock reads it in
	// host-sized blocks (Read, audio thread). The two clocks never match exactly, so the reader keeps a
	// small safety margin buffered and trims the drift with a gentle fractional resampler (a few hundred
	// ppm at most, far below audible pitch change).
	//
	// 2026-09-24: this replaced a spinlocked ring with no margin and no drift control. Two failure modes
	// produced whole 128-frame silent blocks in the guitar signal, heard through an in-game amp as random
	// bursts: a read colliding with a push handed the game silence, and the unmanaged fill ran dry just
	// before a packet arrived. The same missing control let latency creep up when the cable ran fast.
	// See docs/designs/speaker-mode-virtual-clock-2026-09-24.md.
	class VirtualCableCapture final
	{
	public:
		using LogFn = void(*)(const char* format, ...);

		struct Stats
		{
			uint32_t underruns = 0;        // reader ran dry after priming: one silent block each
			uint32_t overflowDrops = 0;    // writer found the ring full and dropped a packet
			uint32_t skips = 0;            // reader jumped forward over a backlog
			uint32_t deviceGlitches = 0;   // WASAPI reported a data discontinuity (capture thread late)
			int32_t fillFrames = 0;        // frames buffered at the last read
			int32_t correctionPpm = 0;     // drift correction currently applied
			int32_t primed = 0;
			int32_t marginFrames = 0;      // current safety margin (grows after an underrun)
		};

		VirtualCableCapture();
		~VirtualCableCapture();
		VirtualCableCapture(const VirtualCableCapture&) = delete;
		VirtualCableCapture& operator=(const VirtualCableCapture&) = delete;

		bool Start();
		void Stop();
		void SetTestMode(bool enabled) { testMode.store(enabled, std::memory_order_release); }
		void SetLog(LogFn log) { logFn = log; }
		// Audio thread: produce `frames` interleaved stereo int32 samples (mono cable duplicated).
		void Read(int32_t* stereoSamples, uint32_t frames);
		void InjectForTest(const int32_t* stereoSamples, uint32_t frames);
		bool HasPhysicalCapture() const { return physicalCapture.load(std::memory_order_acquire); }
		Stats GetStats() const;

	private:
		static constexpr uint32_t RING_FRAMES = 16384;           // power of two
		static constexpr uint32_t RING_MASK = RING_FRAMES - 1;
		static constexpr uint32_t MAX_PACKET_FRAMES = 4096;
		static constexpr int32_t MARGIN_FRAMES = 128;            // starting margin over one block at the fill minimum (2.7 ms)
		static constexpr int32_t MARGIN_STEP_FRAMES = 128;       // added after each underrun
		static constexpr int32_t MARGIN_MAX_FRAMES = 2400;       // 50 ms ceiling
		static constexpr int32_t FADE_FRAMES = 64;
		static constexpr uint32_t SERVO_WINDOW_FRAMES = 12000;   // 250 ms of output per servo update

		void Run();
		bool OpenDevice();
		void CloseDevice();
		bool DrainDevice();
		void PushMono(const float* samples, uint32_t frames);
		void ResetReader();
		void LogStatsIfChanged();
		float At(uint64_t index) const { return ring[static_cast<uint32_t>(index) & RING_MASK]; }

		HANDLE quitEvent = nullptr;
		HANDLE wakeEvent = nullptr;
		HANDLE captureEvent = nullptr;
		std::thread worker;
		std::atomic<bool> running{ false };
		std::atomic<bool> physicalCapture{ false };
		std::atomic<bool> testMode{ false };
		LogFn logFn = nullptr;

		// Ring: the writer owns writeFrame, the reader owns readFrame. No lock.
		std::atomic<uint64_t> readFrame{ 0 };
		std::atomic<uint64_t> writeFrame{ 0 };
		std::atomic<uint32_t> largestPacket{ 0 };
		uint32_t packetWindowMax = 0;     // writer only
		uint32_t packetWindowCount = 0;   // writer only
		std::atomic<bool> resetRequested{ true };
		std::array<float, RING_FRAMES> ring{};
		std::array<float, MAX_PACKET_FRAMES> packet{};

		// Reader state (audio thread only).
		uint64_t readIndex = 0;       // integer part of the read position
		double readPhase = 0.0;       // fractional part, [0, 1)
		bool primed = false;
		int32_t fadeIn = 0;
		int32_t margin = MARGIN_FRAMES;   // adaptive: a writer that is late more than the margin grows it
		int32_t windowMinFill = INT32_MAX;
		uint32_t windowFrames = 0;
		double integralPpm = 0.0;
		double correctionPpm = 0.0;

		// Published stats.
		std::atomic<uint32_t> underruns{ 0 };
		std::atomic<uint32_t> overflowDrops{ 0 };
		std::atomic<uint32_t> skips{ 0 };
		std::atomic<uint32_t> deviceGlitches{ 0 };
		std::atomic<int32_t> lastFill{ 0 };
		std::atomic<int32_t> lastPpm{ 0 };
		std::atomic<int32_t> primedFlag{ 0 };
		std::atomic<int32_t> lastMargin{ MARGIN_FRAMES };
		Stats lastLogged{};
		uint64_t nextLogTick = 0;

		Microsoft::WRL::ComPtr<IAudioClient> backend;
		Microsoft::WRL::ComPtr<IAudioCaptureClient> capture;
		std::wstring activeEndpoint;
		uint64_t nextOpenTick = 0;
	};
}
