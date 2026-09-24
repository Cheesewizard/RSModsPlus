// Automated test for the Rocksmith Audio Bridge proxy ASIO driver. No game, no admin, no hardware.
//
// Registers a stub driver in HKCU, selects it through test-process environment, loads the proxy exactly
// as RS_ASIO does (DllGetClassObject on the proxy CLSID), and drives a few buffer switches. Two scenarios:
//   1. Sink set    -> the tap must receive the exact interleaved ramp the host wrote (recording path).
//   2. No sink     -> forwarding must still happen (host bufferSwitch fires) and nothing is tapped
//                     (everyday playback with recording off must not break or leak).
// Exit code 0 = both pass. Build + run via run-tests.sh.
#include "../AsioInterface.h"
#include "../LatencyDsp.h"
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <cmath>
#pragma comment(lib, "winmm.lib")

// {7B2E5C10-9F3A-4D6B-A1C8-2E4F6A8B0D31} - the proxy under test.
static const CLSID CLSID_Proxy =
	{ 0x7b2e5c10, 0x9f3a, 0x4d6b, { 0xa1, 0xc8, 0x2e, 0x4f, 0x6a, 0x8b, 0x0d, 0x31 } };
static const wchar_t* kStubName = L"RSMods Stub ASIO";
static const wchar_t* kStubClsid = L"{A1B2C3D4-1111-4111-8111-111111111111}";
static const long kFrames = 128;
static const long kOutChannels = 2;

static int32_t Expected(long seq, long frame, long ch) { return (seq * 1000000) + (frame * 10) + ch; }

// g_curSeq is advanced inside bufferSwitch (driver thread) and read by the tap in the same OnBufferSwitch
// call, so host-write and sink-verify always agree. The main thread never touches it.
static long g_curSeq = 0;
static std::atomic<int> g_hostSwitches{ 0 };   // proves the proxy forwarded the callback to us
static std::atomic<int> g_blocks{ 0 };         // proves the tap fired
static std::atomic<int> g_mismatch{ 0 };
static ASIOBufferInfo* g_infos = nullptr;      // [0,1]=in, [2,3]=out

static void FillOutputs(long index)
{
	for (long ch = 0; ch < kOutChannels; ++ch)
	{
		auto* buf = reinterpret_cast<int32_t*>(g_infos[2 + ch].buffers[index]);
		for (long f = 0; f < kFrames; ++f) buf[f] = Expected(g_curSeq, f, ch);
	}
}
static void HostBufferSwitch(long index, ASIOBool) { g_hostSwitches.fetch_add(1); ++g_curSeq; FillOutputs(index); }
static long HostAsioMessage(long, long, void*, double*) { return 0; }

static void __cdecl TapSink(const void* interleaved, long frames, long channels, long asioType, double)
{
	g_blocks.fetch_add(1);
	if (frames != kFrames || channels != kOutChannels || asioType != ASIOSTInt32LSB) { g_mismatch.fetch_add(1); return; }
	const int32_t* s = reinterpret_cast<const int32_t*>(interleaved);
	for (long f = 0; f < frames; ++f)
		for (long ch = 0; ch < channels; ++ch)
			if (s[f * channels + ch] != Expected(g_curSeq, f, ch)) { g_mismatch.fetch_add(1); return; }
}

// Probe scenario: accumulate output channel 0 (int32 -> float) so the DSP can locate the injected probe.
static std::vector<float> g_capture;
static void __cdecl CaptureSink(const void* interleaved, long frames, long channels, long asioType, double)
{
	if (asioType != ASIOSTInt32LSB || channels < 1) return;
	const int32_t* s = reinterpret_cast<const int32_t*>(interleaved);
	for (long f = 0; f < frames; ++f) g_capture.push_back(s[f * channels] / 2147483648.0f);
}

// Limiter scenario: with a below-ceiling signal the make-up gain should scale the tapped output by exactly
// that factor, proving the proxy's per-sample read -> gain -> write path is correct for the output format.
static float g_scaleFactor = 1.0f;
static std::atomic<int> g_scaleBlocks{ 0 };
static std::atomic<int> g_scaleBad{ 0 };
static void __cdecl ScaleSink(const void* interleaved, long frames, long channels, long asioType, double)
{
	g_scaleBlocks.fetch_add(1);
	if (asioType != ASIOSTInt32LSB) { g_scaleBad.fetch_add(1); return; }
	const int32_t* s = reinterpret_cast<const int32_t*>(interleaved);
	for (long f = 0; f < frames; ++f)
		for (long ch = 0; ch < channels; ++ch)
		{
			const double expect = static_cast<double>(Expected(g_curSeq, f, ch)) * g_scaleFactor;
			const int diff = s[f * channels + ch] - static_cast<int32_t>(expect < 0 ? expect - 0.5 : expect + 0.5);
			if (diff > 8 || diff < -8) { g_scaleBad.fetch_add(1); return; }
		}
}

// --- HKCU registration so the proxy can resolve the stub (32-bit process => WOW6432Node) ---
static bool SetKey(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* value)
{
	HKEY k = nullptr;
	if (RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
	const LSTATUS st = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
		static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
	RegCloseKey(k);
	return st == ERROR_SUCCESS;
}
static void RegisterStub(const std::wstring& stubDllPath)
{
	std::wstring inproc = std::wstring(L"Software\\Classes\\CLSID\\") + kStubClsid + L"\\InprocServer32";
	SetKey(HKEY_CURRENT_USER, inproc.c_str(), nullptr, stubDllPath.c_str());
	SetKey(HKEY_CURRENT_USER, inproc.c_str(), L"ThreadingModel", L"Apartment");
	SetKey(HKEY_CURRENT_USER, (std::wstring(L"Software\\ASIO\\") + kStubName).c_str(), L"CLSID", kStubClsid);
}
static void UnregisterStub()
{
	RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\ASIO\\") + kStubName).c_str());
	RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\CLSID\\") + kStubClsid).c_str());
}

// A second sink for the multi-out scenario: just count blocks and sanity-check shape, to prove the same
// output reaches more than one consumer.
static std::atomic<int> g_secondBlocks{ 0 };
static void __cdecl SecondSink(const void* interleaved, long frames, long channels, long asioType, double)
{
	(void)interleaved;
	if (frames == kFrames && channels == kOutChannels && asioType == ASIOSTInt32LSB) g_secondBlocks.fetch_add(1);
}

static ASIOBufferInfo* g_virtualInfos = nullptr;
static long g_virtualInfoCount = 0;
static std::atomic<int> g_virtualSwitches{ 0 };
static std::atomic<int> g_virtualBlocks{ 0 };
static std::atomic<int> g_virtualInputNonZero{ 0 };
static void VirtualHostBufferSwitch(long index, ASIOBool)
{
	for (long i = 0; i < g_virtualInfoCount; ++i)
	{
		if (!g_virtualInfos[i].isInput) continue;
		const auto* buffer = reinterpret_cast<const int32_t*>(g_virtualInfos[i].buffers[index]);
		for (long frame = 0; frame < kFrames; ++frame)
			if (buffer[frame] != 0) { g_virtualInputNonZero.fetch_add(1); break; }
	}
	long outputIndex = 0;
	for (long i = 0; i < g_virtualInfoCount; ++i)
	{
		if (g_virtualInfos[i].isInput) continue;
		if (outputIndex >= kOutChannels) break;
		auto* buffer = reinterpret_cast<int32_t*>(g_virtualInfos[i].buffers[index]);
		for (long frame = 0; frame < kFrames; ++frame) buffer[frame] = static_cast<int32_t>((frame + outputIndex) * 1000000);
		++outputIndex;
	}
	g_virtualSwitches.fetch_add(1);
}
static void __cdecl VirtualTapSink(const void*, long frames, long channels, long asioType, double sampleRate)
{
	if (frames == kFrames && channels == kOutChannels && asioType == ASIOSTInt32LSB && sampleRate == 48000.0)
		g_virtualBlocks.fetch_add(1);
}

typedef HRESULT(STDAPICALLTYPE* GetClassObjectFn)(REFCLSID, REFIID, void**);
typedef void(__cdecl* SetSinkFn)(void*);
typedef void(__cdecl* ArmFn)();
typedef int(__cdecl* GetOutputModeFn)();
typedef int(__cdecl* GetInputModeFn)();
typedef void(__cdecl* InjectInputFn)(const int32_t*, long);
typedef void(__cdecl* SetCaptureTestModeFn)(int);
typedef void(__cdecl* SetStubFlagFn)(int);

// Cable drift scenario: the host records virtual input channel 0 while a feeder injects a sine in
// 480-frame packets on a clock that runs fast or slow against the proxy's.
static ASIOBufferInfo* g_driftInfos = nullptr;
static long g_driftFrames = 0;
static std::vector<float> g_driftCapture;
static void DriftHostBufferSwitch(long index, ASIOBool)
{
	const auto* in = reinterpret_cast<const int32_t*>(g_driftInfos[0].buffers[index]);
	for (long f = 0; f < g_driftFrames; ++f) g_driftCapture.push_back(static_cast<float>(in[f] / 2147483648.0));
}

// Virtual clock scenario: any buffer size RS_ASIO asks for must run, at 48 kHz, without bursts or stalls.
static std::atomic<long> g_clockSwitches{ 0 };
static std::atomic<long long> g_clockMaxGapQpc{ 0 };
static long long g_clockLastQpc = 0;
static ASIOBufferInfo* g_clockInfos = nullptr;
static long g_clockFrames = 0;
static void ClockHostBufferSwitch(long index, ASIOBool)
{
	LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
	if (g_clockLastQpc && g_clockSwitches.load() > 2)   // skip startup
	{
		const long long gap = now.QuadPart - g_clockLastQpc;
		if (gap > g_clockMaxGapQpc.load()) g_clockMaxGapQpc.store(gap);
	}
	g_clockLastQpc = now.QuadPart;
	for (long ch = 0; ch < 2; ++ch)
	{
		auto* buffer = reinterpret_cast<int32_t*>(g_clockInfos[ch].buffers[index]);
		for (long f = 0; f < g_clockFrames; ++f) buffer[f] = f;   // touches every frame: an undersized buffer shows up here
	}
	g_clockSwitches.fetch_add(1);
}

// Create a fresh proxy driver instance, drive it through some buffer switches, dispose. Counters are
// reset here so each scenario is independent. `armProbe` (optional) is called just before start.
static bool RunScenario(GetClassObjectFn getClassObject, SetSinkFn setSink, void* sinkFn, ArmFn armProbe, const char** err)
{
	g_curSeq = 0; g_hostSwitches = 0; g_blocks = 0; g_mismatch = 0;
	setSink(sinkFn);

	IClassFactory* factory = nullptr;
	if (FAILED(getClassObject(CLSID_Proxy, IID_IClassFactory, reinterpret_cast<void**>(&factory))) || !factory) { *err = "DllGetClassObject failed"; return false; }
	IAsioDriver* driver = nullptr;
	const HRESULT hr = factory->CreateInstance(nullptr, CLSID_Proxy, reinterpret_cast<void**>(&driver));
	factory->Release();
	if (FAILED(hr) || !driver) { *err = "CreateInstance failed"; return false; }

	bool ok = false;
	if (!driver->init(nullptr)) { *err = "init returned false (could not load/forward to the stub)"; }
	else
	{
		long in = 0, out = 0;
		driver->getChannels(&in, &out);
		driver->setSampleRate(48000.0);
		static ASIOBufferInfo infos[4];
		infos[0] = { 1, 0, {nullptr, nullptr} }; infos[1] = { 1, 1, {nullptr, nullptr} };
		infos[2] = { 0, 0, {nullptr, nullptr} }; infos[3] = { 0, 1, {nullptr, nullptr} };
		g_infos = infos;
		ASIOCallbacks cb{}; cb.bufferSwitch = &HostBufferSwitch; cb.asioMessage = &HostAsioMessage;
		if (driver->createBuffers(infos, 4, kFrames, &cb) != 0) *err = "createBuffers did not forward";
		else if (!infos[2].buffers[0] || !infos[3].buffers[1]) *err = "stub did not allocate output buffers through the proxy";
		else
		{
			if (armProbe) armProbe();
			driver->start();
			std::this_thread::sleep_for(std::chrono::milliseconds(80));
			driver->stop();
			driver->disposeBuffers();
			ok = true;
		}
	}
	driver->Release();
	setSink(nullptr);   // detach before the next scenario
	return ok;
}

static int Cleanup(int code) { UnregisterStub(); return code; }

int wmain(int argc, wchar_t** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 3) { printf("usage: ProxyTestHost <proxy.dll> <stub.dll>\n"); return 2; }
	SetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_TARGET", kStubName);
	SetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_PREFER_REAL", L"1");
	RegisterStub(argv[2]);

	HMODULE proxy = LoadLibraryW(argv[1]);
	if (!proxy) { printf("FAIL: LoadLibrary(proxy) failed\n"); return Cleanup(1); }
	auto setSink = reinterpret_cast<SetSinkFn>(GetProcAddress(proxy, "RSModsAsio_SetSink"));
	auto getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(proxy, "DllGetClassObject"));
	auto armProbe = reinterpret_cast<ArmFn>(GetProcAddress(proxy, "RSModsAsio_ArmLatencyProbe"));
	auto probeLen = reinterpret_cast<int(__cdecl*)()>(GetProcAddress(proxy, "RSModsAsio_ProbeLength"));
	auto configureLimiter = reinterpret_cast<void(__cdecl*)(float, float)>(GetProcAddress(proxy, "RSModsAsio_ConfigureLimiter"));
	auto enableMeter = reinterpret_cast<void(__cdecl*)(int)>(GetProcAddress(proxy, "RSModsAsio_EnableMeter"));
	auto getLevels = reinterpret_cast<int(__cdecl*)(float*, float*, int)>(GetProcAddress(proxy, "RSModsAsio_GetOutputLevels"));
	auto addSink = reinterpret_cast<int(__cdecl*)(void*)>(GetProcAddress(proxy, "RSModsAsio_AddSink"));
	auto removeSink = reinterpret_cast<void(__cdecl*)(void*)>(GetProcAddress(proxy, "RSModsAsio_RemoveSink"));
	auto getOutputMode = reinterpret_cast<GetOutputModeFn>(GetProcAddress(proxy, "RSModsAsio_GetOutputMode"));
	auto getInputMode = reinterpret_cast<GetInputModeFn>(GetProcAddress(proxy, "RSModsAsio_GetInputMode"));
	auto injectInput = reinterpret_cast<InjectInputFn>(GetProcAddress(proxy, "RSModsAsio_TestInjectInput"));
	auto setCaptureTestMode = reinterpret_cast<SetCaptureTestModeFn>(GetProcAddress(proxy, "RSModsAsio_SetVirtualCaptureTestMode"));
	auto tryPromote = reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(proxy, "RSModsAsio_TryPromote"));
	auto tryDemote = reinterpret_cast<int(__cdecl*)()>(GetProcAddress(proxy, "RSModsAsio_TryDemote"));
	HMODULE stub = LoadLibraryW(argv[2]);
	auto setCallbackStalled = stub ? reinterpret_cast<SetStubFlagFn>(GetProcAddress(stub, "RSModsStub_SetCallbackStalled")) : nullptr;
	auto setCreateBuffersFailed = stub ? reinterpret_cast<SetStubFlagFn>(GetProcAddress(stub, "RSModsStub_SetCreateBuffersFailed")) : nullptr;
	auto stubStopCalls = stub ? reinterpret_cast<int(__cdecl*)()>(GetProcAddress(stub, "RSModsStub_StopCalls")) : nullptr;
	auto stubDisposeCalls = stub ? reinterpret_cast<int(__cdecl*)()>(GetProcAddress(stub, "RSModsStub_DisposeCalls")) : nullptr;
	auto invokeLateCallback = stub ? reinterpret_cast<void(__cdecl*)()>(GetProcAddress(stub, "RSModsStub_InvokeLateCallback")) : nullptr;
	if (!setSink || !getClassObject || !armProbe || !probeLen || !configureLimiter || !enableMeter || !getLevels || !addSink || !removeSink || !getOutputMode || !getInputMode || !injectInput || !setCaptureTestMode || !tryPromote || !tryDemote || !stub || !setCallbackStalled || !setCreateBuffersFailed || !stubStopCalls || !stubDisposeCalls || !invokeLateCallback) { printf("FAIL: proxy/stub missing exports\n"); return Cleanup(1); }
	setCaptureTestMode(1);
	setCallbackStalled(0);
	setCreateBuffersFailed(0);

	const char* err = nullptr;

	// Discovery may construct and initialize the endpoint repeatedly without opening the configured
	// physical driver. It must advertise the Rocksmith-compatible virtual output until createBuffers.
	const int discoveryStopCalls = stubStopCalls();
	const int discoveryDisposeCalls = stubDisposeCalls();
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		IClassFactory* discoveryFactory = nullptr;
		IAsioDriver* discoveryDriver = nullptr;
		if (FAILED(getClassObject(CLSID_Proxy, IID_IClassFactory, reinterpret_cast<void**>(&discoveryFactory)))
			|| !discoveryFactory
			|| FAILED(discoveryFactory->CreateInstance(nullptr, CLSID_Proxy, reinterpret_cast<void**>(&discoveryDriver)))
			|| !discoveryDriver
			|| !discoveryDriver->init(nullptr))
		{
			if (discoveryFactory) discoveryFactory->Release();
			if (discoveryDriver) discoveryDriver->Release();
			printf("FAIL [discovery]: init-only instance was not available\n");
			return Cleanup(1);
		}
		discoveryFactory->Release();
		long discoveryIn = -1, discoveryOut = -1;
		discoveryDriver->getChannels(&discoveryIn, &discoveryOut);
		ASIOChannelInfo discoveryInfo{}; discoveryInfo.channel = 0; discoveryInfo.isInput = 0;
		const ASIOError discoveryInfoResult = discoveryDriver->getChannelInfo(&discoveryInfo);
		ASIOChannelInfo discoveryInputInfo{}; discoveryInputInfo.channel = 0; discoveryInputInfo.isInput = 1;
		const ASIOError discoveryInputResult = discoveryDriver->getChannelInfo(&discoveryInputInfo);
		discoveryDriver->Release();
		if (discoveryIn != 2 || discoveryOut != 2 || discoveryInfoResult != 0 || discoveryInfo.type != ASIOSTInt32LSB
			|| discoveryInputResult != 0 || discoveryInputInfo.type != ASIOSTInt32LSB)
		{
			printf("FAIL [discovery]: init-only instance exposed incompatible output (in=%ld out=%ld type=%ld)\n",
				discoveryIn, discoveryOut, discoveryInfo.type);
			return Cleanup(1);
		}
	}
	if (stubStopCalls() != discoveryStopCalls || stubDisposeCalls() != discoveryDisposeCalls)
	{
		printf("FAIL [discovery]: init-only instances touched the physical driver\n");
		return Cleanup(1);
	}
	printf("[discovery] repeated init-only instances stayed virtual (ASIOSTInt32LSB)\n");

	// A failed physical bind during the host's actual createBuffers call must retain a usable virtual stream.
	setCreateBuffersFailed(1);
	setSink(reinterpret_cast<void*>(&VirtualTapSink));
	IClassFactory* failedFactory = nullptr;
	IAsioDriver* failedDriver = nullptr;
	if (FAILED(getClassObject(CLSID_Proxy, IID_IClassFactory, reinterpret_cast<void**>(&failedFactory)))
		|| !failedFactory
		|| FAILED(failedFactory->CreateInstance(nullptr, CLSID_Proxy, reinterpret_cast<void**>(&failedDriver)))
		|| !failedDriver
		|| !failedDriver->init(nullptr))
	{
		if (failedFactory) failedFactory->Release();
		if (failedDriver) failedDriver->Release();
		setCreateBuffersFailed(0);
		printf("FAIL [createBuffers-failure]: virtual fallback instance unavailable\n");
		return Cleanup(1);
	}
	failedFactory->Release();
	ASIOBufferInfo failedInfos[4]{};
	failedInfos[0].isInput = 1; failedInfos[0].channelNum = 0;
	failedInfos[1].isInput = 1; failedInfos[1].channelNum = 1;
	failedInfos[2].isInput = 0; failedInfos[2].channelNum = 0;
	failedInfos[3].isInput = 0; failedInfos[3].channelNum = 1;
	g_virtualInfos = failedInfos;
	g_virtualInfoCount = 4;
	ASIOCallbacks failedCallbacks{}; failedCallbacks.bufferSwitch = &VirtualHostBufferSwitch;
	const ASIOError failedBind = failedDriver->createBuffers(failedInfos, 4, kFrames, &failedCallbacks);
	const bool failedVirtual = failedBind == 0 && failedInfos[0].buffers[0] != nullptr
		&& failedInfos[1].buffers[1] != nullptr && failedInfos[2].buffers[0] != nullptr
		&& failedInfos[3].buffers[1] != nullptr && failedDriver->start() == 0;
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	const auto* silentCapture = reinterpret_cast<const int32_t*>(failedInfos[0].buffers[0]);
	const int failedOutputMode = getOutputMode();
	const int32_t firstCapture = silentCapture ? silentCapture[0] : 0;
	const int32_t lastCapture = silentCapture ? silentCapture[kFrames - 1] : 0;
	if (!failedVirtual || failedOutputMode != 1 || g_virtualSwitches.load() == 0 || g_virtualBlocks.load() == 0
		|| !silentCapture || firstCapture != 0 || lastCapture != 0)
	{
		printf("FAIL [createBuffers-failure]: physical bind failure did not preserve virtual stream (bind=%d mode=%d)\n",
			failedBind, getOutputMode());
		failedDriver->stop(); failedDriver->disposeBuffers(); failedDriver->Release();
		setCreateBuffersFailed(0);
		return Cleanup(1);
	}
	failedDriver->stop();
	failedDriver->disposeBuffers();
	failedDriver->Release();
	setCreateBuffersFailed(0);
	setSink(nullptr);
	printf("[createBuffers-failure] physical bind failed, virtual ASIOSTInt32LSB stream continued\n");

	// An input-only host must also receive silent virtual capture buffers and a paced callback.
	SetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_PREFER_REAL", L"0");
	IClassFactory* inputFactory = nullptr;
	IAsioDriver* inputDriver = nullptr;
	if (FAILED(getClassObject(CLSID_Proxy, IID_IClassFactory, reinterpret_cast<void**>(&inputFactory)))
		|| !inputFactory
		|| FAILED(inputFactory->CreateInstance(nullptr, CLSID_Proxy, reinterpret_cast<void**>(&inputDriver)))
		|| !inputDriver
		|| !inputDriver->init(nullptr))
	{
		if (inputFactory) inputFactory->Release();
		if (inputDriver) inputDriver->Release();
		printf("FAIL [input-only]: virtual capture instance unavailable\n");
		return Cleanup(1);
	}
	inputFactory->Release();
	ASIOBufferInfo inputInfos[1]{};
	inputInfos[0].isInput = 1; inputInfos[0].channelNum = 0;
	g_virtualInfos = inputInfos;
	g_virtualInfoCount = 1;
	g_virtualSwitches = 0;
	g_virtualInputNonZero = 0;
	ASIOCallbacks inputCallbacks{}; inputCallbacks.bufferSwitch = &VirtualHostBufferSwitch;
	if (inputDriver->createBuffers(inputInfos, 1, kFrames, &inputCallbacks) != 0
		|| inputDriver->start() != 0)
	{
		inputDriver->Release();
		printf("FAIL [input-only]: virtual capture buffers did not start\n");
		return Cleanup(1);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	const auto* inputSamples = reinterpret_cast<const int32_t*>(inputInfos[0].buffers[0]);
	if (g_virtualSwitches.load() == 0 || getInputMode() != 1 || inputSamples[0] != 0 || inputSamples[kFrames - 1] != 0)
	{
		inputDriver->stop(); inputDriver->disposeBuffers(); inputDriver->Release();
		printf("FAIL [input-only]: virtual capture was not silent and paced\n");
		return Cleanup(1);
	}
	// Enough to prime the reader's safety margin (one block + margin + one packet) and play out.
	const long injectFrames = kFrames * 8;
	std::vector<int32_t> injected(injectFrames * 2, 0);
	for (long frame = 0; frame < injectFrames; ++frame) { injected[frame * 2] = 100000 + frame; injected[frame * 2 + 1] = -100000 - frame; }
	for (long offset = 0; offset < injectFrames; offset += kFrames) injectInput(injected.data() + offset * 2, kFrames);   // packet-sized, like the cable
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	if (g_virtualInputNonZero.load() == 0)
	{
		inputDriver->stop(); inputDriver->disposeBuffers(); inputDriver->Release();
		printf("FAIL [input-only]: injected virtual capture did not reach host buffers\n");
		return Cleanup(1);
	}
	std::vector<int32_t> overflow(20000 * 2, 777777);
	injectInput(overflow.data(), 20000);
	inputDriver->stop(); inputDriver->disposeBuffers(); inputDriver->Release();
	SetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_PREFER_REAL", L"1");
	printf("[input-only] virtual capture stayed silent and paced\n");

	// Scenario 1: sink set -> tap receives the exact output.
	const int physicalDisposeBefore = stubDisposeCalls();
	if (!RunScenario(getClassObject, setSink, reinterpret_cast<void*>(&TapSink), nullptr, &err)) { printf("FAIL [tap]: %s\n", err); return Cleanup(1); }
	if (stubDisposeCalls() != physicalDisposeBefore + 1) { printf("FAIL [binding]: createBuffers did not bind and dispose the physical driver exactly once\n"); return Cleanup(1); }
	printf("[tap]    host switches: %d, blocks tapped: %d, mismatches: %d\n", g_hostSwitches.load(), g_blocks.load(), g_mismatch.load());
	if (g_hostSwitches.load() == 0) { printf("FAIL [tap]: proxy never forwarded bufferSwitch\n"); return Cleanup(1); }
	if (g_blocks.load() == 0)       { printf("FAIL [tap]: tap never fired\n"); return Cleanup(1); }
	if (g_mismatch.load() != 0)     { printf("FAIL [tap]: tapped data did not match what the host wrote\n"); return Cleanup(1); }

	// Scenario 2: no sink -> still forwards, taps nothing (everyday playback, recording off).
	if (!RunScenario(getClassObject, setSink, nullptr, nullptr, &err)) { printf("FAIL [passthrough]: %s\n", err); return Cleanup(1); }
	printf("[bypass] host switches: %d, blocks tapped: %d\n", g_hostSwitches.load(), g_blocks.load());
	if (g_hostSwitches.load() == 0) { printf("FAIL [passthrough]: proxy stopped forwarding when no sink was set\n"); return Cleanup(1); }
	if (g_blocks.load() != 0)       { printf("FAIL [passthrough]: tapped output with no sink registered\n"); return Cleanup(1); }

	// Scenario 3: arm the latency probe -> the injected impulse must appear in the tapped output, and the
	// DSP must locate it at lag ~0 (the tap sits at the injection point). This exercises inject + DSP together.
	g_capture.clear();
	if (!RunScenario(getClassObject, setSink, reinterpret_cast<void*>(&CaptureSink), armProbe, &err)) { printf("FAIL [probe]: %s\n", err); return Cleanup(1); }
	{
		const int refLen = probeLen();
		std::vector<float> reference(refLen);
		LatencyDsp::GenerateProbe(reference.data(), refLen);
		LatencyDsp::Match m = LatencyDsp::FindLag(reference.data(), refLen, g_capture.data(), (int)g_capture.size(), 256);
		printf("[probe]  captured %zu samples, DSP lag=%d conf=%.3f\n", g_capture.size(), m.lag, m.confidence);
		if ((int)g_capture.size() < refLen) { printf("FAIL [probe]: not enough output captured\n"); return Cleanup(1); }
		if (m.lag < 0 || m.lag > kFrames)   { printf("FAIL [probe]: injected probe not found near the start of the output\n"); return Cleanup(1); }
		if (m.confidence < 0.9f)            { printf("FAIL [probe]: injected probe correlation too weak\n"); return Cleanup(1); }
	}

	// Scenario 4: the static output trim scales the tapped output exactly by its gain.
	g_scaleFactor = 0.5f; g_scaleBlocks = 0; g_scaleBad = 0;
	configureLimiter(0.5f, 0.0f);   // trim gain 0.5x; second arg reserved
	const bool trimOk = RunScenario(getClassObject, setSink, reinterpret_cast<void*>(&ScaleSink), nullptr, &err);
	configureLimiter(1.0f, 0.0f);   // reset to unity passthrough for anything after
	if (!trimOk) { printf("FAIL [trim]: %s\n", err); return Cleanup(1); }
	printf("[trim] blocks: %d, out-of-tolerance: %d\n", g_scaleBlocks.load(), g_scaleBad.load());
	if (g_scaleBlocks.load() == 0) { printf("FAIL [trim]: trim scenario produced no output\n"); return Cleanup(1); }
	if (g_scaleBad.load() != 0)    { printf("FAIL [trim]: output trim did not scale the output correctly\n"); return Cleanup(1); }

	// Scenario 5: with the meter enabled and the full-scale probe armed, the reported peak must rise well
	// above the tiny test ramp, proving the meter reads the real output level.
	enableMeter(1);
	if (!RunScenario(getClassObject, setSink, nullptr, armProbe, &err)) { printf("FAIL [meter]: %s\n", err); enableMeter(0); return Cleanup(1); }
	float peak[8] = {0}, rms[8] = {0};
	const int levels = getLevels(peak, rms, 8);
	enableMeter(0);
	printf("[meter]  channels: %d, peak[0]=%.3f rms[0]=%.3f\n", levels, peak[0], rms[0]);
	if (levels < 1)        { printf("FAIL [meter]: no channels metered\n"); return Cleanup(1); }
	if (peak[0] < 0.5f)    { printf("FAIL [meter]: meter did not see the full-scale probe\n"); return Cleanup(1); }

	// Scenario 6: register a second sink and confirm the output fans out to both (multi-out).
	g_secondBlocks = 0;
	const int slot = addSink(reinterpret_cast<void*>(&SecondSink));
	if (slot < 1) { printf("FAIL [multi-out]: AddSink returned no slot\n"); return Cleanup(1); }
	const bool moOk = RunScenario(getClassObject, setSink, reinterpret_cast<void*>(&TapSink), nullptr, &err);
	removeSink(reinterpret_cast<void*>(&SecondSink));
	if (!moOk) { printf("FAIL [multi-out]: %s\n", err); return Cleanup(1); }
	printf("[multi]  primary blocks: %d, second sink blocks: %d\n", g_blocks.load(), g_secondBlocks.load());
	if (g_blocks.load() == 0 || g_secondBlocks.load() == 0) { printf("FAIL [multi-out]: output did not reach both sinks\n"); return Cleanup(1); }
	if (g_mismatch.load() != 0) { printf("FAIL [multi-out]: primary sink data wrong with a second sink attached\n"); return Cleanup(1); }

	// Scenario 7: virtual fallback and explicit live-failure transitions.
	UnregisterStub();
	g_virtualSwitches = 0;
	g_virtualBlocks = 0;
	setSink(reinterpret_cast<void*>(&VirtualTapSink));
	IClassFactory* virtualFactory = nullptr;
	if (FAILED(getClassObject(CLSID_Proxy, IID_IClassFactory, reinterpret_cast<void**>(&virtualFactory))) || !virtualFactory) { printf("FAIL [virtual]: class factory unavailable\n"); return Cleanup(1); }
	IAsioDriver* virtualDriver = nullptr;
	const HRESULT virtualCreate = virtualFactory->CreateInstance(nullptr, CLSID_Proxy, reinterpret_cast<void**>(&virtualDriver));
	virtualFactory->Release();
	if (FAILED(virtualCreate) || !virtualDriver || !virtualDriver->init(nullptr)) { printf("FAIL [virtual]: init did not provide fallback\n"); return Cleanup(1); }
	long virtualIn = -1, virtualOut = -1;
	virtualDriver->getChannels(&virtualIn, &virtualOut);
	ASIOBufferInfo virtualInfos[2]{};
	virtualInfos[0].isInput = 0; virtualInfos[0].channelNum = 0;
	virtualInfos[1].isInput = 0; virtualInfos[1].channelNum = 1;
	g_virtualInfos = virtualInfos;
	g_virtualInfoCount = 2;
	ASIOCallbacks virtualCallbacks{}; virtualCallbacks.bufferSwitch = &VirtualHostBufferSwitch;
	if (virtualIn != 2 || virtualOut != 2 || virtualDriver->setSampleRate(48000.0) != 0
		|| virtualDriver->createBuffers(virtualInfos, 2, kFrames, &virtualCallbacks) != 0
		|| virtualDriver->start() != 0)
	{
		printf("FAIL [virtual]: output-only fallback setup/mode failed (in=%ld out=%ld mode=%d)\n", virtualIn, virtualOut, getOutputMode());
		if (virtualDriver) { virtualDriver->disposeBuffers(); virtualDriver->Release(); }
		return Cleanup(1);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	if (getOutputMode() != 1)
	{
		printf("FAIL [virtual]: callback mode was not published\n");
		virtualDriver->stop(); virtualDriver->disposeBuffers(); virtualDriver->Release();
		return Cleanup(1);
	}
	setSink(nullptr);
	RegisterStub(argv[2]);
	SetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_TARGET", L"stale saved target");
	const int switchesBeforePromotion = g_virtualSwitches.load();
	if (!tryPromote(kStubName) || getOutputMode() != 2)
	{
		printf("FAIL [promotion]: returning ASIO driver did not replace the virtual transport (mode=%d)\n", getOutputMode());
		virtualDriver->stop(); virtualDriver->disposeBuffers(); virtualDriver->Release();
		return Cleanup(1);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	if (g_virtualSwitches.load() <= switchesBeforePromotion)
	{
		printf("FAIL [promotion]: host callbacks stopped after promotion\n");
		virtualDriver->stop(); virtualDriver->disposeBuffers(); virtualDriver->Release();
		return Cleanup(1);
	}
	setCallbackStalled(1);
	bool stalledModePublished = false;
	for (int attempt = 0; attempt < 140; ++attempt)
	{
		if (getOutputMode() == 3) { stalledModePublished = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (!stalledModePublished)
	{
		printf("FAIL [stall]: stalled real callback stream was not published as mode 3 (mode=%d)\n", getOutputMode());
		virtualDriver->stop(); virtualDriver->disposeBuffers(); virtualDriver->Release();
		return Cleanup(1);
	}
	const int stopCallsBeforeDemotion = stubStopCalls();
	const int disposeCallsBeforeDemotion = stubDisposeCalls();
	const bool demoted = tryDemote() != 0;
	const int switchesAfterDemotion = g_virtualSwitches.load();
	const int stopCallsAfterDemotion = stubStopCalls();
	const int disposeCallsAfterDemotion = stubDisposeCalls();
	invokeLateCallback();
	const int switchesAfterLateCallback = g_virtualSwitches.load();
	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	if (!demoted || getOutputMode() != 1 || g_virtualSwitches.load() <= switchesAfterDemotion
		|| switchesAfterLateCallback != switchesAfterDemotion
		|| stopCallsAfterDemotion != stopCallsBeforeDemotion || disposeCallsAfterDemotion != disposeCallsBeforeDemotion)
	{
		printf("FAIL [demotion]: takeover did not quarantine stalled driver (mode=%d, callbacks=%d->%d, stop=%d, dispose=%d)\n",
			getOutputMode(), switchesAfterDemotion, g_virtualSwitches.load(), stopCallsAfterDemotion, disposeCallsAfterDemotion);
		virtualDriver->stop(); virtualDriver->disposeBuffers(); virtualDriver->Release();
		return Cleanup(1);
	}
	setCallbackStalled(0);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	if (getOutputMode() != 1)
	{
		printf("FAIL [demotion]: returning real driver auto-promoted without an explicit request (mode=%d)\n", getOutputMode());
		virtualDriver->stop(); virtualDriver->disposeBuffers(); virtualDriver->Release();
		return Cleanup(1);
	}
	const int callbacksBeforeFailedPromotion = g_virtualSwitches.load();
	setCreateBuffersFailed(1);
	const bool failedPromotion = tryPromote(kStubName) == 0;
	for (int attempt = 0; attempt < 20 && getOutputMode() == 0; ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	if (!failedPromotion || getOutputMode() != 1)
	{
		printf("FAIL [promotion-failure]: failed explicit promotion changed virtual mode (mode=%d)\n", getOutputMode());
		setCreateBuffersFailed(0);
		virtualDriver->stop(); virtualDriver->disposeBuffers(); virtualDriver->Release();
		return Cleanup(1);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	if (g_virtualSwitches.load() <= callbacksBeforeFailedPromotion)
	{
		printf("FAIL [promotion-failure]: virtual callbacks did not continue after failed promotion\n");
		setCreateBuffersFailed(0);
		virtualDriver->stop(); virtualDriver->disposeBuffers(); virtualDriver->Release();
		return Cleanup(1);
	}
	setCreateBuffersFailed(0);
	if (!tryPromote(kStubName) || getOutputMode() != 2)
	{
		printf("FAIL [promotion]: explicit promotion after recovery did not restore real mode (mode=%d)\n", getOutputMode());
		virtualDriver->stop(); virtualDriver->disposeBuffers(); virtualDriver->Release();
		return Cleanup(1);
	}
	virtualDriver->stop();
	virtualDriver->disposeBuffers();
	virtualDriver->Release();
	printf("[transitions] callbacks: %d, tapped blocks: %d, direct-target promotion, mode 3 stall, demotion, no auto-promotion, failed promotion, and explicit recovery verified\n", g_virtualSwitches.load(), g_virtualBlocks.load());
	if (g_virtualSwitches.load() == 0 || g_virtualBlocks.load() == 0) { printf("FAIL [virtual]: callback/tap did not run\n"); return Cleanup(1); }

	// Scenario 8: the virtual clock honours whatever buffer size the host picks (RS_ASIO BufferSizeMode
	// driver / host / custom), keeps 48 kHz, and neither bursts nor stalls. Stub unregistered = speaker mode.
	// PreferReal off: with it on and no test target the proxy would read the machine's real HKCU Target and
	// load the developer's actual ASIO driver.
	UnregisterStub();
	SetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_TARGET", L"RSMods Missing Test Driver");
	SetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_PREFER_REAL", L"0");
	{
		LARGE_INTEGER qpf{}; QueryPerformanceFrequency(&qpf);
		const long sizes[] = { 32, 96, 128, 256, 441 };
		for (long size : sizes)
		{
			IClassFactory* factory = nullptr;
			IAsioDriver* driver = nullptr;
			if (FAILED(getClassObject(CLSID_Proxy, IID_IClassFactory, reinterpret_cast<void**>(&factory))) || !factory) { printf("FAIL [clock]: class factory unavailable\n"); return Cleanup(1); }
			const HRESULT created = factory->CreateInstance(nullptr, CLSID_Proxy, reinterpret_cast<void**>(&driver));
			factory->Release();
			if (FAILED(created) || !driver || !driver->init(nullptr)) { printf("FAIL [clock]: init failed\n"); return Cleanup(1); }
			long mn = 0, mx = 0, pref = 0, gran = 0;
			driver->getBufferSize(&mn, &mx, &pref, &gran);
			if (size < mn || size > mx || gran != 1 || pref != 128)
			{
				printf("FAIL [clock]: virtual buffer range %ld..%ld pref %ld gran %ld does not admit %ld\n", mn, mx, pref, gran, size);
				driver->Release(); return Cleanup(1);
			}
			ASIOBufferInfo infos[2]{};
			infos[0].isInput = 0; infos[0].channelNum = 0;
			infos[1].isInput = 0; infos[1].channelNum = 1;
			g_clockInfos = infos; g_clockFrames = size; g_clockSwitches = 0; g_clockMaxGapQpc = 0; g_clockLastQpc = 0;
			ASIOCallbacks callbacks{}; callbacks.bufferSwitch = &ClockHostBufferSwitch;
			const ASIOError createResult = driver->createBuffers(infos, 2, size, &callbacks);
			const ASIOError startResult = createResult == 0 ? driver->start() : -1;
			if (createResult != 0 || startResult != 0)
			{
				printf("FAIL [clock]: virtual mode refused %ld frames (createBuffers %ld, start %ld)\n", size, createResult, startResult);
				driver->disposeBuffers(); driver->Release(); return Cleanup(1);
			}
			const auto begin = std::chrono::steady_clock::now();
			std::this_thread::sleep_for(std::chrono::milliseconds(1500));
			const long switches = g_clockSwitches.load();
			const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
			const int mode = getOutputMode();
			driver->stop(); driver->disposeBuffers(); driver->Release();
			if (mode != 1) { printf("FAIL [clock]: %ld frames did not run in virtual mode (mode=%d)\n", size, mode); return Cleanup(1); }
			const double rate = switches * static_cast<double>(size) / seconds;
			const double periodMs = 1000.0 * size / 48000.0;
			const double maxGapMs = 1000.0 * g_clockMaxGapQpc.load() / qpf.QuadPart;
			printf("[clock]  %4ld frames: %.0f Hz effective, period %.2f ms, worst gap %.2f ms\n", size, rate, periodMs, maxGapMs);
			if (rate < 48000.0 * 0.97 || rate > 48000.0 * 1.03) { printf("FAIL [clock]: %ld-frame virtual clock ran at %.0f Hz\n", size, rate); return Cleanup(1); }
			if (maxGapMs > periodMs + 4.0) { printf("FAIL [clock]: %ld-frame virtual clock stalled %.2f ms\n", size, maxGapMs); return Cleanup(1); }
		}
	}

	// Scenario 9: with the real device wanted and present, the buffer-size query (made while the proxy is
	// still Virtual, before createBuffers) must answer with the REAL device's limits, so RS_ASIO picks a
	// size the hardware accepts. The stub allows exactly 128, granularity 0; the virtual range differs.
	RegisterStub(argv[2]);
	SetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_TARGET", kStubName);
	SetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_PREFER_REAL", L"1");
	{
		IClassFactory* factory = nullptr;
		IAsioDriver* driver = nullptr;
		if (FAILED(getClassObject(CLSID_Proxy, IID_IClassFactory, reinterpret_cast<void**>(&factory))) || !factory) { printf("FAIL [size-query]: class factory unavailable\n"); return Cleanup(1); }
		const HRESULT created = factory->CreateInstance(nullptr, CLSID_Proxy, reinterpret_cast<void**>(&driver));
		factory->Release();
		if (FAILED(created) || !driver || !driver->init(nullptr)) { printf("FAIL [size-query]: init failed\n"); return Cleanup(1); }
		long mn = 0, mx = 0, pref = 0, gran = -5;
		driver->getBufferSize(&mn, &mx, &pref, &gran);
		printf("[size-query] real device present: %ld..%ld preferred %ld granularity %ld\n", mn, mx, pref, gran);
		if (mn != kFrames || mx != kFrames || pref != kFrames || gran != 0) { printf("FAIL [size-query]: query did not report the real device's limits\n"); driver->Release(); return Cleanup(1); }
		ASIOBufferInfo infos[2]{};
		infos[0].isInput = 0; infos[0].channelNum = 0;
		infos[1].isInput = 0; infos[1].channelNum = 1;
		g_clockInfos = infos; g_clockFrames = kFrames; g_clockSwitches = 0; g_clockLastQpc = 0;
		ASIOCallbacks callbacks{}; callbacks.bufferSwitch = &ClockHostBufferSwitch;
		const ASIOError bound = driver->createBuffers(infos, 2, pref, &callbacks);
		if (bound == 0) driver->start();
		std::this_thread::sleep_for(std::chrono::milliseconds(80));   // the mode is published on the first callback
		const int mode = getOutputMode();
		if (bound == 0) driver->stop();
		driver->disposeBuffers(); driver->Release();
		if (bound != 0 || mode != 2) { printf("FAIL [size-query]: real device did not bind at the size it reported (createBuffers %ld, mode %d)\n", bound, mode); return Cleanup(1); }
	}

	// Scenario 10: real device wanted but absent (speaker mode with PreferReal on, e.g. interface unplugged):
	// the query falls back to the virtual range, and createBuffers takes even a size below the advertised
	// minimum rather than leave the player without audio.
	UnregisterStub();
	SetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_TARGET", L"RSMods Missing Test Driver");
	{
		IClassFactory* factory = nullptr;
		IAsioDriver* driver = nullptr;
		if (FAILED(getClassObject(CLSID_Proxy, IID_IClassFactory, reinterpret_cast<void**>(&factory))) || !factory) { printf("FAIL [size-absent]: class factory unavailable\n"); return Cleanup(1); }
		const HRESULT created = factory->CreateInstance(nullptr, CLSID_Proxy, reinterpret_cast<void**>(&driver));
		factory->Release();
		if (FAILED(created) || !driver || !driver->init(nullptr)) { printf("FAIL [size-absent]: init failed\n"); return Cleanup(1); }
		long mn = 0, mx = 0, pref = 0, gran = 0;
		driver->getBufferSize(&mn, &mx, &pref, &gran);
		printf("[size-absent] real device missing: %ld..%ld preferred %ld granularity %ld\n", mn, mx, pref, gran);
		if (mn != 32 || mx != 4096 || pref != 128 || gran != 1) { printf("FAIL [size-absent]: expected the virtual range\n"); driver->Release(); return Cleanup(1); }
		const long tiny = 16;
		ASIOBufferInfo infos[2]{};
		infos[0].isInput = 0; infos[0].channelNum = 0;
		infos[1].isInput = 0; infos[1].channelNum = 1;
		g_clockInfos = infos; g_clockFrames = tiny; g_clockSwitches = 0; g_clockLastQpc = 0;
		ASIOCallbacks callbacks{}; callbacks.bufferSwitch = &ClockHostBufferSwitch;
		if (driver->createBuffers(infos, 2, tiny, &callbacks) != 0 || driver->start() != 0) { printf("FAIL [size-absent]: virtual refused %ld frames\n", tiny); driver->disposeBuffers(); driver->Release(); return Cleanup(1); }
		const auto begin = std::chrono::steady_clock::now();
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		const double rate = g_clockSwitches.load() * static_cast<double>(tiny) / std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
		const int mode = getOutputMode();
		driver->stop(); driver->disposeBuffers(); driver->Release();
		printf("[size-absent] %ld frames in virtual: %.0f Hz effective, mode %d\n", tiny, rate, mode);
		if (mode != 1 || rate < 48000.0 * 0.97 || rate > 48000.0 * 1.03) { printf("FAIL [size-absent]: sub-minimum buffer did not run at 48 kHz in virtual mode\n"); return Cleanup(1); }
	}

	// Scenario 11: the cable runs on its own clock. Feed a 440 Hz sine in 480-frame packets (the cable's
	// 10 ms WASAPI packets) at +/-500 ppm against the proxy's clock, with real sleep jitter, and require a
	// continuous signal: no silent blocks, no discontinuities, no underruns, bounded latency. The old
	// spinlocked, uncontrolled ring dropped whole 128-frame blocks here.
	{
		auto getStats = reinterpret_cast<int(__cdecl*)(long*, long)>(GetProcAddress(proxy, "RSModsAsio_GetVirtualInputStats"));
		if (!getStats) { printf("FAIL [drift]: RSModsAsio_GetVirtualInputStats not exported\n"); return Cleanup(1); }
		SetEnvironmentVariableW(L"RSMODS_ASIO_PROXY_TEST_PREFER_REAL", L"0");
		for (const double ppm : { 500.0, -500.0 })
		{
			IClassFactory* factory = nullptr;
			IAsioDriver* driver = nullptr;
			if (FAILED(getClassObject(CLSID_Proxy, IID_IClassFactory, reinterpret_cast<void**>(&factory))) || !factory) { printf("FAIL [drift]: class factory unavailable\n"); return Cleanup(1); }
			const HRESULT created = factory->CreateInstance(nullptr, CLSID_Proxy, reinterpret_cast<void**>(&driver));
			factory->Release();
			if (FAILED(created) || !driver || !driver->init(nullptr)) { printf("FAIL [drift]: init failed\n"); return Cleanup(1); }
			ASIOBufferInfo infos[3]{};
			infos[0].isInput = 1; infos[0].channelNum = 0;
			infos[1].isInput = 0; infos[1].channelNum = 0;
			infos[2].isInput = 0; infos[2].channelNum = 1;
			g_driftInfos = infos; g_driftFrames = kFrames;
			g_driftCapture.clear(); g_driftCapture.reserve(48000 * 8);
			ASIOCallbacks callbacks{}; callbacks.bufferSwitch = &DriftHostBufferSwitch;
			if (driver->createBuffers(infos, 3, kFrames, &callbacks) != 0 || driver->start() != 0) { printf("FAIL [drift]: virtual stream did not start\n"); driver->disposeBuffers(); driver->Release(); return Cleanup(1); }

			std::atomic<bool> feeding{ true };
			std::thread feeder([&]
			{
				timeBeginPeriod(1);   // WASAPI capture events are ~10 ms regular; the default 15.6 ms timer would clump packets
				const double rate = 48000.0 * (1.0 + ppm * 1e-6);
				const long packet = 480;
				std::vector<int32_t> samples(packet * 2);
				double phase = 0.0;
				const double step = 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;   // the signal's own clock
				const auto start = std::chrono::steady_clock::now();
				for (long n = 1; feeding.load(); ++n)
				{
					for (long f = 0; f < packet; ++f)
					{
						const int32_t v = static_cast<int32_t>(0.5 * std::sin(phase) * 2147483647.0);
						samples[f * 2] = v; samples[f * 2 + 1] = v;
						phase += step;
					}
					injectInput(samples.data(), packet);
					std::this_thread::sleep_until(start + std::chrono::microseconds(static_cast<long long>(n * packet * 1e6 / rate)));
				}
				timeEndPeriod(1);
			});
			std::this_thread::sleep_for(std::chrono::milliseconds(4000));
			long stats[8]{};
			getStats(stats, 8);
			driver->stop();
			feeding.store(false);
			feeder.join();
			driver->disposeBuffers(); driver->Release();

			// Analyse from 20 ms after the signal first appears (past the fade-in) to the end.
			size_t first = 0;
			while (first < g_driftCapture.size() && g_driftCapture[first] == 0.0f) ++first;
			const size_t from = first + 960;
			float worstStep = 0.0f;
			size_t zeroRun = 0, worstZeroRun = 0;
			for (size_t i = from + 2; i < g_driftCapture.size(); ++i)
			{
				const float d2 = std::fabs(g_driftCapture[i] - 2.0f * g_driftCapture[i - 1] + g_driftCapture[i - 2]);
				if (d2 > worstStep) worstStep = d2;
				zeroRun = std::fabs(g_driftCapture[i]) < 1e-4f ? zeroRun + 1 : 0;
				if (zeroRun > worstZeroRun) worstZeroRun = zeroRun;
			}
			const double analysedSeconds = g_driftCapture.size() > from ? (g_driftCapture.size() - from) / 48000.0 : 0.0;
			printf("[drift]  cable %+.0f ppm: %.2f s analysed, worst 2nd-difference %.4f (clean sine ~0.0017), longest near-zero run %zu, underruns %ld, overflow drops %ld, skips %ld, fill %ld, margin %ld, correction %+ld ppm\n",
				ppm, analysedSeconds, worstStep, worstZeroRun, stats[0], stats[1], stats[2], stats[4], stats[7], stats[5]);
			if (analysedSeconds < 3.0) { printf("FAIL [drift]: the signal never started or stopped early\n"); return Cleanup(1); }
			if (stats[0] != 0 || stats[1] != 0 || stats[2] != 0) { printf("FAIL [drift]: the reader dropped or skipped audio\n"); return Cleanup(1); }
			if (worstStep > 0.01f || worstZeroRun > 8) { printf("FAIL [drift]: discontinuity in the captured signal\n"); return Cleanup(1); }
			if (stats[4] > kFrames + 96 + 480 + 480) { printf("FAIL [drift]: latency crept (fill %ld frames)\n", stats[4]); return Cleanup(1); }
		}
	}

	printf("PASS: proxy forwards, taps, injects a probe, gains/limits, meters, fans out, and handles live failure transitions.\n");
	return Cleanup(0);
}
