#pragma once

#include "NoteByNoteProtocol.hpp"

#include <string>

namespace NoteByNoteProbe
{
	struct NativeRenderedAttack;
	struct NativeExpectedAttackEvent;
}

namespace NoteByNoteController
{
	struct PhysicalMarkerDrawEvent
	{
		uint64_t renderFrame = 0;
		uintptr_t nativeCaller = 0;
		uintptr_t streamIdentity = 0;
		uintptr_t textureIdentity = 0;
		uintptr_t vertexShaderIdentity = 0;
		uintptr_t pixelShaderIdentity = 0;
		uint32_t primitiveType = 0;
		int32_t baseVertexIndex = 0;
		uint32_t minimumVertexIndex = 0;
		uint32_t vertexCount = 0;
		uint32_t startIndex = 0;
		uint32_t primitiveCount = 0;
		uint32_t stride = 0;
		int32_t stringIndex = -1;
		int32_t fret = -1;
		int32_t targetString = -1;
		int32_t targetFret = -1;
		bool decoded = false;
		const char* decodeFailure = nullptr;
		bool instanced = false;
		uint32_t streamFrequency = 0;
		uint32_t transformStreamStride = 0;
		uint32_t instanceCount = 0;
		uint32_t decodedInstanceCount = 0;
		float instanceTranslations[32][3] = {};
		bool userPointer = false;
		uint32_t vertexSampleCount = 0;
		int32_t vertexSampleStatus = 1;
		float vertexSample[24] = {};
		bool hasBounds = false;
		float boundsMin[3] = {};
		float boundsMax[3] = {};
		float transform[4][4] = {};
	};

	void ArmRenderSnapshot();
	void ArmNoteDrawListSnapshot();
	void ArmScreenMapSnapshot();
	bool TryHandleProbeCommand(const std::string& requestJson, std::string& response);
	void ProcessNoteDrawList(
		void* renderCtx,
		void* unusedEdx,
		int* noteArray,
		NoteByNoteProtocol::NoteHeadDraw original);
	void DispatchHitDecision(
		void* owner,
		void* unusedEdx,
		void* note,
		NoteByNoteProtocol::HitDecision original,
		bool& result);
	void DispatchGenericHook(uint32_t slotId, const NoteByNoteProtocol::HookContext* context);
	void DispatchNeckPlacementStep(uint32_t site, void* stepContext);
	void DispatchNativeDraw(const NoteByNoteProtocol::NativeDrawObservation& draw);
	// Full draw feed: every in-song draw from all four device entry points, gated by
	// IsFullDrawFeedEnabled host-side and by the probe supplying ObserveFullDraw. The
	// disarmed cost is one relaxed atomic read at the hook site.
	void DispatchFullDraw(const NoteByNoteProtocol::NativeDrawObservation& draw);
	bool IsFullDrawFeedEnabled();
	// Native (filtered note-head) draw feed: same shape as the full feed. Off by default in
	// Release, on in Debug; armed automatically by the render-snapshot and screen-map
	// captures that consume it, and by set_native_draw_feed.
	bool IsNativeDrawFeedEnabled();
	// True when EITHER feed has an enabled flag AND a loaded consumer. The draw hook builds
	// the per-draw observation struct only when this is true, so the idle Release path
	// pays two relaxed atomic reads per draw and nothing else.
	bool IsDrawFeedWanted();
	void DispatchRenderFrameComplete(uint64_t renderFrame);
	void DispatchRenderedAttack(const NoteByNoteProbe::NativeRenderedAttack& attack);
	void DispatchScoringUpdate(
		void* owner,
		float updateTime,
		NoteByNoteProtocol::ScoringUpdate original);
	void DispatchStop();
	// Forward a bootstrap re-arm request to the loaded probe (optional ProbeApi entry;
	// a no-op when the probe predates it). Called on every NBN enable so a same-owner
	// section change still re-arms.
	void DispatchRequestReArm();
	void Initialize();
	bool IsProbeLoaded();
	void PublishPhysicalMarkerDraw(const PhysicalMarkerDrawEvent& event);
	void PublishNoteByNoteEvent(const NoteByNoteProbe::NativeExpectedAttackEvent& event);
	void Shutdown();
	bool TryGetNoteByNoteState(NoteByNoteProtocol::NoteByNoteState& state);

}
