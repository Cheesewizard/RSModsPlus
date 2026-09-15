// Stub "real" ASIO driver for the proxy test harness. No hardware: it allocates double-buffers,
// drives the host's bufferSwitch on a background thread, and lets the host fill the output buffers.
// The proxy loads this the same way it loads a user's real driver (Target -> Software\ASIO -> CLSID),
// so a passing test proves the proxy's resolve/load/forward/tap path end to end, with no game.
#include "../AsioInterface.h"
#include <atomic>
#include <thread>
#include <new>

// {A1B2C3D4-1111-4111-8111-111111111111}
static const CLSID CLSID_StubAsio =
	{ 0xa1b2c3d4, 0x1111, 0x4111, { 0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 } };

namespace { const long kFrames = 128; const long kChannels = 2; }

static std::atomic<bool> g_callbackStalled{ false };
static std::atomic<bool> g_createBuffersFailed{ false };
static std::atomic<int> g_stopCalls{ 0 };
static std::atomic<int> g_disposeCalls{ 0 };
class StubAsioDriver;
static std::atomic<StubAsioDriver*> g_lastDriver{ nullptr };

extern "C" __declspec(dllexport) void RSModsStub_SetCallbackStalled(int stalled)
{
	g_callbackStalled.store(stalled != 0, std::memory_order_release);
}

extern "C" __declspec(dllexport) void RSModsStub_SetCreateBuffersFailed(int failed)
{
	g_createBuffersFailed.store(failed != 0, std::memory_order_release);
}

extern "C" __declspec(dllexport) int RSModsStub_StopCalls() { return g_stopCalls.load(std::memory_order_acquire); }
extern "C" __declspec(dllexport) int RSModsStub_DisposeCalls() { return g_disposeCalls.load(std::memory_order_acquire); }

class StubAsioDriver : public IAsioDriver
{
public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** o) override
	{
		if (!o) return E_POINTER;
		if (riid == IID_IUnknown || riid == CLSID_StubAsio) { *o = this; AddRef(); return S_OK; }
		*o = nullptr; return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
	ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --m_ref; if (!n) delete this; return n; }

	ASIOBool init(void*) override { return 1; }
	void getDriverName(char* name) override { if (name) strcpy_s(name, 32, "RSMods Stub ASIO"); }
	long getDriverVersion() override { return 1; }
	void getErrorMessage(char* s) override { if (s) s[0] = 0; }
	ASIOError getChannels(long* in, long* out) override { if (in) *in = kChannels; if (out) *out = kChannels; return 0; }
	ASIOError getLatencies(long* in, long* out) override { if (in) *in = kFrames; if (out) *out = kFrames; return 0; }
	ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override
	{ if (mn) *mn = kFrames; if (mx) *mx = kFrames; if (pref) *pref = kFrames; if (gran) *gran = 0; return 0; }
	ASIOError canSampleRate(ASIOSampleRate r) override { return r == 48000.0 ? 0 : -1; }
	ASIOError getSampleRate(ASIOSampleRate* r) override { if (r) *r = 48000.0; return 0; }
	ASIOError setSampleRate(ASIOSampleRate) override { return 0; }
	ASIOError getClockSources(ASIOClockSource* c, long* n) override { if (n) *n = 0; (void)c; return 0; }
	ASIOError setClockSource(long) override { return 0; }
	ASIOError getSamplePosition(ASIOSamples*, ASIOTimeStamp*) override { return 0; }
	ASIOError getChannelInfo(ASIOChannelInfo* info) override
	{
		if (!info) return -1;
		info->isActive = 1; info->channelGroup = 0; info->type = ASIOSTInt32LSB;
		strcpy_s(info->name, 32, info->isInput ? "In" : "Out");
		return 0;
	}
	ASIOError createBuffers(ASIOBufferInfo* infos, long numChannels, long bufferSize, ASIOCallbacks* cb) override
	{
		if (g_createBuffersFailed.load(std::memory_order_acquire)) return -1;
		if (!infos || !cb || bufferSize != kFrames) return -1;
		m_callbacks = *cb;
		m_numChannels = numChannels;
		for (long i = 0; i < numChannels; ++i)
			for (int d = 0; d < 2; ++d)
			{
				void* buf = ::calloc(static_cast<size_t>(bufferSize), sizeof(int32_t));
				infos[i].buffers[d] = buf;
				m_owned[i][d] = buf;
			}
		return 0;
	}
	ASIOError disposeBuffers() override
	{
		g_disposeCalls.fetch_add(1, std::memory_order_relaxed);
		Stop();
		for (long i = 0; i < m_numChannels; ++i)
			for (int d = 0; d < 2; ++d) { ::free(m_owned[i][d]); m_owned[i][d] = nullptr; }
		m_numChannels = 0;
		return 0;
	}
	ASIOError start() override
	{
		if (!m_callbacks.bufferSwitch) return -1;
		m_run = true;
		m_thread = std::thread([this]
		{
			long index = 0;
			for (int i = 0; m_run; ++i)
			{
				if (g_callbackStalled.load(std::memory_order_acquire))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}
				m_callbacks.bufferSwitch(index, 0);   // host fills output buffers[index]; proxy taps
				index ^= 1;
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		});
		return 0;
	}
	ASIOError stop() override { g_stopCalls.fetch_add(1, std::memory_order_relaxed); Stop(); return 0; }
	ASIOError controlPanel() override { return 0; }
	ASIOError future(long, void*) override { return 0; }
	ASIOError outputReady() override { return 0; }
	void InvokeCallbackForTest() { if (m_callbacks.bufferSwitch) m_callbacks.bufferSwitch(0, 0); }

private:
	void Stop() { m_run = false; if (m_thread.joinable()) m_thread.join(); }

	std::atomic<ULONG> m_ref{ 1 };
	ASIOCallbacks m_callbacks{};
	long m_numChannels = 0;
	void* m_owned[16][2]{};
	std::thread m_thread;
	std::atomic<bool> m_run{ false };
};

class StubFactory : public IClassFactory
{
public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** o) override
	{ if (!o) return E_POINTER; if (riid == IID_IUnknown || riid == IID_IClassFactory) { *o = this; return S_OK; } *o = nullptr; return E_NOINTERFACE; }
	ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
	ULONG STDMETHODCALLTYPE Release() override { return 1; }
	HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object) override
	{
		if (outer) return CLASS_E_NOAGGREGATION;
		auto* d = new (std::nothrow) StubAsioDriver();
		if (!d) return E_OUTOFMEMORY;
		g_lastDriver.store(d, std::memory_order_release);
		const HRESULT hr = d->QueryInterface(riid, object);
		d->Release();
		return hr;
	}
	HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};

static StubFactory g_stubFactory;

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID riid, void** object)
{
	if (clsid == CLSID_StubAsio) return g_stubFactory.QueryInterface(riid, object);
	if (object) *object = nullptr;
	return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" __declspec(dllexport) void RSModsStub_InvokeLateCallback()
{
	StubAsioDriver* driver = g_lastDriver.load(std::memory_order_acquire);
	if (driver) driver->InvokeCallbackForTest();
}
extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow() { return S_OK; }
BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(m); return TRUE; }
