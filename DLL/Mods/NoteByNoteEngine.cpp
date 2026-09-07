#include "NoteByNoteRuntime.hpp"
#include "NoteByNoteProtocol.hpp"
#include "NoteByNoteScoringCore.hpp"
#include "NoteByNoteNativeScoring.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
namespace {
constexpr char PROBE_NAME[] = "RSModsPlus Note by Note Controller";
const std::string PROBE_BUILD_ID = std::string(__DATE__) + " " + __TIME__;
namespace NativeNoteList {
bool LooksReadable(uintptr_t p)
		{
			return p >= 0x10000 && p < 0x7FFF0000;
		}
float ReadFloat(uintptr_t base, unsigned int offset)
		{
			return *reinterpret_cast<const float*>(base + offset);
		}
int ReadInt(uintptr_t base, unsigned int offset)
		{
			return *reinterpret_cast<const int*>(base + offset);
		}
}
namespace StoppedPreviewFilter
	{
		constexpr uintptr_t PREVIEW_CONTEXT_VTABLE = 0x0121B6F0;
		constexpr float MARKER_OFFSET = 0.5f;
		constexpr float OFFSCREEN_STRING_POSITION = -999.5f;
		constexpr float MARKER_SCALE_MIN = 0.70f;
		constexpr float MARKER_SCALE_MAX = 1.05f;
		constexpr float MARKER_W_MIN = 0.80f;
		constexpr float MARKER_W_MAX = 0.95f;
		constexpr float FRET_EPSILON = 0.35f;
		constexpr float STRING_EPSILON = 0.20f;
		constexpr std::array<float, 24> FRET_CENTERS =
		{
			-50.469f, -44.429f, -38.739f, -33.163f, -27.755f, -22.552f,
			-17.478f, -12.512f, -7.723f, -3.061f, 1.496f, 5.932f,
			10.242f, 14.448f, 18.593f, 22.627f, 26.565f, 30.107f,
			33.896f, 37.366f, 40.977f, 44.478f, 47.915f, 51.351f,
		};
		constexpr std::array<float, 6> STRING_HEIGHTS =
		{
			4.018f, 2.410f, 0.803f, -0.803f, -2.410f, -4.018f,
		};
		std::atomic<uint32_t> hiddenCount{ 0 };
		constexpr size_t PENDING_CAPACITY = 16;
		thread_local uintptr_t pendingNotes[PENDING_CAPACITY] = {};
		thread_local float pendingStringPositions[PENDING_CAPACITY] = {};
		thread_local size_t pendingCount = 0;
		bool TryWriteFloat(uintptr_t address, float value)
		{
			__try
			{
				*reinterpret_cast<float*>(address) = value;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
		bool TryReadFloat(uintptr_t address, float* value)
		{
			__try
			{
				*value = *reinterpret_cast<const float*>(address);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
		bool TryReadDword(uintptr_t address, uint32_t* value)
		{
			__try
			{
				*value = *reinterpret_cast<const uint32_t*>(address);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
		std::atomic<bool> isPersistentRelocationEnabled{ false };
		constexpr size_t PERSISTENT_CAPACITY = 128;
		struct PersistentEntry
		{
			uintptr_t note;
			float originalPosition;
			uint32_t headerWord;
			uint32_t fretWord;
		};
		std::mutex persistentMutex;
		PersistentEntry persistentEntries[PERSISTENT_CAPACITY] = {};
		size_t persistentCount = 0;
		int persistentTargetString = -1;
		int persistentTargetFret = -1;
		std::atomic<uint32_t> persistentHiddenCount{ 0 };
		std::atomic<bool> wasCapacityReported{ false };
		constexpr int PER_HOLD_LOG_BUDGET = 6;
		int perHoldLogBudget = PER_HOLD_LOG_BUDGET;
		uint32_t perHoldRelocations = 0;
		uint32_t perHoldRewrites = 0;
		void RestorePersistent(const char* reason)
		{
			std::lock_guard<std::mutex> lock(persistentMutex);
			persistentTargetString = -1;
			persistentTargetFret = -1;
			const auto holdRelocations = perHoldRelocations;
			const auto holdRewrites = perHoldRewrites;
			perHoldLogBudget = PER_HOLD_LOG_BUDGET;
			perHoldRelocations = 0;
			perHoldRewrites = 0;
			if (persistentCount == 0)
			{
				LOG_INFO("(NBN STOPPED PREVIEW) restore pass with nothing tracked, "
					<< "reason=" << reason << "." << std::endl);
				return;
			}
			size_t restored = 0;
			size_t recycled = 0;
			for (size_t index = 0; index < persistentCount; ++index)
			{
				const auto& entry = persistentEntries[index];
				float current = 0.0f;
				uint32_t headerWord = 0;
				uint32_t fretWord = 0;
				if (!TryReadFloat(entry.note + 0x34, &current)
					|| std::fabs(current - OFFSCREEN_STRING_POSITION) > 0.01f
					|| !TryReadDword(entry.note + 0x00, &headerWord)
					|| headerWord != entry.headerWord
					|| !TryReadDword(entry.note + 0x30, &fretWord)
					|| fretWord != entry.fretWord)
				{
					++recycled;
					continue;
				}
				if (TryWriteFloat(entry.note + 0x34, entry.originalPosition))
				{
					++restored;
				}
			}
			LOG_INFO("(NBN STOPPED PREVIEW) restored " << restored
				<< " persistent marker(s), " << recycled << " recycled, holdRelocations="
				<< holdRelocations << " holdRewrites=" << holdRewrites << ", reason="
				<< reason << "." << std::endl);
			persistentCount = 0;
		}
		void HidePersistently(
			uintptr_t note,
			float originalPosition,
			const NoteByNoteProtocol::NoteByNoteState& state,
			int stringIndex,
			int fret)
		{
			std::lock_guard<std::mutex> lock(persistentMutex);
			for (size_t index = 0; index < persistentCount; ++index)
			{
				if (persistentEntries[index].note != note) continue;
				if (TryWriteFloat(note + 0x34, OFFSCREEN_STRING_POSITION))
				{
					persistentEntries[index].originalPosition = originalPosition;
					TryReadDword(note + 0x00, &persistentEntries[index].headerWord);
					TryReadDword(note + 0x30, &persistentEntries[index].fretWord);
					++perHoldRewrites;
					if (perHoldLogBudget > 0)
					{
						--perHoldLogBudget;
						LOG_INFO("(NBN STOPPED PREVIEW) re-relocated after game rewrite "
							<< stringIndex << ':' << fret
							<< " note=0x" << std::hex << note << std::dec
							<< " holdRewrites=" << perHoldRewrites << std::endl);
					}
				}
				return;
			}
			if (persistentCount >= PERSISTENT_CAPACITY)
			{
				if (!wasCapacityReported.exchange(true, std::memory_order_acq_rel))
				{
					LOG_ERROR("(NBN STOPPED PREVIEW) persistent list full at "
						<< PERSISTENT_CAPACITY
						<< " records; further markers stay visible." << std::endl);
				}
				return;
			}
			uint32_t headerWord = 0;
			uint32_t fretWord = 0;
			if (!TryReadDword(note + 0x00, &headerWord)
				|| !TryReadDword(note + 0x30, &fretWord))
			{
				return;
			}
			if (!TryWriteFloat(note + 0x34, OFFSCREEN_STRING_POSITION))
			{
				LOG_ERROR("(NBN STOPPED PREVIEW) Could not relocate marker record 0x"
					<< std::hex << note << std::dec << "." << std::endl);
				return;
			}
			persistentEntries[persistentCount].note = note;
			persistentEntries[persistentCount].originalPosition = originalPosition;
			persistentEntries[persistentCount].headerWord = headerWord;
			persistentEntries[persistentCount].fretWord = fretWord;
			++persistentCount;
			persistentTargetString = state.visualString;
			persistentTargetFret = state.visualFret;
			++perHoldRelocations;
			const auto total = persistentHiddenCount.fetch_add(1, std::memory_order_acq_rel) + 1;
			const bool hasHoldBudget = perHoldLogBudget > 0;
			if (hasHoldBudget) --perHoldLogBudget;
			if (hasHoldBudget || total <= 18 || total % 500 == 0)
			{
				LOG_INFO("(NBN STOPPED PREVIEW) relocated persistently "
					<< stringIndex << ':' << fret
					<< " target=" << state.visualString << ':' << state.visualFret
					<< " note=0x" << std::hex << note << std::dec
					<< " tracked=" << persistentCount
					<< " holdRelocations=" << perHoldRelocations
					<< " count=" << total << std::endl);
			}
		}
		void RestorePending()
		{
			for (size_t index = 0; index < pendingCount; ++index)
			{
				if (!TryWriteFloat(pendingNotes[index] + 0x34, pendingStringPositions[index]))
				{
					LOG_ERROR("(NBN STOPPED PREVIEW) Could not restore marker record 0x"
						<< std::hex << pendingNotes[index] << std::dec << "." << std::endl);
				}
				pendingNotes[index] = 0;
			}
			pendingCount = 0;
		}
		int DecodeClosest(float value, const float* values, size_t count, float epsilon)
		{
			int closest = -1;
			float closestDistance = epsilon;
			for (size_t index = 0; index < count; ++index)
			{
				const float distance = std::fabs(value - values[index]);
				if (distance >= closestDistance) continue;
				closest = static_cast<int>(index);
				closestDistance = distance;
			}
			return closest;
		}
		bool IsSelectedCoordinate(
			const NoteByNoteProtocol::NoteByNoteState& state,
			int stringIndex,
			int fret)
		{
			if (stringIndex == state.visualString && fret == state.visualFret) return true;
			for (uint32_t index = 0; index < state.visualGroupCount
				&& index < NoteByNoteProtocol::NoteByNoteState::MaxVisualGroup; ++index)
			{
				if (stringIndex == state.visualGroupStrings[index]
					&& fret == state.visualGroupFrets[index])
				{
					return true;
				}
			}
			return false;
		}
		bool HideNonTarget(void* renderCtx, void* noteArray)
		{
			RestorePending();
			const auto state = NoteByNoteNativeScoring::GetStateSnapshot();
			const bool isPersistent =
				isPersistentRelocationEnabled.load(std::memory_order_relaxed);
			const bool isHoldActive = state.ownsNativeHold != 0 && state.visualChordId < 0;
			const char* restoreReason = nullptr;
			{
				std::lock_guard<std::mutex> lock(persistentMutex);
				if (persistentCount > 0)
				{
					if (!isPersistent) restoreReason = "persist-disabled";
					else if (!isHoldActive) restoreReason = "hold-ended";
					else if (persistentTargetString != state.visualString
						|| persistentTargetFret != state.visualFret)
					{
						restoreReason = "target-changed";
					}
				}
			}
			if (restoreReason != nullptr) RestorePersistent(restoreReason);
			if (!isHoldActive) return false;
			if (!NativeNoteList::LooksReadable(reinterpret_cast<uintptr_t>(renderCtx))
				|| *reinterpret_cast<const uintptr_t*>(renderCtx) != PREVIEW_CONTEXT_VTABLE)
			{
				return false;
			}
			const auto arrayAddress = reinterpret_cast<uintptr_t>(noteArray);
			if (!NativeNoteList::LooksReadable(arrayAddress)) return false;
			const auto notes = static_cast<uintptr_t>(
				static_cast<unsigned int>(NativeNoteList::ReadInt(arrayAddress, 0)));
			const int count = NativeNoteList::ReadInt(arrayAddress, 4);
			if (!NativeNoteList::LooksReadable(notes) || count < 1 || count > 64) return false;
			bool didHide = false;
			for (int index = 0; index < count && pendingCount < PENDING_CAPACITY; ++index)
			{
				const auto note = static_cast<uintptr_t>(static_cast<unsigned int>(
					NativeNoteList::ReadInt(notes + static_cast<uintptr_t>(index) * 4, 0)));
				if (!NativeNoteList::LooksReadable(note)) continue;
				const int fretIndex = DecodeClosest(
					NativeNoteList::ReadFloat(note, 0x30) - MARKER_OFFSET,
					FRET_CENTERS.data(),
					FRET_CENTERS.size(),
					FRET_EPSILON);
				const int stringIndex = DecodeClosest(
					NativeNoteList::ReadFloat(note, 0x34) - MARKER_OFFSET,
					STRING_HEIGHTS.data(),
					STRING_HEIGHTS.size(),
					STRING_EPSILON);
				if (fretIndex < 0 || stringIndex < 0) continue;
				const int fret = fretIndex + 1;
				if (IsSelectedCoordinate(state, stringIndex, fret)) continue;
				const float originalPosition = NativeNoteList::ReadFloat(note, 0x34);
				if (isPersistent)
				{
					HidePersistently(note, originalPosition, state, stringIndex, fret);
					continue;
				}
				if (!TryWriteFloat(note + 0x34, OFFSCREEN_STRING_POSITION))
				{
					LOG_ERROR("(NBN STOPPED PREVIEW) Could not hide marker record 0x"
						<< std::hex << note << std::dec << "." << std::endl);
					continue;
				}
				pendingNotes[pendingCount] = note;
				pendingStringPositions[pendingCount] = originalPosition;
				++pendingCount;
				didHide = true;
				const auto total = hiddenCount.fetch_add(1, std::memory_order_acq_rel) + 1;
				if (total <= 18 || total % 500 == 0)
				{
					LOG_INFO("(NBN STOPPED PREVIEW) hidden transiently " << stringIndex << ':' << fret
						<< " target=" << state.visualString << ':' << state.visualFret
						<< " note=0x" << std::hex << note << std::dec
						<< " count=" << total << std::endl);
				}
			}
			return didHide;
		}
	}
uint8_t __cdecl InitializeProbe()
	{
		NoteByNoteScoringCore::Initialize();
		return NoteByNoteScoringCore::IsAvailable() ? 1 : 0;
	}
void __cdecl ShutdownProbe()
	{
		StoppedPreviewFilter::RestorePersistent("probe-shutdown");
		NoteByNoteNativeScoring::Shutdown();
		NoteByNoteRuntime::Shutdown();
	}
void __stdcall ProcessScoringUpdate(
		void* owner,
		float updateTime,
		NoteByNoteProtocol::ScoringUpdate original)
	{
		NoteByNoteNativeScoring::ProcessScoringUpdate(owner, updateTime, original);
	}
bool __fastcall ProcessHitDecision(
		void* owner,
		void* unusedEdx,
		void* note,
		NoteByNoteProtocol::HitDecision original)
	{
		return NoteByNoteNativeScoring::ProcessHitDecision(owner, unusedEdx, note, original);
	}
void __cdecl ObserveRenderedAttack(const NoteByNoteProtocol::RenderedAttack* source)
	{
		if (source == nullptr) return;
		NoteByNoteProbe::NativeRenderedAttack attack;
		attack.isTransition = source->isTransition != 0;
		attack.renderFrame = source->renderFrame;
		attack.songTime = source->songTime;
		attack.longitudinalPosition = source->longitudinalPosition;
		attack.notes.reserve(source->noteCount);
		for (size_t index = 0; index < source->noteCount; ++index)
		{
			attack.notes.push_back({ source->notes[index].stringIndex, source->notes[index].fret });
		}
		NoteByNoteScoringCore::ObserveRenderedAttack(attack);
	}
void __cdecl StopProbe()
	{
		StoppedPreviewFilter::RestorePersistent("probe-stop");
		NoteByNoteScoringCore::Stop();
	}
void __cdecl RequestReArmProbe()
	{
		NoteByNoteScoringCore::RequestReArm();
	}
uint8_t __cdecl GetState(NoteByNoteProtocol::NoteByNoteState* state)
	{
		if (state == nullptr || state->structSize < sizeof(NoteByNoteProtocol::NoteByNoteState)) return 0;
		*state = NoteByNoteNativeScoring::GetStateSnapshot();
		return 1;
	}
uint8_t __cdecl PrepareNoteDrawList(void* renderCtx, void* noteArray)
	{
		return StoppedPreviewFilter::HideNonTarget(renderCtx, noteArray) ? 1 : 0;
	}
void __cdecl CompleteNoteDrawList()
	{
		StoppedPreviewFilter::RestorePending();
	}
void __cdecl GetRequestedHooks(const NoteByNoteProtocol::HookRequest** requests, uint32_t* count) { if (requests) *requests = nullptr; if (count) *count = 0; }
	const NoteByNoteProtocol::ProbeApi probeApi =
	{
		NoteByNoteProtocol::PROBE_API_VERSION,
		sizeof(NoteByNoteProtocol::ProbeApi),
		PROBE_NAME,
		PROBE_BUILD_ID.c_str(),
		&InitializeProbe,
		&ShutdownProbe,
		&ProcessScoringUpdate,
		&ProcessHitDecision,
		&ObserveRenderedAttack,
		&StopProbe,
		&GetState,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		&PrepareNoteDrawList,
		&CompleteNoteDrawList,
		&GetRequestedHooks,
		nullptr,
		nullptr,
		nullptr,
		&RequestReArmProbe
	};
}
#define RSMP_PROBE_EXPORT
extern "C" RSMP_PROBE_EXPORT const NoteByNoteProtocol::ProbeApi* __cdecl GetNoteByNoteControllerApi(
	uint32_t hostApiVersion,
	const NoteByNoteProtocol::HostApi* hostApi)
{
	if (hostApiVersion != NoteByNoteProtocol::HOST_API_VERSION) return nullptr;
	if (!NoteByNoteRuntime::Initialize(hostApi)) return nullptr;
	return &probeApi;
}
