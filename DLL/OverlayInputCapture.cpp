#include "stdafx.h"
#include "OverlayInputCapture.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

namespace OverlayInputCapture
{
	namespace
	{
		constexpr size_t DIRECT_INPUT_VTABLE_SIZE = 32;
		constexpr size_t DIRECT_INPUT_DEVICE_VTABLE_SIZE = 32;
		constexpr size_t CREATE_DEVICE_SLOT = 3;
		constexpr size_t GET_DEVICE_STATE_SLOT = 9;
		constexpr size_t GET_DEVICE_DATA_SLOT = 10;

		using DirectInput8CreateFunction = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
		using CreateDeviceFunction = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
		using GetDeviceStateFunction = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPVOID);
		using GetDeviceDataFunction = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

		std::atomic<bool> mouseButtonsCaptured{ false };
		DirectInput8CreateFunction originalDirectInput8Create = nullptr;
		CreateDeviceFunction originalCreateDevice = nullptr;
		GetDeviceStateFunction originalGetDeviceState = nullptr;
		GetDeviceDataFunction originalGetDeviceData = nullptr;
		void** directInputImportSlot = nullptr;
		std::atomic<bool> installed{ false };
		std::atomic<bool> loggedStateRead{ false };
		std::atomic<bool> loggedDataRead{ false };
		std::atomic<bool> loggedCaptureEnabled{ false };

		HRESULT STDMETHODCALLTYPE HookGetDeviceState(IDirectInputDevice8A* self, DWORD size, LPVOID data);
		HRESULT STDMETHODCALLTYPE HookGetDeviceData(
			IDirectInputDevice8A* self,
			DWORD size,
			LPDIDEVICEOBJECTDATA data,
			LPDWORD itemCount,
			DWORD flags);

		const GUID DIRECT_INPUT8_A_GUID =
		{ 0xbf798031, 0x483a, 0x4da2, { 0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00 } };
		const GUID DIRECT_INPUT8_W_GUID =
		{ 0xbf798030, 0x483a, 0x4da2, { 0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00 } };

		bool IsMouseButtonOffset(DWORD offset)
		{
			return offset >= DIMOFS_BUTTON0 && offset <= DIMOFS_BUTTON7;
		}

		bool IsMouseDevice(IDirectInputDevice8A* device)
		{
			DIDEVICEINSTANCEA info = {};
			info.dwSize = sizeof(info);
			return SUCCEEDED(device->GetDeviceInfo(&info))
				&& GET_DIDEVICE_TYPE(info.dwDevType) == DI8DEVTYPE_MOUSE;
		}

		void HookDevice(IDirectInputDevice8A* device)
		{
			if (device == nullptr) return;

			auto** originalVtable = reinterpret_cast<void***>(device);
			if (*originalVtable == nullptr) return;

			auto* hookedVtable = static_cast<void**>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
				DIRECT_INPUT_DEVICE_VTABLE_SIZE * sizeof(void*)));
			if (hookedVtable == nullptr)
			{
				LOG_ERROR("[OverlayInputCapture] Could not allocate the DirectInput mouse vtable." << std::endl);
				return;
			}

			std::memcpy(hookedVtable, *originalVtable, DIRECT_INPUT_DEVICE_VTABLE_SIZE * sizeof(void*));
			if (originalGetDeviceState == nullptr)
				originalGetDeviceState = reinterpret_cast<GetDeviceStateFunction>(hookedVtable[GET_DEVICE_STATE_SLOT]);
			if (originalGetDeviceData == nullptr)
				originalGetDeviceData = reinterpret_cast<GetDeviceDataFunction>(hookedVtable[GET_DEVICE_DATA_SLOT]);

			hookedVtable[GET_DEVICE_STATE_SLOT] = reinterpret_cast<void*>(&HookGetDeviceState);
			hookedVtable[GET_DEVICE_DATA_SLOT] = reinterpret_cast<void*>(&HookGetDeviceData);
			*originalVtable = hookedVtable;
			LOG_INFO("[OverlayInputCapture] Rocksmith DirectInput mouse device hooked." << std::endl);
		}

		HRESULT STDMETHODCALLTYPE HookCreateDevice(
			IDirectInput8A* self,
			REFGUID guid,
			LPDIRECTINPUTDEVICE8A* device,
			LPUNKNOWN outer)
		{
			const HRESULT result = originalCreateDevice(self, guid, device, outer);
			if (SUCCEEDED(result) && device != nullptr && *device != nullptr && IsMouseDevice(*device))
				HookDevice(*device);
			return result;
		}

		void HookDirectInput(IDirectInput8A* directInput)
		{
			if (directInput == nullptr) return;

			auto** originalVtable = reinterpret_cast<void***>(directInput);
			if (*originalVtable == nullptr) return;

			auto* hookedVtable = static_cast<void**>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
				DIRECT_INPUT_VTABLE_SIZE * sizeof(void*)));
			if (hookedVtable == nullptr)
			{
				LOG_ERROR("[OverlayInputCapture] Could not allocate the DirectInput vtable." << std::endl);
				return;
			}

			std::memcpy(hookedVtable, *originalVtable, DIRECT_INPUT_VTABLE_SIZE * sizeof(void*));
			if (originalCreateDevice == nullptr)
				originalCreateDevice = reinterpret_cast<CreateDeviceFunction>(hookedVtable[CREATE_DEVICE_SLOT]);
			hookedVtable[CREATE_DEVICE_SLOT] = reinterpret_cast<void*>(&HookCreateDevice);
			*originalVtable = hookedVtable;
		}

		HRESULT WINAPI HookDirectInput8Create(
			HINSTANCE instance,
			DWORD version,
			REFIID riid,
			LPVOID* output,
			LPUNKNOWN outer)
		{
			const HRESULT result = originalDirectInput8Create(instance, version, riid, output, outer);
			if (SUCCEEDED(result) && output != nullptr && *output != nullptr
				&& (IsEqualGUID(riid, DIRECT_INPUT8_A_GUID) || IsEqualGUID(riid, DIRECT_INPUT8_W_GUID)))
			{
				HookDirectInput(reinterpret_cast<IDirectInput8A*>(*output));
				LOG_INFO("[OverlayInputCapture] Rocksmith created its DirectInput 8 interface." << std::endl);
			}
			return result;
		}

		bool PatchDirectInputImport()
		{
			auto* module = reinterpret_cast<HMODULE>(GetModuleHandleW(nullptr));
			auto* dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
			if (dosHeader == nullptr || dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return false;

			auto* ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<BYTE*>(module) + dosHeader->e_lfanew);
			const auto& directory = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (directory.VirtualAddress == 0) return false;

			auto* imports = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(reinterpret_cast<BYTE*>(module) + directory.VirtualAddress);
			for (; imports->Name != 0; ++imports)
			{
				const char* moduleName = reinterpret_cast<const char*>(reinterpret_cast<BYTE*>(module) + imports->Name);
				if (_stricmp(moduleName, "DINPUT8.dll") != 0) continue;

				auto* names = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(module) + imports->OriginalFirstThunk);
				auto* functions = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(module) + imports->FirstThunk);
				for (; names->u1.AddressOfData != 0; ++names, ++functions)
				{
					if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
					auto* importName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(reinterpret_cast<BYTE*>(module) + names->u1.AddressOfData);
					if (std::strcmp(reinterpret_cast<const char*>(importName->Name), "DirectInput8Create") != 0) continue;

					DWORD oldProtection = 0;
					if (!VirtualProtect(&functions->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtection)) return false;
					originalDirectInput8Create = reinterpret_cast<DirectInput8CreateFunction>(functions->u1.Function);
					directInputImportSlot = reinterpret_cast<void**>(&functions->u1.Function);
					functions->u1.Function = reinterpret_cast<ULONG_PTR>(&HookDirectInput8Create);
					DWORD unused = 0;
					VirtualProtect(&functions->u1.Function, sizeof(void*), oldProtection, &unused);
					FlushInstructionCache(GetCurrentProcess(), &functions->u1.Function, sizeof(void*));
					return true;
				}
			}
			return false;
		}

		HRESULT STDMETHODCALLTYPE HookGetDeviceState(IDirectInputDevice8A* self, DWORD size, LPVOID data)
		{
			if (!loggedStateRead.exchange(true, std::memory_order_relaxed))
				LOG_INFO("[OverlayInputCapture] Rocksmith reads mouse state through GetDeviceState." << std::endl);
			const HRESULT result = originalGetDeviceState(self, size, data);
			if (SUCCEEDED(result) && mouseButtonsCaptured.load(std::memory_order_relaxed) && data != nullptr)
			{
				if (size >= sizeof(DIMOUSESTATE2))
				{
					auto* state = static_cast<DIMOUSESTATE2*>(data);
					std::memset(state->rgbButtons, 0, sizeof(state->rgbButtons));
				}
				else if (size >= sizeof(DIMOUSESTATE))
				{
					auto* state = static_cast<DIMOUSESTATE*>(data);
					std::memset(state->rgbButtons, 0, sizeof(state->rgbButtons));
				}
			}
			return result;
		}

		HRESULT STDMETHODCALLTYPE HookGetDeviceData(
			IDirectInputDevice8A* self,
			DWORD size,
			LPDIDEVICEOBJECTDATA data,
			LPDWORD itemCount,
			DWORD flags)
		{
			if (!loggedDataRead.exchange(true, std::memory_order_relaxed))
				LOG_INFO("[OverlayInputCapture] Rocksmith reads buffered mouse events through GetDeviceData." << std::endl);
			const HRESULT result = originalGetDeviceData(self, size, data, itemCount, flags);
			if (FAILED(result) || !mouseButtonsCaptured.load(std::memory_order_relaxed)
				|| data == nullptr || itemCount == nullptr || size < sizeof(DWORD))
				return result;

			DWORD writeIndex = 0;
			auto* bytes = reinterpret_cast<BYTE*>(data);
			for (DWORD readIndex = 0; readIndex < *itemCount; ++readIndex)
			{
				auto* source = bytes + readIndex * size;
				const DWORD offset = *reinterpret_cast<DWORD*>(source);
				if (IsMouseButtonOffset(offset)) continue;
				if (writeIndex != readIndex)
					std::memmove(bytes + writeIndex * size, source, size);
				++writeIndex;
			}
			*itemCount = writeIndex;
			return result;
		}
	}

	bool Install()
	{
		if (installed.load(std::memory_order_acquire)) return true;
		if (!PatchDirectInputImport())
		{
			LOG_ERROR("[OverlayInputCapture] DirectInput8Create import was not found; mouse button capture is unavailable." << std::endl);
			return false;
		}
		installed.store(true, std::memory_order_release);
		LOG_INFO("[OverlayInputCapture] DirectInput mouse button capture installed." << std::endl);
		return true;
	}

	void SetMouseCapture(bool shouldCapture)
	{
		mouseButtonsCaptured.store(shouldCapture, std::memory_order_relaxed);
		if (shouldCapture && !loggedCaptureEnabled.exchange(true, std::memory_order_relaxed))
			LOG_INFO("[OverlayInputCapture] ImGui requested DirectInput mouse-button capture." << std::endl);
	}

	void Shutdown()
	{
		mouseButtonsCaptured.store(false, std::memory_order_relaxed);
		if (directInputImportSlot != nullptr && originalDirectInput8Create != nullptr)
		{
			DWORD oldProtection = 0;
			if (VirtualProtect(directInputImportSlot, sizeof(void*), PAGE_READWRITE, &oldProtection))
			{
				*directInputImportSlot = reinterpret_cast<void*>(originalDirectInput8Create);
				DWORD unused = 0;
				VirtualProtect(directInputImportSlot, sizeof(void*), oldProtection, &unused);
			}
		}
		installed.store(false, std::memory_order_release);
	}
}
