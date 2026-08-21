#include "stdafx.h"
#include "AsioHook.hpp"
#include "ComVTable.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Audio::AsioHook
{
	namespace
	{
		constexpr size_t SLOT_CAPTURE_CLIENT_GET_BUFFER = 3;
		constexpr uint32_t MAX_BUFFER_FRAMES = 4096;
		constexpr float INT16_TO_FLOAT = 1.0f / 32768.0f;
		constexpr float INT24_TO_FLOAT = 1.0f / 8388608.0f;
		constexpr float INT32_TO_FLOAT = 1.0f / 2147483648.0f;
		constexpr char RS_ASIO_VERSION_MARKER[] = "Wrapper DLL loaded (v0.7.5)";

		constexpr std::array<uint8_t, 11> UNMARSHAL_CALL_PRE_PATCH =
		{
			0xe8, 0x59, 0xdf, 0xff, 0xff, 0x57, 0xe8, 0x33, 0xe0, 0xff, 0xff
		};

		constexpr std::array<uint8_t, 11> UNMARSHAL_CALL_POST_PATCH =
		{
			0xe8, 0x97, 0xe2, 0xff, 0xff, 0x57, 0xe8, 0x71, 0xe3, 0xff, 0xff
		};

		constexpr std::array<uint8_t, 11> UNMARSHAL_CALL_LEARN_AND_PLAY =
		{
			0xe8, 0xd6, 0xfd, 0xff, 0xff, 0x56, 0xe8, 0xb0, 0xfe, 0xff, 0xff
		};

		const GUID PCM_SUBFORMAT =
		{
			0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
		};

		const GUID FLOAT_SUBFORMAT =
		{
			0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
		};

		struct RsAsioConfiguration
		{
			std::array<int, INPUT_ROUTE_COUNT> inputChannels{ 0, -1 };
			std::array<bool, INPUT_ROUTE_COUNT> inputConfigured{ false, false };
			std::array<std::string, INPUT_ROUTE_COUNT> inputSources;
			std::array<bool, INPUT_ROUTE_COUNT> inputInferred{ false, false };
		};

		struct PaWasapiSubStreamPrefix
		{
			IUnknown* clientParent;
			IUnknown* clientStream;
			IUnknown* clientProc;
			WAVEFORMATEXTENSIBLE waveFormat;
			uint8_t remaining[0xdc - sizeof(WAVEFORMATEXTENSIBLE)];
		};

		struct PaWasapiStreamPrefix
		{
			uint8_t prefix[0x108];
			PaWasapiSubStreamPrefix input;
			IUnknown* captureClientParent;
			IUnknown* captureClientStream;
			IAudioCaptureClient* captureClient;
		};

		static_assert(sizeof(void*) == 4, "RSModsPlus and Rocksmith require a 32-bit build");
		static_assert(offsetof(PaWasapiStreamPrefix, input.waveFormat) == 0x114, "Unexpected PortAudio input format offset");
		static_assert(offsetof(PaWasapiStreamPrefix, captureClientParent) == 0x1f0, "Unexpected PortAudio capture parent offset");
		static_assert(offsetof(PaWasapiStreamPrefix, captureClient) == 0x1f8, "Unexpected PortAudio capture client offset");

		using UnmarshalStreamComPointers_t = HRESULT(__cdecl*)(void*);
		using CaptureGetBuffer_t = HRESULT(STDMETHODCALLTYPE*)(
			IAudioCaptureClient*, BYTE**, UINT32*, DWORD*, UINT64*, UINT64*);

		std::array<CaptureFormat, INPUT_ROUTE_COUNT> routeFormats;
		std::array<std::vector<float>, INPUT_ROUTE_COUNT> conversionBuffers;
		std::array<std::atomic<IInputProcessor*>, INPUT_ROUTE_COUNT> activeProcessors;
		std::array<std::atomic<IAudioCaptureClient*>, INPUT_ROUTE_COUNT> routeCaptureClients;
		std::array<int, INPUT_ROUTE_COUNT> selectedInputChannels{ -1, -1 };
		std::array<bool, INPUT_ROUTE_COUNT> configuredInputs{ false, false };
		std::array<std::atomic<bool>, INPUT_ROUTE_COUNT> inputReady;
		std::atomic<bool> bufferLayoutReady{ false };
		std::atomic<bool> processingEnabled{ false };
		std::atomic<bool> autoEnabledOnce{ false };
		std::mutex registrationMutex;

		UnmarshalStreamComPointers_t originalUnmarshalStreamComPointers = nullptr;
		CaptureGetBuffer_t originalCaptureGetBuffer = nullptr;
		void** captureClientVTable = nullptr;

		float ClampSample(float value)
		{
			if (!std::isfinite(value)) return 0.0f;
			return std::clamp(value, -1.0f, 1.0f);
		}

		template<typename SampleType, int BIT_DEPTH>
		SampleType FloatToSignedInteger(float value)
		{
			static_assert(std::is_signed<SampleType>::value, "SampleType must be signed");
			constexpr int64_t magnitude = int64_t{ 1 } << (BIT_DEPTH - 1);
			const float clamped = ClampSample(value);
			if (clamped <= -1.0f) return static_cast<SampleType>(-magnitude);
			if (clamped >= 1.0f) return static_cast<SampleType>(magnitude - 1);
			return static_cast<SampleType>(clamped * static_cast<float>(magnitude));
		}

		int32_t ReadInt24(const uint8_t* sample)
		{
			const uint32_t packed = static_cast<uint32_t>(sample[0])
				| (static_cast<uint32_t>(sample[1]) << 8)
				| (static_cast<uint32_t>(sample[2]) << 16);

			return (packed & 0x00800000u) != 0
				? static_cast<int32_t>(packed) - 0x01000000
				: static_cast<int32_t>(packed);
		}

		void WriteInt24(int32_t value, uint8_t* sample)
		{
			const uint32_t packed = static_cast<uint32_t>(value);
			sample[0] = static_cast<uint8_t>(packed);
			sample[1] = static_cast<uint8_t>(packed >> 8);
			sample[2] = static_cast<uint8_t>(packed >> 16);
		}

		RsAsioConfiguration ReadRsAsioConfiguration()
		{
			CSimpleIniA reader;
			if (reader.LoadFile("RS_ASIO.ini") < 0) return {};

			RsAsioConfiguration configuration;
			const char* inputZeroDriver = reader.GetValue("Asio.Input.0", "Driver", "");
			const char* inputOneDriver = reader.GetValue("Asio.Input.1", "Driver", "");
			const bool hasInputZero = inputZeroDriver && *inputZeroDriver;
			const bool hasInputOne = inputOneDriver && *inputOneDriver;

			if (hasInputZero)
			{
				configuration.inputConfigured[0] = true;
				configuration.inputChannels[0] = static_cast<int>(reader.GetLongValue("Asio.Input.0", "Channel", 0));
				configuration.inputSources[0] = "[Asio.Input.0]";

				if (hasInputOne)
				{
					configuration.inputConfigured[1] = true;
					configuration.inputChannels[1] = static_cast<int>(reader.GetLongValue("Asio.Input.1", "Channel", -1));
					configuration.inputSources[1] = "[Asio.Input.1]";
				}
			}
			else if (hasInputOne)
			{
				configuration.inputConfigured[0] = true;
				configuration.inputChannels[0] = static_cast<int>(reader.GetLongValue("Asio.Input.1", "Channel", 0));
				configuration.inputSources[0] = "[Asio.Input.1], the only configured input section";
				configuration.inputInferred[0] = true;
			}
			else
			{
				const char* outputDriver = reader.GetValue("Asio.Output", "Driver", "");
				if (outputDriver && *outputDriver)
				{
					configuration.inputConfigured[0] = true;
					configuration.inputChannels[0] = 0;
					configuration.inputSources[0] = "[Asio.Output] driver, assuming channel 0";
					configuration.inputInferred[0] = true;
				}
			}

			return configuration;
		}

		bool IsRangeInsideModule(const void* address, size_t size, const MODULEINFO& moduleInfo)
		{
			const uintptr_t start = reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll);
			const uintptr_t end = start + moduleInfo.SizeOfImage;
			const uintptr_t rangeStart = reinterpret_cast<uintptr_t>(address);
			return rangeStart >= start && rangeStart <= end && size <= end - rangeStart;
		}

		bool ModuleContainsMarker(const MODULEINFO& moduleInfo, const char* marker)
		{
			const auto* start = static_cast<const char*>(moduleInfo.lpBaseOfDll);
			const auto* end = start + moduleInfo.SizeOfImage;
			const size_t markerLength = std::strlen(marker);
			return std::search(start, end, marker, marker + markerLength) != end;
		}

		template<size_t SIZE>
		void FindPattern(const MODULEINFO& moduleInfo, const std::array<uint8_t, SIZE>& pattern, std::vector<uint8_t*>& matches)
		{
			auto* start = static_cast<uint8_t*>(moduleInfo.lpBaseOfDll);
			auto* end = start + moduleInfo.SizeOfImage;
			auto* cursor = start;

			while (cursor < end)
			{
				cursor = std::search(cursor, end, pattern.begin(), pattern.end());
				if (cursor == end) return;
				matches.push_back(cursor);
				++cursor;
			}
		}

		bool ReplacePatchedTarget(uint8_t* target, void* replacement)
		{
			return MemUtil::PatchAdr(target + 1, &replacement, sizeof(replacement));
		}

		CaptureFormat ReadCaptureFormat(const WAVEFORMATEXTENSIBLE& waveFormat)
		{
			const WAVEFORMATEX& baseFormat = waveFormat.Format;
			CaptureFormat format;
			format.sampleRate = baseFormat.nSamplesPerSec;
			format.channelCount = baseFormat.nChannels;

			if (baseFormat.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
			{
				if (baseFormat.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return {};

				if (IsEqualGUID(waveFormat.SubFormat, FLOAT_SUBFORMAT) && baseFormat.wBitsPerSample == 32)
					format.sampleFormat = SampleFormat::Float32;
				else if (IsEqualGUID(waveFormat.SubFormat, PCM_SUBFORMAT))
				{
					switch (baseFormat.wBitsPerSample)
					{
					case 32: format.sampleFormat = SampleFormat::Int32; break;
					case 24: format.sampleFormat = SampleFormat::Int24; break;
					case 16: format.sampleFormat = SampleFormat::Int16; break;
					}
				}
			}
			else if (baseFormat.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && baseFormat.wBitsPerSample == 32)
			{
				format.sampleFormat = SampleFormat::Float32;
			}
			else if (baseFormat.wFormatTag == WAVE_FORMAT_PCM)
			{
				switch (baseFormat.wBitsPerSample)
				{
				case 32: format.sampleFormat = SampleFormat::Int32; break;
				case 24: format.sampleFormat = SampleFormat::Int24; break;
				case 16: format.sampleFormat = SampleFormat::Int16; break;
				}
			}

			const uint32_t bytesPerSample = baseFormat.wBitsPerSample / 8;
			if (!format.IsUsable() || bytesPerSample == 0
				|| baseFormat.nBlockAlign != format.channelCount * bytesPerSample)
				return {};

			return format;
		}

		bool CopyFirstChannelToFloat(const BYTE* packet, const CaptureFormat& format, uint32_t frameCount, float* output)
		{
			const size_t channelCount = format.channelCount;
			switch (format.sampleFormat)
			{
			case SampleFormat::Float32:
			{
				const float* samples = reinterpret_cast<const float*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					output[frame] = samples[static_cast<size_t>(frame) * channelCount];
				return true;
			}
			case SampleFormat::Int32:
			{
				const int32_t* samples = reinterpret_cast<const int32_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					output[frame] = static_cast<float>(samples[static_cast<size_t>(frame) * channelCount]) * INT32_TO_FLOAT;
				return true;
			}
			case SampleFormat::Int24:
			{
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const size_t sampleIndex = static_cast<size_t>(frame) * channelCount;
					output[frame] = static_cast<float>(ReadInt24(packet + sampleIndex * 3)) * INT24_TO_FLOAT;
				}
				return true;
			}
			case SampleFormat::Int16:
			{
				const int16_t* samples = reinterpret_cast<const int16_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
					output[frame] = static_cast<float>(samples[static_cast<size_t>(frame) * channelCount]) * INT16_TO_FLOAT;
				return true;
			}
			default:
				return false;
			}
		}

		void CopyFloatToAllChannels(BYTE* packet, const CaptureFormat& format, uint32_t frameCount, const float* converted)
		{
			const size_t channelCount = format.channelCount;
			switch (format.sampleFormat)
			{
			case SampleFormat::Float32:
			{
				float* samples = reinterpret_cast<float*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const float value = ClampSample(converted[frame]);
					for (size_t channel = 0; channel < channelCount; ++channel)
						samples[static_cast<size_t>(frame) * channelCount + channel] = value;
				}
				break;
			}
			case SampleFormat::Int32:
			{
				int32_t* samples = reinterpret_cast<int32_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const int32_t value = FloatToSignedInteger<int32_t, 32>(converted[frame]);
					for (size_t channel = 0; channel < channelCount; ++channel)
						samples[static_cast<size_t>(frame) * channelCount + channel] = value;
				}
				break;
			}
			case SampleFormat::Int24:
			{
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const int32_t value = FloatToSignedInteger<int32_t, 24>(converted[frame]);
					for (size_t channel = 0; channel < channelCount; ++channel)
					{
						const size_t sampleIndex = static_cast<size_t>(frame) * channelCount + channel;
						WriteInt24(value, packet + sampleIndex * 3);
					}
				}
				break;
			}
			case SampleFormat::Int16:
			{
				int16_t* samples = reinterpret_cast<int16_t*>(packet);
				for (uint32_t frame = 0; frame < frameCount; ++frame)
				{
					const int16_t value = FloatToSignedInteger<int16_t, 16>(converted[frame]);
					for (size_t channel = 0; channel < channelCount; ++channel)
						samples[static_cast<size_t>(frame) * channelCount + channel] = value;
				}
				break;
			}
			default:
				break;
			}
		}

		int FindCaptureRoute(IAudioCaptureClient* captureClient)
		{
			for (size_t routeIndex = 0; routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
			{
				if (routeCaptureClients[routeIndex].load(std::memory_order_relaxed) == captureClient)
					return static_cast<int>(routeIndex);
			}
			return -1;
		}

		HRESULT STDMETHODCALLTYPE Hook_CaptureGetBuffer(
			IAudioCaptureClient* self,
			BYTE** data,
			UINT32* frameCount,
			DWORD* flags,
			UINT64* devicePosition,
			UINT64* performanceCounterPosition)
		{
			const HRESULT result = originalCaptureGetBuffer(
				self, data, frameCount, flags, devicePosition, performanceCounterPosition);

			if (FAILED(result) || !processingEnabled.load(std::memory_order_acquire)
				|| !data || !*data || !frameCount || *frameCount == 0 || *frameCount > MAX_BUFFER_FRAMES
				|| (flags && (*flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0))
				return result;

			const int routeIndex = FindCaptureRoute(self);
			if (routeIndex < 0 || !inputReady[routeIndex].load(std::memory_order_acquire)) return result;

			IInputProcessor* processor = activeProcessors[routeIndex].load(std::memory_order_relaxed);
			if (!processor) return result;

			float* converted = conversionBuffers[routeIndex].data();
			if (!CopyFirstChannelToFloat(*data, routeFormats[routeIndex], *frameCount, converted)) return result;

			processor->Process(converted, *frameCount);
			CopyFloatToAllChannels(*data, routeFormats[routeIndex], *frameCount, converted);
			return result;
		}

		void UpdateBufferLayoutReady()
		{
			for (size_t routeIndex = 0; routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
			{
				if (!configuredInputs[routeIndex]) continue;
				if (!routeCaptureClients[routeIndex].load(std::memory_order_acquire)
					|| !routeFormats[routeIndex].IsUsable()) return;
			}
			bufferLayoutReady.store(true, std::memory_order_release);
		}

		void RegisterCaptureStream(PaWasapiStreamPrefix* stream)
		{
			if (!stream || !stream->input.clientParent || !stream->captureClient) return;
			std::lock_guard<std::mutex> guard(registrationMutex);
			if (FindCaptureRoute(stream->captureClient) >= 0) return;

			size_t routeIndex = INPUT_ROUTE_COUNT;
			for (size_t candidate = 0; candidate < INPUT_ROUTE_COUNT; ++candidate)
			{
				if (configuredInputs[candidate]
					&& !routeCaptureClients[candidate].load(std::memory_order_relaxed))
				{
					routeIndex = candidate;
					break;
				}
			}

			if (routeIndex == INPUT_ROUTE_COUNT)
			{
				LOG_WARNING("[AsioHook] Ignoring an additional capture stream after all configured routes were attached." << std::endl);
				return;
			}

			const CaptureFormat format = ReadCaptureFormat(stream->input.waveFormat);
			if (!format.IsUsable())
			{
				const WAVEFORMATEX& waveFormat = stream->input.waveFormat.Format;
				LOG_ERROR("[AsioHook] Player " << routeIndex + 1 << " negotiated an unsupported capture format: tag "
					<< waveFormat.wFormatTag << ", " << waveFormat.nSamplesPerSec << " Hz, "
					<< waveFormat.nChannels << " channel(s), " << waveFormat.wBitsPerSample << " bits." << std::endl);
				return;
			}

			void** vTable = ComVTable::GetVTable(stream->captureClient);
			if (!captureClientVTable)
			{
				originalCaptureGetBuffer = reinterpret_cast<CaptureGetBuffer_t>(
					vTable[SLOT_CAPTURE_CLIENT_GET_BUFFER]);
				void* replacedGetBuffer = ComVTable::PatchSlot(
					stream->captureClient,
					SLOT_CAPTURE_CLIENT_GET_BUFFER,
					Hook_CaptureGetBuffer);

				if (!replacedGetBuffer
					|| replacedGetBuffer != reinterpret_cast<void*>(originalCaptureGetBuffer))
				{
					originalCaptureGetBuffer = nullptr;
					LOG_ERROR("[AsioHook] Could not patch IAudioCaptureClient::GetBuffer." << std::endl);
					return;
				}
				captureClientVTable = vTable;
			}
			else if (captureClientVTable != vTable)
			{
				LOG_ERROR("[AsioHook] Player " << routeIndex + 1
					<< " uses a different capture implementation; processing stays disabled." << std::endl);
				return;
			}

			processingEnabled.store(false, std::memory_order_release);
			inputReady[routeIndex].store(false, std::memory_order_relaxed);
			routeFormats[routeIndex] = format;
			routeCaptureClients[routeIndex].store(stream->captureClient, std::memory_order_release);

			LOG_INFO("[AsioHook] Player " << routeIndex + 1 << " attached to existing RS_ASIO capture client "
				<< stream->captureClient << " using " << DescribeFormat(format) << "." << std::endl);
			UpdateBufferLayoutReady();
		}

		HRESULT __cdecl Hook_UnmarshalStreamComPointers(void* stream)
		{
			const HRESULT result = originalUnmarshalStreamComPointers(stream);
			if (SUCCEEDED(result)) RegisterCaptureStream(reinterpret_cast<PaWasapiStreamPrefix*>(stream));
			return result;
		}

		bool InstallUnmarshalHook()
		{
			HMODULE rsAsioModule = GetModuleHandleA("RS_ASIO.dll");
			if (!rsAsioModule)
			{
				LOG_ERROR("[AsioHook] RS_ASIO.dll is not loaded; late capture cannot be installed." << std::endl);
				return false;
			}

			MODULEINFO rsAsioInfo{};
			if (!GetModuleInformation(GetCurrentProcess(), rsAsioModule, &rsAsioInfo, sizeof(rsAsioInfo)))
			{
				LOG_ERROR("[AsioHook] Could not inspect RS_ASIO.dll, error " << GetLastError() << "." << std::endl);
				return false;
			}

			if (!ModuleContainsMarker(rsAsioInfo, RS_ASIO_VERSION_MARKER))
			{
				LOG_ERROR("[AsioHook] ASIO Drop Pedal requires RS_ASIO v0.7.5; processing stays disabled." << std::endl);
				return false;
			}

			HMODULE gameModule = GetModuleHandleA(nullptr);
			MODULEINFO gameInfo{};
			if (!gameModule || !GetModuleInformation(GetCurrentProcess(), gameModule, &gameInfo, sizeof(gameInfo)))
			{
				LOG_ERROR("[AsioHook] Could not inspect the Rocksmith executable, error " << GetLastError() << "." << std::endl);
				return false;
			}

			std::vector<uint8_t*> callSites;
			FindPattern(gameInfo, UNMARSHAL_CALL_PRE_PATCH, callSites);
			FindPattern(gameInfo, UNMARSHAL_CALL_POST_PATCH, callSites);
			FindPattern(gameInfo, UNMARSHAL_CALL_LEARN_AND_PLAY, callSites);

			if (callSites.size() != 1)
			{
				LOG_ERROR("[AsioHook] Expected one supported PortAudio unmarshal call, found "
					<< callSites.size() << "; processing stays disabled." << std::endl);
				return false;
			}

			uint8_t* callSite = callSites[0];
			const int32_t relativeTarget = *reinterpret_cast<const int32_t*>(callSite + 1);
			uint8_t* patchedTarget = callSite + 5 + relativeTarget;

			if (!IsRangeInsideModule(patchedTarget, 6, gameInfo)
				|| patchedTarget[0] != 0x68 || patchedTarget[5] != 0xc3)
			{
				LOG_ERROR("[AsioHook] RS_ASIO did not own the expected unmarshal target; processing stays disabled." << std::endl);
				return false;
			}

			void* rsAsioUnmarshal = nullptr;
			std::memcpy(&rsAsioUnmarshal, patchedTarget + 1, sizeof(rsAsioUnmarshal));
			if (!IsRangeInsideModule(rsAsioUnmarshal, 1, rsAsioInfo))
			{
				LOG_ERROR("[AsioHook] The existing unmarshal target is outside RS_ASIO.dll; processing stays disabled." << std::endl);
				return false;
			}

			originalUnmarshalStreamComPointers = reinterpret_cast<UnmarshalStreamComPointers_t>(rsAsioUnmarshal);
			if (!ReplacePatchedTarget(patchedTarget, reinterpret_cast<void*>(Hook_UnmarshalStreamComPointers)))
			{
				originalUnmarshalStreamComPointers = nullptr;
				LOG_ERROR("[AsioHook] Could not chain the existing RS_ASIO unmarshal hook." << std::endl);
				return false;
			}

			LOG_INFO("[AsioHook] Attached to RS_ASIO's existing capture path without creating another ASIO host." << std::endl);
			return true;
		}
	}

	void Install()
	{
		static bool installed = false;
		if (installed) return;
		installed = true;

		LOG_INFO("[AsioHook] RS_ASIO v0.7.5 capture integration." << std::endl);
		const RsAsioConfiguration configuration = ReadRsAsioConfiguration();
		bool hasConfiguredInput = false;

		for (size_t routeIndex = 0; routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
		{
			configuredInputs[routeIndex] = configuration.inputConfigured[routeIndex];
			selectedInputChannels[routeIndex] = configuration.inputChannels[routeIndex];

			if (!configuredInputs[routeIndex])
				continue;

			hasConfiguredInput = true;
			if (selectedInputChannels[routeIndex] < 0)
			{
				LOG_ERROR("[AsioHook] Player " << routeIndex + 1
					<< " has no valid Channel in RS_ASIO.ini; processing stays disabled." << std::endl);
				return;
			}

			LOG_INFO("[AsioHook] Player " << routeIndex + 1 << " will follow the existing RS_ASIO endpoint for "
				<< configuration.inputSources[routeIndex] << ", ASIO channel "
				<< selectedInputChannels[routeIndex] << "." << std::endl);

			if (configuration.inputInferred[routeIndex])
			{
				LOG_WARNING("[AsioHook] Player " << routeIndex + 1
					<< " input routing was inferred because no matching [Asio.Input.N] section named a driver." << std::endl);
			}
		}

		if (!hasConfiguredInput)
		{
			LOG_INFO("[AsioHook] No ASIO input is configured in RS_ASIO.ini; nothing to attach." << std::endl);
			return;
		}

		InstallUnmarshalHook();
	}

	void Poll()
	{
		if (autoEnabledOnce.load(std::memory_order_relaxed)) return;
		if (processingEnabled.load(std::memory_order_relaxed)) return;
		if (!bufferLayoutReady.load(std::memory_order_acquire)) return;

		for (size_t routeIndex = 0; routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
		{
			if (!configuredInputs[routeIndex]) continue;
			if (!routeFormats[routeIndex].IsUsable()) return;
			if (!activeProcessors[routeIndex].load(std::memory_order_relaxed)) return;
		}

		autoEnabledOnce.store(true, std::memory_order_relaxed);
		for (size_t routeIndex = 0; routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
		{
			if (!configuredInputs[routeIndex]) continue;

			conversionBuffers[routeIndex].assign(MAX_BUFFER_FRAMES, 0.0f);
			IInputProcessor* processor = activeProcessors[routeIndex].load(std::memory_order_relaxed);
			processor->Prepare(routeFormats[routeIndex]);
			inputReady[routeIndex].store(true, std::memory_order_release);
			LOG_INFO("[AsioHook] Player " << routeIndex + 1 << " processor prepared with "
				<< processor->GetLatencyFrames() << " frames of latency." << std::endl);
		}

		SetProcessingEnabled(true);
	}

	void SetProcessor(size_t routeIndex, IInputProcessor* inputProcessor)
	{
		if (routeIndex >= INPUT_ROUTE_COUNT)
		{
			LOG_ERROR("[AsioHook] Refusing to set processor for invalid route " << routeIndex << "." << std::endl);
			return;
		}
		activeProcessors[routeIndex].store(inputProcessor, std::memory_order_relaxed);
	}

	void SetProcessingEnabled(bool enabled)
	{
		if (enabled && !bufferLayoutReady.load(std::memory_order_acquire))
		{
			LOG_ERROR("[AsioHook] Refusing to enable processing before RS_ASIO capture is attached." << std::endl);
			return;
		}

		if (enabled)
		{
			for (size_t routeIndex = 0; routeIndex < INPUT_ROUTE_COUNT; ++routeIndex)
			{
				if (!configuredInputs[routeIndex]) continue;
				if (!activeProcessors[routeIndex].load(std::memory_order_relaxed)
					|| !routeCaptureClients[routeIndex].load(std::memory_order_acquire)
					|| !routeFormats[routeIndex].IsUsable()
					|| !inputReady[routeIndex].load(std::memory_order_acquire))
				{
					LOG_ERROR("[AsioHook] Refusing to enable processing because Player "
						<< routeIndex + 1 << " is not ready." << std::endl);
					return;
				}
			}
		}

		processingEnabled.store(enabled, std::memory_order_release);
		LOG_INFO("[AsioHook] Processing " << (enabled ? "enabled" : "disabled") << std::endl);
	}

	bool IsProcessingEnabled()
	{
		return processingEnabled.load(std::memory_order_acquire);
	}

	bool IsInputConfigured(size_t routeIndex)
	{
		return routeIndex < INPUT_ROUTE_COUNT && configuredInputs[routeIndex];
	}

	bool IsInputReady(size_t routeIndex)
	{
		return routeIndex < INPUT_ROUTE_COUNT && inputReady[routeIndex].load(std::memory_order_acquire);
	}
}

namespace Audio
{
	std::string DescribeFormat(const CaptureFormat& format)
	{
		const char* sampleType = "unsupported";
		switch (format.sampleFormat)
		{
		case SampleFormat::Float32: sampleType = "32-bit float"; break;
		case SampleFormat::Int32: sampleType = "32-bit PCM"; break;
		case SampleFormat::Int24: sampleType = "24-bit PCM"; break;
		case SampleFormat::Int16: sampleType = "16-bit PCM"; break;
		default: break;
		}

		return std::to_string(format.channelCount) + " channel(s), "
			+ std::to_string(format.sampleRate) + " Hz, " + sampleType;
	}
}
