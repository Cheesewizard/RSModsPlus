#pragma once

#include <cstddef>
#include <cstdint>

#include "NoteByNoteTypes.hpp"

namespace NoteByNoteProtocol
{
	constexpr uint32_t PROBE_API_VERSION = 12;
	// The oldest probe the resident bridge still accepts. Raise this only when the required
	// prefix (through CompleteNoteDrawList) changes shape; appending optional entries does not.
	constexpr uint32_t PROBE_API_MIN_VERSION = 12;

	// OR-ed into the site argument of ObserveNeckPlacementStep when the forward happens at
	// the native step's entry rather than after its return. Only site 2 forwards pre-call;
	// see the ObserveNeckPlacementStep comment.
	constexpr uint32_t NECK_PLACEMENT_STEP_PRE = 0x100;

	// One address the probe wants observed. slotId is the probe's own tag, echoed back to
	// ObserveGenericHook so one callback can serve many hooks. The host installs a
	// pass-through detour on address that always runs the original, so requesting a hook can
	// never change behavior, only observe it.
	struct HookRequest
	{
		uint32_t address = 0;
		uint32_t slotId = 0;
	};

	// The machine state captured at the entry of a generically hooked function, as an overlay
	// on the live stack rather than a copy: the host's per-hook stub does pushad/pushfd and
	// hands the probe a pointer into that frame. registers are the general-purpose file at
	// entry (ecx = thiscall `this` or fastcall arg0, edx = fastcall arg1). returnAddress is the
	// native caller that invoked the hooked function, which usually names the calling function
	// outright. stackArgs is a window over the caller's stack immediately past the return
	// address: for cdecl/stdcall stackArgs[0] is the first argument, for thiscall the first
	// argument past `this`, for fastcall the third argument onward. The overlay is valid only
	// for the duration of the callback; the probe must not retain the pointer.
	struct HookContext
	{
		uint32_t eflags;
		uint32_t edi;
		uint32_t esi;
		uint32_t ebp;
		uint32_t esp;
		uint32_t ebx;
		uint32_t edx;
		uint32_t ecx;
		uint32_t eax;
		uint32_t returnAddress;
		uint32_t stackArgs[16];
	};

	struct ProbeApi
	{
		uint32_t version = PROBE_API_VERSION;
		uint32_t structSize = sizeof(ProbeApi);
		const char* name = nullptr;
		const char* buildId = nullptr;
		uint8_t(__cdecl* Initialize)() = nullptr;
		void(__cdecl* Shutdown)() = nullptr;
		void(__stdcall* ProcessScoringUpdate)(void* owner, float updateTime, ScoringUpdate original) = nullptr;
		bool(__fastcall* ProcessHitDecision)(void* owner, void* unusedEdx, void* note, HitDecision original) = nullptr;
		void(__cdecl* ObserveRenderedAttack)(const RenderedAttack* attack) = nullptr;
		void(__cdecl* Stop)() = nullptr;
		uint8_t(__cdecl* GetState)(NoteByNoteState* state) = nullptr;

		// Render snapshot, moved out of the startup DLL so it iterates without a game restart.
		// The host installs no new hook for this: it forwards the note-head draw it already
		// filters at DrawIndexedPrimitive, plus a per-frame boundary tick, and the probe owns
		// the arm/capture/log. Reload replaces all three, so what is captured and how it is
		// logged changes at probe-build speed. Appended after GetState; the exact-version gate
		// in the bridge guarantees a loaded probe supplies them.
		void(__cdecl* ArmRenderSnapshot)() = nullptr;
		void(__cdecl* ObserveNativeDraw)(const NativeDrawObservation* draw) = nullptr;
		void(__cdecl* NotifyRenderFrameComplete)(uint64_t renderFrame) = nullptr;

		void(__cdecl* ArmNoteDrawListSnapshot)() = nullptr;
		void(__cdecl* ObserveNoteDrawList)(void* renderCtx, void* noteArray) = nullptr;

		void(__cdecl* ArmScreenMapSnapshot)() = nullptr;

		uint8_t(__cdecl* HandleProbeCommand)(
			const char* requestJson,
			char* responseBuffer,
			uint32_t responseCapacity) = nullptr;

		// The fix seam for the fretboard read-ahead markers. When the transport is stopped,
		// the game assigns a pool of preview-marker quads (neck-anchored
		// space: x = time-until-due, y = fret position, z = string height) to upcoming notes
		// at x = 0 - native pause-preview behavior that Note by Note's frozen transport
		// triggers permanently. PrepareNoteDrawList may temporarily move a classified
		// non-target marker off-screen and returns 1 when it did so. The host always runs the
		// original draw, then calls CompleteNoteDrawList before releasing the probe lock.
		uint8_t(__cdecl* PrepareNoteDrawList)(void* renderCtx, void* noteArray) = nullptr;
		void(__cdecl* CompleteNoteDrawList)() = nullptr;

		// ---- Optional from here down: structSize-gated, no version bump to append. ----

		// Generic observation hooks (v12). GetRequestedHooks reports the addresses the probe
		// wants observed; the host installs a pass-through detour on each and calls
		// ObserveGenericHook on every hit with the entry register file. Both null means the
		// probe wants no generic hooks. This is the seam that lets a newly discovered address
		// be observed at probe-rebuild speed with no host change: the address list lives in the
		// probe, so adding one is a probe rebuild and a reload, not a game restart. The pointer
		// returned by GetRequestedHooks must stay valid until the next call or Shutdown.
		void(__cdecl* GetRequestedHooks)(const HookRequest** requests, uint32_t* count) = nullptr;
		void(__cdecl* ObserveGenericHook)(uint32_t slotId, const HookContext* context) = nullptr;

		void(__cdecl* ObserveNeckPlacementStep)(uint32_t site, void* stepContext) = nullptr;

		void(__cdecl* ObserveFullDraw)(const NativeDrawObservation* draw) = nullptr;

		void(__cdecl* RequestReArm)() = nullptr;
	};

	// Everything through CompleteNoteDrawList is required; a probe whose struct is at least this
	// large and supplies those pointers is accepted. Entries after this are optional and probed
	// by structSize, so appending one needs no version bump.
	constexpr size_t REQUIRED_PROBE_API_SIZE = offsetof(ProbeApi, GetRequestedHooks);

	using GetProbeApi = const ProbeApi* (__cdecl*)(uint32_t hostApiVersion, const HostApi* hostApi);
}
