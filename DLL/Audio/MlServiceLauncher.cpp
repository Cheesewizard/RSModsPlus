#include "stdafx.h"
#include "MlServiceLauncher.hpp"
#include "../Log.hpp"

#include <windows.h>
#include <filesystem>
#include <string>

namespace
{
	bool attempted = false;
	HANDLE serviceJob = nullptr;
	HANDLE serviceProcess = nullptr;
}

void MlServiceLauncher::EnsureStarted()
{
	if (serviceProcess != nullptr && WaitForSingleObject(serviceProcess, 0) == WAIT_OBJECT_0)
	{
		DWORD exitCode = 0;
		GetExitCodeProcess(serviceProcess, &exitCode);
		LOG_ERROR("[MlServiceLauncher] Bundled ML service exited, code=" << exitCode
			<< ". See %LOCALAPPDATA%\\RSModsPlus\\Logs\\ml-service.log and startup-error.log." << std::endl);
		CloseHandle(serviceProcess);
		serviceProcess = nullptr;
	}
	if (attempted) return;
	attempted = true;

	wchar_t gamePath[32768] = {};
	const DWORD length = GetModuleFileNameW(nullptr, gamePath, static_cast<DWORD>(std::size(gamePath)));
	if (length == 0 || length >= std::size(gamePath))
	{
		LOG_ERROR("[MlServiceLauncher] Cannot resolve the game installation directory." << std::endl);
		return;
	}
	const auto directory = std::filesystem::path(gamePath).parent_path() / L"RSMods";
	const auto executable = directory / L"RSMods.exe";
	std::error_code error;
	if (!std::filesystem::is_regular_file(executable, error)
		|| !std::filesystem::is_regular_file(directory.parent_path() / L"rsmodsplus.dll", error))
	{
		LOG_ERROR("[MlServiceLauncher] Bundled ML service is missing. Install the complete matching release package." << std::endl);
		return;
	}

	serviceJob = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (serviceJob == nullptr || !SetInformationJobObject(serviceJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
	{
		LOG_ERROR("[MlServiceLauncher] Cannot establish game-owned ML service lifetime, error=" << GetLastError() << std::endl);
		if (serviceJob != nullptr) CloseHandle(serviceJob);
		serviceJob = nullptr;
		return;
	}

	std::wstring command = L"\"" + executable.wstring() + L"\" --ml-service " + std::to_wstring(GetCurrentProcessId());
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
		CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process))
	{
		LOG_ERROR("[MlServiceLauncher] Cannot start bundled ML service, error=" << GetLastError() << std::endl);
		CloseHandle(serviceJob);
		serviceJob = nullptr;
		return;
	}
	if (!AssignProcessToJobObject(serviceJob, process.hProcess) || ResumeThread(process.hThread) == static_cast<DWORD>(-1))
	{
		LOG_ERROR("[MlServiceLauncher] Cannot bind/start bundled ML service, error=" << GetLastError() << std::endl);
		TerminateProcess(process.hProcess, 1);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		CloseHandle(serviceJob);
		serviceJob = nullptr;
		return;
	}
	CloseHandle(process.hThread);
	serviceProcess = process.hProcess;
	LOG_INFO("[MlServiceLauncher] Started bundled C# FretNet service with embedded model; bound to game lifetime." << std::endl);
}

void MlServiceLauncher::Shutdown()
{
	if (serviceJob != nullptr)
	{
		CloseHandle(serviceJob);
		serviceJob = nullptr;
	}
	if (serviceProcess != nullptr)
	{
		CloseHandle(serviceProcess);
		serviceProcess = nullptr;
	}
}
