#include "stdafx.h"
#include "MlServiceLauncher.hpp"
#include "MlStringFretReader.hpp"
#include "../Log.hpp"

#include <windows.h>

#include <filesystem>
#include <string>

namespace
{
	// The companion result mailbox the Python service creates. If it already exists a
	// service is running (ours from a previous launch that outlived nothing, or one the
	// user started by hand) - opening it and finding it present is the single-instance
	// guard that also kills the duplicate-writer bug at the source.
	constexpr char SF_MAPPING_NAME[] = "Local\\RSModsPlus.MlStringFret.v2";

	// The service lives in the research tree, which is NOT deployed next to the game, so
	// the host cannot derive it from its own module path. Default to the pinned research
	// workstation location and let an environment variable override it for any other box.
	// On a machine without the tree this simply does not exist and the launch no-ops.
	constexpr wchar_t DEFAULT_SERVICE_DIR[] =
		L"C:\\Programming\\RSModsPlus\\tools\\ml-string-fret-service";
	constexpr wchar_t SERVICE_DIR_ENV[] = L"RSMODSPLUS_ML_SERVICE_DIR";

	// Spawn is attempted exactly once per game run, whatever the outcome; a per-tick
	// retry against a missing tree or a hard failure would just spam.
	bool g_attempted = false;
	// The job that binds the child to this (game) process: closing our handle - or the
	// game dying and the kernel closing it for us - kills the child. This is the whole
	// "game lifecycle" contract.
	HANDLE g_job = nullptr;

	std::wstring ResolveServiceDir()
	{
		wchar_t buffer[MAX_PATH * 2] = {};
		const DWORD length = GetEnvironmentVariableW(
			SERVICE_DIR_ENV, buffer, static_cast<DWORD>(std::size(buffer)));
		if (length > 0 && length < std::size(buffer)) return std::wstring(buffer, length);
		return DEFAULT_SERVICE_DIR;
	}

	// A service is considered already up if its result mailbox exists.
	bool ServiceAlreadyRunning()
	{
		HANDLE existing = OpenFileMappingA(FILE_MAP_READ, FALSE, SF_MAPPING_NAME);
		if (existing == nullptr) return false;
		CloseHandle(existing);
		return true;
	}
}

void MlServiceLauncher::EnsureStarted()
{
	if (g_attempted) return;
	g_attempted = true;

	if (ServiceAlreadyRunning())
	{
		LOG_INFO("[MlServiceLauncher] ML string/fret service already running; not spawning a second (single instance)." << std::endl);
		return;
	}

	const std::filesystem::path serviceDir = ResolveServiceDir();
	const std::filesystem::path python = serviceDir / L".venv" / L"Scripts" / L"python.exe";
	const std::filesystem::path script = serviceDir / L"service.py";

	std::error_code ec;
	if (!std::filesystem::exists(python, ec) || !std::filesystem::exists(script, ec))
	{
		LOG_INFO("[MlServiceLauncher] ML service not launched: "
			<< python.string() << " or service.py not found. Set "
			<< "RSMODSPLUS_ML_SERVICE_DIR to the ml-string-fret-service folder to enable it." << std::endl);
		return;
	}

	// Kill-on-close job: when the last handle to the job closes (we close ours on
	// Shutdown, or the kernel closes it when the game process dies), the child is
	// terminated. Bind the child before it runs so no window of un-parented life exists.
	g_job = CreateJobObjectW(nullptr, nullptr);
	if (g_job != nullptr)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit = {};
		limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
	}

	// Redirect the child's stdout/stderr to a log next to the service so a failed launch
	// is diagnosable. Opened inheritable; append so successive runs accumulate.
	SECURITY_ATTRIBUTES sa = {};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	const std::filesystem::path logPath = serviceDir / L"service-autostart.log";
	HANDLE logHandle = CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	const bool haveLog = logHandle != INVALID_HANDLE_VALUE && logHandle != nullptr;

	// CommandLine must be a writable buffer. Quote the exe; -u keeps the child's output
	// unbuffered so the log is live.
	std::wstring commandLine = L"\"" + python.wstring() + L"\" -u service.py";
	std::wstring writableCommand = commandLine;

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	if (haveLog)
	{
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdOutput = logHandle;
		startup.hStdError = logHandle;
		startup.hStdInput = INVALID_HANDLE_VALUE;
	}

	PROCESS_INFORMATION process = {};
	// CREATE_SUSPENDED so the child is assigned to the job before its first instruction;
	// CREATE_NO_WINDOW keeps the console service headless.
	const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW;
	const BOOL created = CreateProcessW(
		python.wstring().c_str(),
		writableCommand.data(),
		nullptr, nullptr,
		haveLog ? TRUE : FALSE,
		flags,
		nullptr,
		serviceDir.wstring().c_str(),
		&startup,
		&process);

	if (haveLog) CloseHandle(logHandle);

	if (!created)
	{
		const DWORD err = GetLastError();
		LOG_ERROR("[MlServiceLauncher] Failed to launch the ML service (CreateProcess error "
			<< err << "): " << python.string() << std::endl);
		if (g_job != nullptr) { CloseHandle(g_job); g_job = nullptr; }
		return;
	}

	if (g_job != nullptr)
	{
		// If assignment fails the child still runs; it just is not bound to the game's
		// lifetime. Log it rather than aborting a working service.
		if (!AssignProcessToJobObject(g_job, process.hProcess))
		{
			LOG_ERROR("[MlServiceLauncher] Could not bind the ML service to the game job (error "
				<< GetLastError() << "); it will not auto-exit with the game." << std::endl);
		}
	}

	ResumeThread(process.hThread);
	CloseHandle(process.hThread);
	// The job (or the kernel on game exit) owns the child's lifetime now; we do not need
	// the process handle. Closing it does not affect the running child.
	CloseHandle(process.hProcess);

	LOG_INFO("[MlServiceLauncher] Launched the FretNet ML string/fret service from "
		<< serviceDir.string() << " (bound to the game's lifetime)." << std::endl);
}

void MlServiceLauncher::Shutdown()
{
	if (g_job != nullptr)
	{
		// Closing the last job handle terminates the child (KILL_ON_JOB_CLOSE).
		CloseHandle(g_job);
		g_job = nullptr;
	}
}
