#include "../DLL/Research/ResearchProtocol.hpp"

#include <Windows.h>

#include <cstdint>
#include <iostream>
#include <string>

namespace
{
	uint32_t scoringCallCount = 0;
	uint32_t hitCallCount = 0;
	uint32_t logCount = 0;

	uint8_t __cdecl IsNoteByNoteEnabled()
	{
		return 0;
	}

	void __cdecl HandleControllerFault(const char* reason)
	{
		std::cerr << "Unexpected controller fault: " << (reason == nullptr ? "<null>" : reason) << '\n';
	}

	void __cdecl PublishExpectedAttackEvent(const ResearchProtocol::ExpectedAttackEvent*)
	{
		std::cerr << "Unexpected attack event while the offline controller is disabled.\n";
	}

	void __cdecl Log(ResearchProtocol::LogLevel, const char*)
	{
		++logCount;
	}

	void __stdcall OriginalScoringUpdate(void*, float)
	{
		++scoringCallCount;
	}

	bool __fastcall OriginalHitDecision(void*, void*, void*)
	{
		++hitCallCount;
		return true;
	}

	uint64_t __cdecl GetMlAudioSampleIndex()
	{
		return 0;
	}

	uint8_t __cdecl QueryMlNoteEvidence(int, float, uint64_t,
		ResearchProtocol::MlNoteEvidence* evidence)
	{
		*evidence = {};
		return 0;
	}
	ResearchProtocol::HostApi CreateHostApi()
	{
		ResearchProtocol::HostApi api;
		api.version = ResearchProtocol::HOST_API_VERSION;
		api.structSize = sizeof(ResearchProtocol::HostApi);
		api.IsNoteByNoteEnabled = &IsNoteByNoteEnabled;
		api.HandleControllerFault = &HandleControllerFault;
		api.PublishExpectedAttackEvent = &PublishExpectedAttackEvent;
		api.Log = &Log;
		api.GetMlAudioSampleIndex = &GetMlAudioSampleIndex;
		api.QueryMlNoteEvidence = &QueryMlNoteEvidence;
		return api;
	}

	const ResearchProtocol::HostApi hostApi = CreateHostApi();

	bool RunLoadCycle(const std::wstring& path)
	{
		const auto module = LoadLibraryW(path.c_str());
		if (module == nullptr)
		{
			std::cerr << "LoadLibrary failed with error " << GetLastError() << ".\n";
			return false;
		}

		const auto getApi = reinterpret_cast<ResearchProtocol::GetProbeApi>(
			GetProcAddress(module, "RSMP_GetResearchProbeApi"));
		if (getApi == nullptr)
		{
			std::cerr << "RSMP_GetResearchProbeApi is missing.\n";
			FreeLibrary(module);
			return false;
		}

		const auto api = getApi(ResearchProtocol::HOST_API_VERSION, &hostApi);
		if (api == nullptr
			|| api->version != ResearchProtocol::PROBE_API_VERSION
			|| api->structSize < sizeof(ResearchProtocol::ProbeApi)
			|| api->Initialize == nullptr
			|| api->Shutdown == nullptr)
		{
			std::cerr << "The probe API is invalid.\n";
			FreeLibrary(module);
			return false;
		}

		if (api->Initialize() == 0)
		{
			std::cerr << "The probe rejected initialization.\n";
			api->Shutdown();
			FreeLibrary(module);
			return false;
		}

		ResearchProtocol::NoteByNoteState state;
		if (api->GetState(&state) == 0 || state.isInitialized == 0 || state.ownsNativeHold != 0)
		{
			std::cerr << "The initial controller state is invalid.\n";
			api->Shutdown();
			FreeLibrary(module);
			return false;
		}

		api->ProcessScoringUpdate(nullptr, 1.0f, &OriginalScoringUpdate);
		const auto hitResult = api->ProcessHitDecision(nullptr, nullptr, nullptr, &OriginalHitDecision);
		api->Stop();
		api->Shutdown();
		FreeLibrary(module);

		if (scoringCallCount == 0 || hitCallCount == 0 || !hitResult || logCount == 0)
		{
			std::cerr << "A disabled controller did not preserve the original native calls.\n";
			return false;
		}
		return true;
	}
}

int wmain(int argumentCount, wchar_t** arguments)
{
	if (argumentCount != 2)
	{
		std::cerr << "Usage: research-bridge-harness.exe <NoteByNoteProbe.dll>\n";
		return 2;
	}

	if (!RunLoadCycle(arguments[1])) return 1;
	if (!RunLoadCycle(arguments[1])) return 1;

	std::cout << "PASS: probe ABI, initialization, original-call forwarding, shutdown, unload, and reload.\n";
	return 0;
}
