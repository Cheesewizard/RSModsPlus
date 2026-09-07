#include "../stdafx.h"
#include "NoteByNoteController.hpp"
#include "NoteByNoteHostServices.hpp"
#include "NoteByNoteNativeScoring.hpp"
#include "NoteByNoteProbe.hpp"

extern "C" const NoteByNoteProtocol::ProbeApi* __cdecl GetNoteByNoteControllerApi(
	uint32_t version, const NoteByNoteProtocol::HostApi* host);

namespace
{
	std::recursive_mutex probeMutex;
	struct Controller
	{
		bool isInitialized = false;
		NoteByNoteProtocol::ProbeApi api = {};
	} loadedProbe;
}

void NoteByNoteController::Initialize()
{
	const auto* api = GetNoteByNoteControllerApi(NoteByNoteProtocol::HOST_API_VERSION, &NoteByNoteHostServices::GetHostApi());
	if (api == nullptr || api->version != NoteByNoteProtocol::PROBE_API_VERSION || api->structSize != sizeof(*api) || !api->Initialize())
	{
		LOG_ERROR("(NBN CONTROLLER) Built-in controller initialization failed." << std::endl);
		return;
	}
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	loadedProbe.api = *api;
	loadedProbe.isInitialized = true;
	const NoteByNoteProtocol::HookRequest* requests = nullptr;
	uint32_t count = 0;
	api->GetRequestedHooks(&requests, &count);
	NoteByNoteNativeScoring::SetRequestedGenericHooks(requests, count);
	LOG_INFO("(NBN CONTROLLER) Built-in gameplay controller ready. Public build; developer endpoints excluded." << std::endl);
}

void NoteByNoteController::Shutdown()
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.isInitialized) loadedProbe.api.Shutdown();
	loadedProbe = {};
}
bool NoteByNoteController::IsProbeLoaded()
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	return loadedProbe.isInitialized;
}

void NoteByNoteController::DispatchGenericHook(
	uint32_t slotId,
	const NoteByNoteProtocol::HookContext* context)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (!loadedProbe.isInitialized || loadedProbe.api.ObserveGenericHook == nullptr) return;
	loadedProbe.api.ObserveGenericHook(slotId, context);
}

void NoteByNoteController::DispatchNeckPlacementStep(uint32_t site, void* stepContext)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (!loadedProbe.isInitialized
		|| loadedProbe.api.ObserveNeckPlacementStep == nullptr)
	{
		return;
	}
	loadedProbe.api.ObserveNeckPlacementStep(site, stepContext);
}

void NoteByNoteController::DispatchScoringUpdate(
	void* owner,
	float updateTime,
	NoteByNoteProtocol::ScoringUpdate original)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (!loadedProbe.isInitialized)
	{
		original(owner, updateTime);
		return;
	}
	loadedProbe.api.ProcessScoringUpdate(owner, updateTime, original);
}

void NoteByNoteController::DispatchHitDecision(
	void* owner,
	void* unusedEdx,
	void* note,
	NoteByNoteProtocol::HitDecision original,
	bool& result)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	result = !loadedProbe.isInitialized
		? original(owner, unusedEdx, note)
		: loadedProbe.api.ProcessHitDecision(owner, unusedEdx, note, original);
}

void NoteByNoteController::DispatchRenderedAttack(
	const NoteByNoteProbe::NativeRenderedAttack& attack)
{
	std::vector<NoteByNoteProtocol::RenderedNote> notes;
	notes.reserve(attack.notes.size());
	for (const auto& source : attack.notes)
	{
		notes.push_back({ source.stringIndex, source.fret });
	}

	const NoteByNoteProtocol::RenderedAttack researchAttack =
	{
		attack.isTransition ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0),
		attack.renderFrame,
		attack.songTime,
		attack.longitudinalPosition,
		notes.data(),
		notes.size()
	};

	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.isInitialized) loadedProbe.api.ObserveRenderedAttack(&researchAttack);
}

void NoteByNoteController::DispatchStop()
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.isInitialized) loadedProbe.api.Stop();
}

void NoteByNoteController::DispatchRequestReArm()
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	// Optional ProbeApi entry: a probe built before the seam leaves it null, so the
	// re-arm is simply skipped there rather than faulting.
	if (loadedProbe.isInitialized && loadedProbe.api.RequestReArm != nullptr)
	{
		loadedProbe.api.RequestReArm();
	}
}

void NoteByNoteController::ArmRenderSnapshot()
{
}

void NoteByNoteController::DispatchNativeDraw(const NoteByNoteProtocol::NativeDrawObservation& draw)
{
}

void NoteByNoteController::DispatchFullDraw(const NoteByNoteProtocol::NativeDrawObservation& draw)
{
}

bool NoteByNoteController::IsFullDrawFeedEnabled()
{
	return false;
}

bool NoteByNoteController::IsNativeDrawFeedEnabled()
{
	return false;
}

bool NoteByNoteController::IsDrawFeedWanted()
{
	return false;
}

void NoteByNoteController::DispatchRenderFrameComplete(uint64_t renderFrame)
{
}

void NoteByNoteController::ArmNoteDrawListSnapshot()
{
}

void NoteByNoteController::ArmScreenMapSnapshot()
{
}

bool NoteByNoteController::TryHandleProbeCommand(const std::string& requestJson, std::string& response)
{
	return false;
}

void NoteByNoteController::ProcessNoteDrawList(
	void* renderCtx,
	void* unusedEdx,
	int* noteArray,
	NoteByNoteProtocol::NoteHeadDraw original)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (!loadedProbe.isInitialized)
	{
		original(renderCtx, unusedEdx, noteArray);
		return;
	}

	const bool wasPrepared = loadedProbe.api.PrepareNoteDrawList(renderCtx, noteArray) != 0;
	original(renderCtx, unusedEdx, noteArray);
	if (wasPrepared) loadedProbe.api.CompleteNoteDrawList();
}

bool NoteByNoteController::TryGetNoteByNoteState(NoteByNoteProtocol::NoteByNoteState& state)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (!loadedProbe.isInitialized) return false;
	state = {};
	return loadedProbe.api.GetState(&state) != 0;
}


void NoteByNoteController::PublishPhysicalMarkerDraw(const PhysicalMarkerDrawEvent&) {}
void NoteByNoteController::PublishNoteByNoteEvent(const NoteByNoteProbe::NativeExpectedAttackEvent&) {}
