#include "stdafx.h"
#include "ModernFastBoot.hpp"
#include "../Audio/ComVTable.hpp"

#include <new>
#include <Wbemidl.h>

namespace Startup::ModernFastBoot
{
	namespace
	{
		constexpr size_t SLOT_WBEM_LOCATOR_CONNECT_SERVER = 3;
		constexpr size_t SLOT_WBEM_SERVICES_CREATE_INSTANCE_ENUM = 18;
		const CLSID WBEM_LOCATOR_CLASS_ID = { 0x4590F811, 0x1D3A, 0x11D0, { 0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 } };

		using ConnectServer = HRESULT(STDMETHODCALLTYPE*)(
			IWbemLocator*,
			const BSTR,
			const BSTR,
			const BSTR,
			const BSTR,
			long,
			const BSTR,
			IWbemContext*,
			IWbemServices**);

		using CreateInstanceEnum = HRESULT(STDMETHODCALLTYPE*)(
			IWbemServices*,
			const BSTR,
			long,
			IWbemContext*,
			IEnumWbemClassObject**);

		struct VTableHook
		{
			void** vtable;
			void* originalFunction;
		};

		std::mutex hookMutex;
		std::vector<VTableHook> locatorHooks;
		std::vector<VTableHook> servicesHooks;
		std::atomic<unsigned long> pnpEnumerationCount{ 0 };
		std::atomic<bool> isInstalled{ false };
		std::atomic<bool> isStartupSuppressionEnabled{ false };
		std::atomic<bool> hasStartupCompleted{ false };

		class EmptyWbemEnumerator final : public IEnumWbemClassObject
		{
		public:
			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void** object) override
			{
				if (!object) return E_POINTER;

				if (IsEqualIID(interfaceId, IID_IUnknown) || IsEqualIID(interfaceId, __uuidof(IEnumWbemClassObject)))
				{
					*object = static_cast<IEnumWbemClassObject*>(this);
					AddRef();
					return S_OK;
				}

				*object = nullptr;
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef() override
			{
				return referenceCount.fetch_add(1, std::memory_order_relaxed) + 1;
			}

			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG remaining = referenceCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
				if (remaining == 0) delete this;
				return remaining;
			}

			HRESULT STDMETHODCALLTYPE Reset() override
			{
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE Next(long, ULONG, IWbemClassObject**, ULONG* returnedCount) override
			{
				if (returnedCount) *returnedCount = 0;
				return WBEM_S_FALSE;
			}

			HRESULT STDMETHODCALLTYPE NextAsync(ULONG, IWbemObjectSink*) override
			{
				return WBEM_E_NOT_SUPPORTED;
			}

			HRESULT STDMETHODCALLTYPE Clone(IEnumWbemClassObject** enumerator) override
			{
				if (!enumerator) return E_POINTER;

				*enumerator = new (std::nothrow) EmptyWbemEnumerator();
				return *enumerator ? S_OK : E_OUTOFMEMORY;
			}

			HRESULT STDMETHODCALLTYPE Skip(long, ULONG) override
			{
				return WBEM_S_FALSE;
			}

		private:
			std::atomic<ULONG> referenceCount{ 1 };
		};

		void HookWbemServices(IWbemServices* services);

		void* FindOriginalFunction(void* object, const std::vector<VTableHook>& hooks)
		{
			if (!object) return nullptr;

			void** vtable = Audio::ComVTable::GetVTable(object);
			std::lock_guard<std::mutex> lock(hookMutex);
			for (const auto& hook : hooks)
			{
				if (hook.vtable == vtable) return hook.originalFunction;
			}

			return nullptr;
		}

		bool HookVTable(
			void* object,
			size_t slotIndex,
			void* replacement,
			std::vector<VTableHook>& hooks,
			const char* interfaceName)
		{
			if (!object) return false;

			void** vtable = Audio::ComVTable::GetVTable(object);
			std::lock_guard<std::mutex> lock(hookMutex);
			for (const auto& hook : hooks)
			{
				if (hook.vtable == vtable) return true;
			}

			hooks.push_back({ vtable, vtable[slotIndex] });
			void* originalFunction = Audio::ComVTable::PatchSlot(object, slotIndex, replacement);
			if (!originalFunction)
			{
				hooks.pop_back();
				LOG_ERROR("[ModernFastBoot] Could not patch " << interfaceName << "." << std::endl);
				return false;
			}

			hooks.back().originalFunction = originalFunction;
			LOG_INFO("[ModernFastBoot] " << interfaceName << " hooked at vtable " << vtable << "." << std::endl);
			return true;
		}

		HRESULT STDMETHODCALLTYPE HookCreateInstanceEnum(
			IWbemServices* self,
			const BSTR className,
			long flags,
			IWbemContext* context,
			IEnumWbemClassObject** enumerator)
		{
			auto originalFunction = reinterpret_cast<CreateInstanceEnum>(FindOriginalFunction(self, servicesHooks));
			if (!originalFunction)
			{
				LOG_ERROR("[ModernFastBoot] Missing original IWbemServices::CreateInstanceEnum function." << std::endl);
				return E_UNEXPECTED;
			}

			if (!className || _wcsicmp(className, L"Win32_PNPEntity") != 0)
				return originalFunction(self, className, flags, context, enumerator);

			if (!isStartupSuppressionEnabled.load(std::memory_order_acquire))
				return originalFunction(self, className, flags, context, enumerator);

			const unsigned long enumerationIndex = pnpEnumerationCount.fetch_add(1, std::memory_order_relaxed) + 1;
			if (enumerationIndex == 1)
			{
				const auto startedAt = std::chrono::steady_clock::now();
				const HRESULT result = originalFunction(self, className, flags, context, enumerator);
				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - startedAt);

				LOG_INFO("[ModernFastBoot] Initial Win32_PNPEntity enumeration completed in "
					<< elapsed.count() << " ms, hr=0x" << std::hex << result << std::dec << std::endl);
				if (FAILED(result))
				{
					isStartupSuppressionEnabled.store(false, std::memory_order_release);
					LOG_ERROR("[ModernFastBoot] Initial PnP enumeration failed; startup requests will run normally."
						<< std::endl);
				}

				return result;
			}

			if (!enumerator) return E_POINTER;

			*enumerator = new (std::nothrow) EmptyWbemEnumerator();
			if (!*enumerator) return E_OUTOFMEMORY;

			LOG_INFO("[ModernFastBoot] Skipped redundant startup Win32_PNPEntity enumeration "
				<< enumerationIndex << "." << std::endl);
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE HookConnectServer(
			IWbemLocator* self,
			const BSTR networkResource,
			const BSTR user,
			const BSTR password,
			const BSTR locale,
			long securityFlags,
			const BSTR authority,
			IWbemContext* context,
			IWbemServices** services)
		{
			auto originalFunction = reinterpret_cast<ConnectServer>(FindOriginalFunction(self, locatorHooks));
			if (!originalFunction)
			{
				LOG_ERROR("[ModernFastBoot] Missing original IWbemLocator::ConnectServer function." << std::endl);
				return E_UNEXPECTED;
			}

			const HRESULT result = originalFunction(
				self,
				networkResource,
				user,
				password,
				locale,
				securityFlags,
				authority,
				context,
				services);

			const bool isRocksmithPnpNamespace = networkResource
				&& _wcsicmp(networkResource, L"ROOT\\CIMV2") == 0;
			if (SUCCEEDED(result) && isRocksmithPnpNamespace && services && *services)
				HookWbemServices(*services);

			return result;
		}

		void HookWbemServices(IWbemServices* services)
		{
			HookVTable(
				services,
				SLOT_WBEM_SERVICES_CREATE_INSTANCE_ENUM,
				HookCreateInstanceEnum,
				servicesHooks,
				"IWbemServices::CreateInstanceEnum");
		}
	}

	void TryHookWbemLocator(REFCLSID classId, REFIID interfaceId, void* createdObject)
	{
		if (!GetModuleHandleA("RS_ASIO.dll")) return;
		if (hasStartupCompleted.load(std::memory_order_acquire)) return;
		if (!createdObject) return;
		if (!IsEqualCLSID(classId, WBEM_LOCATOR_CLASS_ID)) return;
		if (!IsEqualIID(interfaceId, __uuidof(IWbemLocator))) return;

		if (!HookVTable(
			createdObject,
			SLOT_WBEM_LOCATOR_CONNECT_SERVER,
			HookConnectServer,
			locatorHooks,
			"IWbemLocator::ConnectServer"))
		{
			return;
		}

		if (!isInstalled.exchange(true, std::memory_order_acq_rel))
		{
			pnpEnumerationCount.store(0, std::memory_order_relaxed);
			isStartupSuppressionEnabled.store(true, std::memory_order_release);
			LOG_INFO("[ModernFastBoot] RS_ASIO startup acceleration enabled." << std::endl);
		}
	}

	void CompleteStartupSuppression()
	{
		if (hasStartupCompleted.exchange(true, std::memory_order_acq_rel)) return;
		if (!isInstalled.load(std::memory_order_acquire)) return;
		if (!isStartupSuppressionEnabled.exchange(false, std::memory_order_acq_rel)) return;

		const unsigned long enumerationCount = pnpEnumerationCount.load(std::memory_order_relaxed);
		LOG_INFO("[ModernFastBoot] Startup acceleration complete after " << enumerationCount
			<< " PnP request(s); later enumerations will run normally." << std::endl);
	}
}
