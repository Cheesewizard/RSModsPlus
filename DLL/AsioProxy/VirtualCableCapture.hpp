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
	class VirtualCableCapture final
	{
	public:
		VirtualCableCapture();
		~VirtualCableCapture();
		VirtualCableCapture(const VirtualCableCapture&) = delete;
		VirtualCableCapture& operator=(const VirtualCableCapture&) = delete;

		bool Start();
		void Stop();
		void SetTestMode(bool enabled) { testMode.store(enabled, std::memory_order_release); }
		void Read(int32_t* stereoSamples, uint32_t frames);
		void InjectForTest(const int32_t* stereoSamples, uint32_t frames);
		bool HasPhysicalCapture() const { return physicalCapture.load(std::memory_order_acquire); }

	private:
		static constexpr uint32_t RING_FRAMES = 16384;
		static constexpr uint32_t MAX_PACKET_FRAMES = 4096;

		void Run();
		bool OpenDevice();
		void CloseDevice();
		bool DrainDevice();
		void Push(const int32_t* stereoSamples, uint32_t frames);
		void PushFloat(const float* samples, uint32_t frames, bool silent);
		void Clear();

		HANDLE quitEvent = nullptr;
		HANDLE wakeEvent = nullptr;
		HANDLE captureEvent = nullptr;
		std::thread worker;
		std::atomic<bool> running{ false };
		std::atomic<bool> physicalCapture{ false };
		std::atomic<bool> testMode{ false };
		std::atomic_flag ringBusy = ATOMIC_FLAG_INIT;
		std::atomic<uint64_t> readFrame{ 0 };
		std::atomic<uint64_t> writeFrame{ 0 };
		std::array<int32_t, RING_FRAMES * 2> ring{};
		std::array<int32_t, MAX_PACKET_FRAMES * 2> packet{};
		Microsoft::WRL::ComPtr<IAudioClient> backend;
		Microsoft::WRL::ComPtr<IAudioCaptureClient> capture;
		std::wstring activeEndpoint;
		uint64_t nextOpenTick = 0;
	};
}
