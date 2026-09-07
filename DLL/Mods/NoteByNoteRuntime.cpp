#include "NoteByNoteRuntime.hpp"

#include <mutex>

namespace
{
	std::mutex hostApiMutex;
	const NoteByNoteProtocol::HostApi* activeHostApi = nullptr;

	const NoteByNoteProtocol::HostApi* GetHostApi()
	{
		std::lock_guard<std::mutex> lock(hostApiMutex);
		return activeHostApi;
	}
}

bool NoteByNoteRuntime::Initialize(const NoteByNoteProtocol::HostApi* hostApi)
{
	if (hostApi == nullptr
		|| hostApi->version != NoteByNoteProtocol::HOST_API_VERSION
		|| hostApi->structSize < sizeof(NoteByNoteProtocol::HostApi)
		|| hostApi->IsNoteByNoteEnabled == nullptr
		|| hostApi->HandleControllerFault == nullptr
		|| hostApi->PublishExpectedAttackEvent == nullptr
		|| hostApi->QueryMlNoteEvidence == nullptr
		|| hostApi->GetMlAudioSampleIndex == nullptr
		|| hostApi->Log == nullptr
		|| hostApi->QueryRawToneComb == nullptr
		|| hostApi->QueryRawNoteConfirmation == nullptr
		|| hostApi->QueryRawAttacks == nullptr)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(hostApiMutex);
	activeHostApi = hostApi;
	return true;
}

void NoteByNoteRuntime::Shutdown()
{
	std::lock_guard<std::mutex> lock(hostApiMutex);
	activeHostApi = nullptr;
}

bool NoteByNoteRuntime::IsNoteByNoteEnabled()
{
	const auto hostApi = GetHostApi();
	return hostApi != nullptr && hostApi->IsNoteByNoteEnabled() != 0;
}

void NoteByNoteRuntime::HandleControllerFault(const std::string& reason)
{
	const auto hostApi = GetHostApi();
	if (hostApi != nullptr) hostApi->HandleControllerFault(reason.c_str());
}

void NoteByNoteRuntime::PublishExpectedAttackEvent(
	const NoteByNoteProtocol::ExpectedAttackEvent& event)
{
	const auto hostApi = GetHostApi();
	if (hostApi != nullptr) hostApi->PublishExpectedAttackEvent(&event);
}

void NoteByNoteRuntime::Log(NoteByNoteProtocol::LogLevel level, const std::string& message)
{
	const auto hostApi = GetHostApi();
	if (hostApi != nullptr) hostApi->Log(level, message.c_str());
}

int NoteByNoteRuntime::GetInputOnsetShiftSemitones()
{
	const auto hostApi = GetHostApi();
	// Initialize already required structSize >= sizeof(HostApi), so the entry is present when
	// a host is active; guard the pointer for an older host that left it null.
	if (hostApi == nullptr || hostApi->GetInputOnsetShiftSemitones == nullptr) return 0;
	return hostApi->GetInputOnsetShiftSemitones();
}

bool NoteByNoteRuntime::QueryRawNoteConfirmation(double frequencyHz, NoteByNoteProtocol::RawNoteConfirmation& out, uint64_t minimumSampleIndex, uint64_t maximumSampleIndex)
{
	out = {};
	const auto hostApi = GetHostApi();
	return hostApi != nullptr && hostApi->QueryRawNoteConfirmation(frequencyHz, minimumSampleIndex, maximumSampleIndex, &out) != 0;
}

bool NoteByNoteRuntime::QueryRawToneComb(double frequencyHz, NoteByNoteProtocol::RawToneComb& out)
{
	out = {};
	const auto hostApi = GetHostApi();
	if (hostApi == nullptr) return false;
	return hostApi->QueryRawToneComb(frequencyHz, &out) != 0;
}

bool NoteByNoteRuntime::QueryRawToneEvidence(double frequencyHz, float windowSeconds,
	NoteByNoteProtocol::RawToneEvidence& out)
{
	const auto hostApi = GetHostApi();
	if (hostApi == nullptr || hostApi->QueryRawToneEvidence == nullptr) return false;
	out.structSize = sizeof(NoteByNoteProtocol::RawToneEvidence);
	return hostApi->QueryRawToneEvidence(frequencyHz, windowSeconds, &out) != 0;
}

bool NoteByNoteRuntime::QueryMlPitch(float& outMidi, float& outConfidence,
	double& outAgeSeconds)
{
	const auto hostApi = GetHostApi();
	// Initialize's structSize gate guarantees the appended entries exist on any accepted
	// host; the null guard covers a same-size host that left them unwired.
	if (hostApi == nullptr || hostApi->QueryMlPitch == nullptr) return false;
	return hostApi->QueryMlPitch(&outMidi, &outConfidence, &outAgeSeconds) != 0;
}

bool NoteByNoteRuntime::IsMlPitchServiceAlive()
{
	const auto hostApi = GetHostApi();
	if (hostApi == nullptr || hostApi->IsMlPitchServiceAlive == nullptr) return false;
	return hostApi->IsMlPitchServiceAlive() != 0;
}

uint64_t NoteByNoteRuntime::GetMlAudioSampleIndex()
{
	const auto hostApi = GetHostApi();
	return hostApi == nullptr ? 0 : hostApi->GetMlAudioSampleIndex();
}

bool NoteByNoteRuntime::QueryMlNoteEvidence(int expectedMidi, float minConfidence,
	uint64_t minimumSampleIndex, NoteByNoteProtocol::MlNoteEvidence& evidence)
{
	const auto hostApi = GetHostApi();
	evidence = {};
	if (hostApi == nullptr) return false;
	return hostApi->QueryMlNoteEvidence(expectedMidi, minConfidence,
		minimumSampleIndex, &evidence) != 0;
}

bool NoteByNoteRuntime::QueryRawAttacks(uint64_t afterSampleIndex, RawPitchVerifier::RawAttackBatch& out)
{
	out = {};
	const auto hostApi = GetHostApi();
	return hostApi != nullptr && hostApi->QueryRawAttacks(afterSampleIndex, &out) != 0;
}
