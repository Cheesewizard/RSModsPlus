#include "../stdafx.h"
#include "NoteByNoteMenu.hpp"

#include "NoteByNoteProbe.hpp"
#include "../GameState.hpp"
#include "../OverlayToggles.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>


namespace
{
	constexpr uintptr_t RIFF_REPEATER_INPUT_DISPATCHER = 0x00630800;
	constexpr uintptr_t RIFF_REPEATER_ADJUST_SLIDER = 0x00633150;
	// The Advanced Settings stage builder: __stdcall(controller, int* sortOrderCounter), RET 8.
	// It creates the eight shipped rows in code and numbers each SortOrder = *counter++; its
	// caller seeds *counter = (highest SortOrder among the manifest's JSON rows) + 1, so a
	// manifest row is ALWAYS numbered below the game's rows and therefore drawn first with the
	// cursor on it, and "down" walks into the timeline markers. No manifest value can change
	// that. The post-hook below renumbers our row to the counter's final value once the game's
	// rows exist, which is also the primitive any future mod-added row will use.
	constexpr uintptr_t RIFF_REPEATER_STAGE_BUILDER = 0x006397D0;
	constexpr uintptr_t VIEW_CONTAINER_OFFSET = 0x1C4;      // controller+0x1C4: the row container
	constexpr uintptr_t CONTAINER_ENUMERATE_SLOT = 0x74;    // (old container class only) fills {begin,end,cap}
	constexpr uintptr_t CONTAINER_GET_BY_NAME_SLOT = 0x18;  // (const char* name) -> row node (AdjustSlider 0x633150)
	constexpr uintptr_t NODE_RESOLVE_SLOT = 0x5C;           // () -> resolved row object (smart-ptr get)
	constexpr uintptr_t CHILD_GET_MEMBER_SLOT = 0x14;       // (const char* name) -> member or null
	constexpr uintptr_t MEMBER_AS_NUMBER_SLOT = 0x50;       // () -> holder or null; double at holder+8
	constexpr uintptr_t NUMBER_HOLDER_VALUE_OFFSET = 0x8;
	// push ebp; mov ebp,esp; and esp,-40h; sub esp,0B4h; mov eax,[cookie]
	const byte STAGE_BUILDER_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0, 0x81, 0xEC, 0xB4, 0x00, 0x00, 0x00, 0xA1 };
	using StageBuilder = void(__stdcall*)(void* controller, int* sortOrderCounter);
	StageBuilder originalStageBuilder = nullptr;
	constexpr const char* NOTE_BY_NOTE_SLIDER_ID = "NoteByNote";
	constexpr unsigned int POLL_FRAME_INTERVAL = 3;
	constexpr int UNWRITTEN = -12345;

	// push ebp; mov ebp,esp; and esp,-8; sub esp,44h; mov eax,[cookie]
	const byte DISPATCHER_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x44, 0xA1 };
	const byte ADJUST_SLIDER_PROLOGUE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x70, 0xA1 };

	void* originalDispatcher = nullptr;

	// Written by the dispatcher thunk on the game's UI thread, read by Poll on the same thread.
	void* volatile dispatcherController = nullptr;
	volatile unsigned long dispatcherGeneration = 0;

	// The controller's name string object (screen setup 0x62E770 at 0x62F6F2 compares it
	// with "RiffRepeater_AdvancedSettings"): inline text at +0x78 unless the pointer at
	// +0x8C is not the address +0x88, in which case the heap pointer at +0x78 is the text;
	// end pointer at +0x88.
	constexpr uintptr_t CONTROLLER_NAME_OFFSET = 0x78;
	constexpr const char* ADVANCED_SETTINGS_CONTROLLER_NAME = "RiffRepeater_AdvancedSettings";
	// Learned from the stage builder, which only ever runs with a live Advanced Settings
	// controller; every adopted pointer must carry the same vtable.
	uintptr_t knownControllerVtable = 0;

	// Render-thread state.
	unsigned int frameCounter = 0;
	unsigned long adoptedGeneration = 0;
	void* controller = nullptr;
	std::string controllerMenu;
	bool hasFocusedValue = false;
	int lastFocusedValue = -1;
	bool loggedFault = false;
	int menuSettleFrames = 0;

	std::mutex diagnosticsMutex;
	NoteByNoteMenu::Diagnostics diagnostics;

	// Read one dword at an address, SEH-guarded. Returns 0 on fault (and sets ok=false).
	unsigned long SafeReadDword(uintptr_t address, bool& ok)
	{
		ok = false;
		__try
		{
			unsigned long value = *reinterpret_cast<volatile unsigned long*>(address);
			ok = true;
			return value;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	// container = *(controller+0x1C4); vtable = *container; slot74 = *(vtable+0x74). Each read is
	// guarded so a dangling/replaced container costs a zero, never a crash.
	void SampleContainerVtable(void* controller, unsigned long& containerOut,
		unsigned long& vtableOut, unsigned long& slot74Out)
	{
		containerOut = 0; vtableOut = 0; slot74Out = 0;
		if (controller == nullptr) return;
		bool ok = false;
		const uintptr_t container = SafeReadDword(reinterpret_cast<uintptr_t>(controller) + VIEW_CONTAINER_OFFSET, ok);
		if (!ok) return;
		containerOut = static_cast<unsigned long>(container);
		if (container == 0) return;
		const uintptr_t vtable = SafeReadDword(container, ok);
		if (!ok) return;
		vtableOut = static_cast<unsigned long>(vtable);
		if (vtable == 0) return;
		slot74Out = SafeReadDword(vtable + CONTAINER_ENUMERATE_SLOT, ok);
	}

	bool MatchesBytes(uintptr_t address, const byte* expected, size_t count)
	{
		__try
		{
			return std::memcmp(reinterpret_cast<const void*>(address), expected, count) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Callee-cleaned stack, ecx = out. Returns true when *out was written (focused read, or a
	// named move that changed the value). The return register is not trusted.
	bool AdjustSliderUnguarded(void* target, int delta, int* value)
	{
		const char* sliderId = NOTE_BY_NOTE_SLIDER_ID;
		// The address must live in a variable: `call dword ptr [constant]` would read the
		// function's first bytes as the target (the 12:23 crash, EIP 0x3158E7E0).
		const uintptr_t function = RIFF_REPEATER_ADJUST_SLIDER;
		__asm
		{
			mov ecx, value
			push delta
			push sliderId
			push target
			call dword ptr[function]
		}
		return *value != UNWRITTEN;
	}

	constexpr uintptr_t FOCUSED_COMPONENT_OFFSET = 0x5C;

	// SEH boundary (no objects with destructors in here): a stale controller faults inside the
	// game's lookup; that becomes "drop the pointer" instead of a crash.
	bool AdjustSliderGuarded(void* target, int delta, int& value, bool& faulted, bool blindToFocus)
	{
		value = UNWRITTEN;
		faulted = false;
		void** focusedSlot = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(target) + FOCUSED_COMPONENT_OFFSET);
		void* savedFocused = nullptr;
		bool blanked = false;
		__try
		{
			if (blindToFocus)
			{
				savedFocused = *focusedSlot;
				*focusedSlot = nullptr;
				blanked = true;
			}
			const bool written = AdjustSliderUnguarded(target, delta, &value);
			if (blanked) *focusedSlot = savedFocused;
			return written;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (blanked) *focusedSlot = savedFocused;
			faulted = true;
			return false;
		}
	}

	__declspec(naked) void DispatcherDetour()
	{
		__asm
		{
			mov dispatcherController, ecx
			inc dispatcherGeneration
			jmp dword ptr[originalDispatcher]
		}
	}

	// --- Row renumbering after the game built its rows (mirrors the caller's own scan) ---

	struct ChildRange { void** begin; void** end; void** capacity; };

	template <typename Fn>
	Fn VirtualSlot(void* object, uintptr_t slotOffset)
	{
		const uintptr_t vtable = *reinterpret_cast<uintptr_t*>(object);
		return *reinterpret_cast<Fn*>(vtable + slotOffset);
	}

	void EnumerateChildren(void* container, ChildRange* range)
	{
		using Fn = void(__fastcall*)(void* self, void* edx, ChildRange* out);
		VirtualSlot<Fn>(container, CONTAINER_ENUMERATE_SLOT)(container, nullptr, range);
	}

	void* GetMember(void* child, const char* name)
	{
		using Fn = void*(__fastcall*)(void* self, void* edx, const char* name);
		return VirtualSlot<Fn>(child, CHILD_GET_MEMBER_SLOT)(child, nullptr, name);
	}

	void* MemberAsNumber(void* member)
	{
		using Fn = void*(__fastcall*)(void* self, void* edx);
		return VirtualSlot<Fn>(member, MEMBER_AS_NUMBER_SLOT)(member, nullptr);
	}

	// Get a row node from the container by name, the way AdjustSlider (0x633150) does:
	// container->vtable[0x18](name). Kept for the realized container (Poll-time probes).
	void* GetChildByName(void* container, const char* name)
	{
		using Fn = void*(__fastcall*)(void* self, void* edx, const char* name);
		return VirtualSlot<Fn>(container, CONTAINER_GET_BY_NAME_SLOT)(container, nullptr, name);
	}

	// Resolve a row node to its concrete object: node->vtable[0x5c]() (a smart-pointer get).
	void* ResolveNode(void* node)
	{
		using Fn = void*(__fastcall*)(void* self, void* edx);
		return VirtualSlot<Fn>(node, NODE_RESOLVE_SLOT)(node, nullptr);
	}

	// Finds a child's numeric member holder the way the game does: child->GetMember(name),
	// member->AsNumber() twice (first call reports presence, second returns the holder).
	// AdjustSlider (0x633150) reads the row's current value through exactly this path with
	// the name "Value", so the same recipe reaches the row's data model.
	double* FindNumberMember(void* child, const char* name)
	{
		if (child == nullptr) return nullptr;
		void* member = GetMember(child, name);
		if (member == nullptr) return nullptr;
		if (MemberAsNumber(member) == nullptr) return nullptr;
		void* holder = MemberAsNumber(member);
		if (holder == nullptr) return nullptr;
		return reinterpret_cast<double*>(reinterpret_cast<uintptr_t>(holder) + NUMBER_HOLDER_VALUE_OFFSET);
	}

	double* FindSortOrder(void* child)
	{
		return FindNumberMember(child, "SortOrder");
	}

	// member->AsString(): slot 0x60, called twice like AsNumber (presence, then holder).
	constexpr uintptr_t MEMBER_AS_STRING_SLOT = 0x60;
	void* MemberAsString(void* member)
	{
		using Fn = void*(__fastcall*)(void* self, void* edx);
		return VirtualSlot<Fn>(member, MEMBER_AS_STRING_SLOT)(member, nullptr);
	}

	const char* FindStringMember(void* child, const char* name)
	{
		if (child == nullptr) return nullptr;
		void* member = GetMember(child, name);
		if (member == nullptr) return nullptr;
		if (MemberAsString(member) == nullptr) return nullptr;
		void* holder = MemberAsString(member);
		if (holder == nullptr) return nullptr;
		char* text = *reinterpret_cast<char**>(reinterpret_cast<uintptr_t>(holder) + 8);
		if (text == nullptr) return nullptr;
		if (*reinterpret_cast<char**>(text + 0x14) != text + 0x10) text = *reinterpret_cast<char**>(text);
		return text;
	}

	enum : uintptr_t
	{
		INTERN_NAME_FN = 0x0087C470,
		MAKE_NUMBER_FN = 0x0087F930,
		SET_MEMBER_FN = 0x008816C0,
		RELEASE_NAME_FN = 0x0087C3D0,
	};

	void InternName(const char* name, uint32_t* outHandle)
	{
		using Fn = void(__fastcall*)(const char* name, void* edx, uint32_t* out);
		reinterpret_cast<Fn>(INTERN_NAME_FN)(name, nullptr, outHandle);
	}

	void MakeNumberValue(double value, void** outValue)
	{
		const uintptr_t fn = MAKE_NUMBER_FN;
		__asm
		{
			mov edi, outValue
			sub esp, 8
			fld value
			fstp qword ptr [esp]
			call dword ptr [fn]
			add esp, 8
		}
	}

	void SetMember(void* object, uint32_t* nameHandle, void** valuePtr)
	{
		using Fn = void(__stdcall*)(void* object, uint32_t* nameHandle, void** valuePtr);
		reinterpret_cast<Fn>(SET_MEMBER_FN)(object, nameHandle, valuePtr);
	}

	void ReleaseName(uint32_t handle)
	{
		const uintptr_t fn = RELEASE_NAME_FN;
		__asm
		{
			mov esi, handle
			call dword ptr [fn]
		}
	}

	void ValueAddRef(void* valueObject)
	{
		using Fn = void(__fastcall*)(void* self, void* edx);
		VirtualSlot<Fn>(valueObject, 0x0)(valueObject, nullptr);
	}

	void ValueRelease(void* valueObject)
	{
		using Fn = void(__fastcall*)(void* self, void* edx);
		VirtualSlot<Fn>(valueObject, 0x4)(valueObject, nullptr);
	}

	// Assigns a NEW number object to object.name. Returns false when the game gave no value
	// object back. Not SEH-guarded itself; callers run it inside their own __try.
	bool SetNumberMember(void* object, const char* name, double value)
	{
		uint32_t handle = 0;
		InternName(name, &handle);
		void* valueObject = nullptr;
		MakeNumberValue(value, &valueObject);
		if (valueObject == nullptr)
		{
			if (handle != 0) ReleaseName(handle);
			return false;
		}
		ValueAddRef(valueObject);
		void* valuePtr = valueObject;
		SetMember(object, &handle, &valuePtr);
		ValueRelease(valueObject);
		ValueRelease(valueObject);
		if (handle != 0) ReleaseName(handle);
		return true;
	}

	struct BuilderScan
	{
		int childCount;
		int rowFrom;
		int rowTo;
		int preset;           // bitmask 1 = InitialValue written, 2 = Value written; 0 = no member; -1 = row not found
		bool faulted;
		void* rowObject;      // our row's data object, found pre-builder, renumbered post-builder
		double* rowSortOrder; // holders, reported only (never written: they are shared flyweights)
		double* rowInitialValue;
		double* rowValue;
		char listing[1024];
	};

	void ScanBuilderRows(void* controller, bool presetOn, BuilderScan& scan)
	{
		scan.childCount = 0; scan.rowFrom = -1; scan.rowTo = -1; scan.preset = -1;
		scan.faulted = false; scan.rowObject = nullptr; scan.rowSortOrder = nullptr;
		scan.rowInitialValue = nullptr; scan.rowValue = nullptr; scan.listing[0] = 0;
		size_t used = 0;
		__try
		{
			void* container = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(controller) + VIEW_CONTAINER_OFFSET);
			if (container == nullptr) return;
			ChildRange range = {};
			EnumerateChildren(container, &range);
			for (void** at = range.begin; at != nullptr && at != range.end && scan.childCount < 32; ++at)
			{
				++scan.childCount;
				void* child = *at;
				const char* id = FindStringMember(child, "ID");
				double* sortOrder = FindSortOrder(child);
				const int written = std::snprintf(scan.listing + used, sizeof(scan.listing) - used,
					"%s%s:%d", scan.childCount > 1 ? " " : "", id != nullptr ? id : "?",
					sortOrder != nullptr ? static_cast<int>(*sortOrder) : -1);
				if (written > 0 && used + written < sizeof(scan.listing)) used += written;
				if (id == nullptr || std::strcmp(id, NOTE_BY_NOTE_SLIDER_ID) != 0) continue;
				scan.rowObject = child;
				scan.rowSortOrder = sortOrder;
				if (sortOrder != nullptr) scan.rowFrom = static_cast<int>(*sortOrder);
				scan.preset = 0;
				scan.rowInitialValue = FindNumberMember(child, "InitialValue");
				scan.rowValue = FindNumberMember(child, "Value");
				if (!presetOn) continue;
				// Fresh value objects, never a write into the shared holder.
				if (SetNumberMember(child, "InitialValue", 1.0)) scan.preset |= 1;
				if (SetNumberMember(child, "Value", 1.0)) scan.preset |= 2;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			scan.faulted = true;
		}
	}

	// Post-builder: number our row after the game's rows. SEH-guarded write through the
	// holder pointer captured pre-builder.
	bool RenumberScannedRow(BuilderScan& scan, int* counter)
	{
		__try
		{
			if (scan.rowObject == nullptr || counter == nullptr) return false;
			scan.rowTo = *counter;
			if (!SetNumberMember(scan.rowObject, "SortOrder", static_cast<double>(scan.rowTo)))
			{
				scan.rowTo = -1;
				return false;
			}
			*counter = scan.rowTo + 1;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			scan.rowTo = -1;
			return false;
		}
	}

	volatile bool rowProbeRequested = false;

	// POD-only SEH body (no objects with destructors): fills a fixed text buffer.
	bool ProbeRowsUnwound(void* target, uintptr_t& containerAddress, uintptr_t& rangeBegin,
		uintptr_t& rangeEnd, int& count, char* text, size_t textCapacity)
	{
		containerAddress = 0; rangeBegin = 0; rangeEnd = 0; count = 0; text[0] = 0;
		size_t used = 0;
		__try
		{
			void* container = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(target) + VIEW_CONTAINER_OFFSET);
			containerAddress = reinterpret_cast<uintptr_t>(container);
			if (container == nullptr) return true;
			ChildRange range = {};
			EnumerateChildren(container, &range);
			rangeBegin = reinterpret_cast<uintptr_t>(range.begin);
			rangeEnd = reinterpret_cast<uintptr_t>(range.end);
			for (void** at = range.begin; at != nullptr && at != range.end && count < 32; ++at)
			{
				++count;
				double* sortOrder = FindSortOrder(*at);
				const int written = sortOrder != nullptr
					? std::snprintf(text + used, textCapacity - used, "%s%p:%d", count > 1 ? " " : "", *at, static_cast<int>(*sortOrder))
					: std::snprintf(text + used, textCapacity - used, "%s%p:-", count > 1 ? " " : "", *at);
				if (written <= 0 || used + written >= textCapacity) break;
				used += written;
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void ProbeRows(void* target)
	{
		uintptr_t containerAddress = 0, rangeBegin = 0, rangeEnd = 0;
		int count = 0;
		char text[1024];
		const bool ok = ProbeRowsUnwound(target, containerAddress, rangeBegin, rangeEnd, count, text, sizeof(text));
		std::string listing = text;
		if (!ok) listing += " <fault>";
		unsigned long pContainer = 0, pVtable = 0, pSlot74 = 0;
		SampleContainerVtable(target, pContainer, pVtable, pSlot74);
		std::lock_guard<std::mutex> lock(diagnosticsMutex);
		diagnostics.probeContainer = containerAddress;
		diagnostics.probeRangeBegin = rangeBegin;
		diagnostics.probeRangeEnd = rangeEnd;
		diagnostics.probeChildCount = count;
		diagnostics.probeListing = listing;
		diagnostics.probeVtable = pVtable;
		diagnostics.probeSlot74 = pSlot74;
		++diagnostics.probes;
	}

	// SEH-guarded: true only when the pointer still carries the builder-learned vtable and
	// names itself the Advanced Settings screen. Both Riff Repeater screens share the menu
	// name "LearnASong_RiffRepeater", so the menu-name drop in Poll never fires on ESC back
	// to Practice Selection; the 21:27 first-chance dump (0xC0000409, a game stack-cookie
	// failure inside AdjustSlider on the destroyed controller, swallowed by our SEH) and
	// the 21:30 crash that followed were exactly that stale pointer.
	bool ControllerLooksLive(void* candidate)
	{
		if (candidate == nullptr || knownControllerVtable == 0) return false;
		__try
		{
			const uintptr_t base = reinterpret_cast<uintptr_t>(candidate);
			if (*reinterpret_cast<uintptr_t*>(base) != knownControllerVtable) return false;
			const char* text = reinterpret_cast<const char*>(base + CONTROLLER_NAME_OFFSET);
			if (*reinterpret_cast<uintptr_t*>(base + CONTROLLER_NAME_OFFSET + 0x14) != base + CONTROLLER_NAME_OFFSET + 0x10)
				text = *reinterpret_cast<const char* const*>(base + CONTROLLER_NAME_OFFSET);
			const char* end = *reinterpret_cast<const char* const*>(base + CONTROLLER_NAME_OFFSET + 0x10);
			const size_t length = static_cast<size_t>(end - text);
			if (length != std::strlen(ADVANCED_SETTINGS_CONTROLLER_NAME)) return false;
			return std::memcmp(text, ADVANCED_SETTINGS_CONTROLLER_NAME, length) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void __stdcall StageBuilderDetour(void* controller, int* counter)
	{
		knownControllerVtable = *reinterpret_cast<uintptr_t*>(controller);
		// Pre-builder: the container the caller just enumerated. Scan it, preset the row.
		unsigned long preContainer = 0, preVtable = 0, preSlot74 = 0;
		SampleContainerVtable(controller, preContainer, preVtable, preSlot74);
		// Both writes are live-switchable over the bridge (set_overlay menu_preset /
		// menu_renumber) so their effects can be separated without a restart: the first
		// working scan (21:19) renumbered SortOrder 0 -> 11 and the slider then READ BACK
		// 11 as its value, so one of the two number holders is not what its name says.
		const bool enabledAtBuild = NoteByNoteProbe::IsAutomaticEnabled()
			&& OverlayToggles::Get("menu_preset");
		static BuilderScan scan;   // UI thread only; static keeps the 1 KB listing off the stack
		ScanBuilderRows(controller, enabledAtBuild, scan);

		originalStageBuilder(controller, counter);

		// Post-builder: the counter is final, number our row after the game's rows.
		const bool moved = OverlayToggles::Get("menu_renumber") && RenumberScannedRow(scan, counter);
		unsigned long bContainer = 0, bVtable = 0, bSlot74 = 0;
		SampleContainerVtable(controller, bContainer, bVtable, bSlot74);
		{
			std::lock_guard<std::mutex> lock(diagnosticsMutex);
			diagnostics.preBuilderContainer = preContainer;
			diagnostics.preBuilderVtable = preVtable;
			diagnostics.preBuilderSlot74 = preSlot74;
			++diagnostics.stageBuilds;
			diagnostics.lastRowSortOrderFrom = scan.rowFrom;
			diagnostics.lastRowSortOrderTo = scan.rowTo;
			diagnostics.lastChildCount = scan.childCount;
			diagnostics.lastRowPreset = scan.preset;
			diagnostics.builderListing = scan.listing;
			if (scan.faulted) diagnostics.builderListing += " <fault>";
			diagnostics.lastBuilderController = reinterpret_cast<uintptr_t>(controller);
			diagnostics.lastBuilderCounter = counter != nullptr ? *counter : -1;
			diagnostics.builderContainer = bContainer;
			diagnostics.builderVtable = bVtable;
			diagnostics.builderSlot74 = bSlot74;
		}
		// The builder runs on the UI thread with the NEW screen's controller: adopt it here so
		// the row is repaired on the first poll instead of after the first key press.
		dispatcherController = controller;
		++dispatcherGeneration;
		LOG_INFO("(NBN MENU) Advanced Settings built: " << scan.childCount << " rows ["
			<< scan.listing << (scan.faulted ? " <fault>" : "") << "]; NOTE BY NOTE "
			<< (moved ? "renumbered " : "NOT renumbered (")
			<< scan.rowFrom << (moved ? " -> " : ")") << (moved ? std::to_string(scan.rowTo) : std::string())
			<< "; Note by Note is " << (enabledAtBuild ? "ON" : "OFF")
			<< (scan.preset == -1 ? ", row not found"
				: !enabledAtBuild ? ", row left at Off"
				: scan.preset == 0 ? ", row NOT preset (no InitialValue/Value member)"
				: ", row preset to On")
			<< (enabledAtBuild && scan.preset > 0
				? std::string(" (InitialValue ") + ((scan.preset & 1) ? "yes" : "no")
					+ ", Value " + ((scan.preset & 2) ? "yes" : "no") + ")"
				: std::string())
			<< ". Holders: SortOrder=0x" << std::hex << reinterpret_cast<uintptr_t>(scan.rowSortOrder)
			<< " InitialValue=0x" << reinterpret_cast<uintptr_t>(scan.rowInitialValue)
			<< " Value=0x" << reinterpret_cast<uintptr_t>(scan.rowValue) << std::dec << "." << std::endl);
	}

	void DropController(const char* reason)
	{
		if (controller != nullptr)
		{
			LOG_INFO("(NBN MENU) Riff Repeater controller dropped: " << reason << "." << std::endl);
		}
		controller = nullptr;
		controllerMenu.clear();
		hasFocusedValue = false;
	}
}

void NoteByNoteMenu::Initialize()
{
	if (!MatchesBytes(RIFF_REPEATER_INPUT_DISPATCHER, DISPATCHER_PROLOGUE, sizeof(DISPATCHER_PROLOGUE))
		|| !MatchesBytes(RIFF_REPEATER_ADJUST_SLIDER, ADJUST_SLIDER_PROLOGUE, sizeof(ADJUST_SLIDER_PROLOGUE)))
	{
		LOG_ERROR("(NBN MENU) The Riff Repeater functions do not match the expected game build;"
			<< " the NOTE BY NOTE rocker is not wired (N key and the bridge still work)." << std::endl);
		return;
	}

	originalDispatcher = DetourFunction(
		reinterpret_cast<byte*>(RIFF_REPEATER_INPUT_DISPATCHER),
		reinterpret_cast<byte*>(DispatcherDetour));
	if (originalDispatcher == nullptr)
	{
		LOG_ERROR("(NBN MENU) Could not hook the Riff Repeater input dispatcher." << std::endl);
		return;
	}

	if (MatchesBytes(RIFF_REPEATER_STAGE_BUILDER, STAGE_BUILDER_PROLOGUE, sizeof(STAGE_BUILDER_PROLOGUE)))
	{
		originalStageBuilder = reinterpret_cast<StageBuilder>(DetourFunction(
			reinterpret_cast<byte*>(RIFF_REPEATER_STAGE_BUILDER),
			reinterpret_cast<byte*>(StageBuilderDetour)));
		if (originalStageBuilder == nullptr)
		{
			LOG_ERROR("(NBN MENU) Could not hook the Advanced Settings stage builder;"
				<< " the NOTE BY NOTE row stays first in the list." << std::endl);
		}
	}
	else
	{
		LOG_ERROR("(NBN MENU) The Advanced Settings stage builder does not match the expected"
			<< " game build; the NOTE BY NOTE row stays first in the list." << std::endl);
	}

	{
		std::lock_guard<std::mutex> lock(diagnosticsMutex);
		diagnostics.hooked = true;
	}
	LOG_INFO("(NBN MENU) Riff Repeater rocker wired: dispatcher=0x" << std::hex
		<< RIFF_REPEATER_INPUT_DISPATCHER << " adjust=0x" << RIFF_REPEATER_ADJUST_SLIDER << std::dec
		<< "; the row is read while focused and repaired by name while not." << std::endl);
}

void NoteByNoteMenu::Poll()
{
	if (originalDispatcher == nullptr) return;

	if (!GameState::Menus::IsInRiffRepeaterMenus())
	{
		if (controller != nullptr) DropController("left the Riff Repeater menus");
		return;
	}

	const unsigned long generation = dispatcherGeneration;
	if (generation != adoptedGeneration)
	{
		adoptedGeneration = generation;
		void* const captured = dispatcherController;
		if (captured != controller)
		{
			controller = captured;
			controllerMenu = GameState::currentMenu;
			hasFocusedValue = false;
			menuSettleFrames = 30;
		}
	}
	// A screen change invalidates whatever was adopted, once the menu name has had a moment
	// to catch up with the screen the capture came from (the name is polled on another thread).
	if (menuSettleFrames > 0)
	{
		--menuSettleFrames;
		if (GameState::currentMenu != controllerMenu) controllerMenu = GameState::currentMenu;
	}
	else if (controller != nullptr && GameState::currentMenu != controllerMenu)
	{
		DropController("screen changed");
	}
	if (controller == nullptr) return;
	if (!ControllerLooksLive(controller))
	{
		DropController("not a live Advanced Settings controller");
		return;
	}
	if (rowProbeRequested)
	{
		rowProbeRequested = false;
		ProbeRows(controller);
	}
	if (++frameCounter % POLL_FRAME_INTERVAL != 0) return;

	const bool enabled = NoteByNoteProbe::IsAutomaticEnabled();
	bool faulted = false;
	int value = 0;

	// Focused read first (delta 0 never moves anything).
	if (AdjustSliderGuarded(controller, 0, value, faulted, false))
	{
		{
			std::lock_guard<std::mutex> lock(diagnosticsMutex);
			++diagnostics.focusedReads;
			diagnostics.lastRowValue = value;
		}
		// A change between reads is the player. On the first read after adoption the
		// builder preset has already created the row at the real state, so any
		// disagreement is a change made through the row (or an external toggle that the
		// row has not caught up with); either way the row is the player-facing truth.
		const bool userToggled = hasFocusedValue
			? value != lastFocusedValue
			: (value == 1) != enabled;
		if (userToggled)
		{
			const bool wantEnabled = value == 1;
			const bool accepted = NoteByNoteProbe::SetAutomaticEnabled(wantEnabled);
			{
				std::lock_guard<std::mutex> lock(diagnosticsMutex);
				++diagnostics.toggles;
				diagnostics.lastToggleAccepted = accepted;
			}
			LOG_INFO("(NBN MENU) Rocker moved to " << value << " -> Note by Note "
				<< (wantEnabled ? "ON" : "OFF") << (accepted ? " accepted" : " REJECTED")
				<< "; state is " << (NoteByNoteProbe::IsAutomaticEnabled() ? "ON" : "OFF")
				<< "." << std::endl);
		}
		hasFocusedValue = true;
		lastFocusedValue = value;
		return;
	}
	if (faulted)
	{
		if (!loggedFault)
		{
			loggedFault = true;
			LOG_ERROR("(NBN MENU) AdjustSlider faulted on controller 0x" << std::hex
				<< reinterpret_cast<uintptr_t>(controller) << std::dec
				<< " (menu " << controllerMenu << "); pointer dropped." << std::endl);
		}
		DropController("faulted");
		return;
	}

	// Not focused: repair the row toward the real state. A move that changes nothing
	// (already there, or no such row on this screen) writes nothing.
	hasFocusedValue = false;
	int moved = 0;
	if (AdjustSliderGuarded(controller, enabled ? 1 : -1, moved, faulted, false))
	{
		std::lock_guard<std::mutex> lock(diagnosticsMutex);
		++diagnostics.repairs;
		diagnostics.lastRowValue = moved;
	}
	else if (faulted)
	{
		DropController("faulted");
	}
}

void NoteByNoteMenu::RequestRowProbe()
{
	rowProbeRequested = true;
}

NoteByNoteMenu::Diagnostics NoteByNoteMenu::GetDiagnostics()
{
	std::lock_guard<std::mutex> lock(diagnosticsMutex);
	auto copy = diagnostics;
	copy.controllerCaptured = controller != nullptr;
	return copy;
}
