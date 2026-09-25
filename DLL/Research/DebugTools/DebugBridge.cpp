#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "DebugApi.hpp"

namespace
{
	std::atomic<bool> stopping{ false };
	std::thread server;
	DebugHostApi host = {};
	HANDLE pipe = INVALID_HANDLE_VALUE;

	void RunServer()
	{
		while (!stopping.load())
		{
			const bool connected = ConnectNamedPipe(pipe, nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
			if (!connected) { Sleep(10); continue; }
			std::string request;
			const auto deadline = GetTickCount64() + 2000;
			while (!stopping.load() && GetTickCount64() < deadline && request.size() < 65536)
			{
				char bytes[4096];
				DWORD count = 0;
				if (!ReadFile(pipe, bytes, sizeof(bytes), &count, nullptr))
				{
					if (GetLastError() == ERROR_NO_DATA) { Sleep(5); continue; }
					break;
				}
				request.append(bytes, count);
				const auto end = request.find('\n');
				if (end == std::string::npos) continue;
				request.resize(end);
				std::vector<char> response(1024 * 1024);
				const auto length = host.HandleRequest(request.c_str(), response.data(), static_cast<uint32_t>(response.size() - 1));
				if (length > 0 && length < response.size())
				{
					response[length] = '\n';
					DWORD sent = 0;
					DWORD written = 0;
					while (sent < length + 1 && GetTickCount64() < deadline && !stopping.load())
					{
						if (!WriteFile(pipe, response.data() + sent, length + 1 - sent, &written, nullptr)) break;
						sent += written;
						if (written == 0) Sleep(5);
					}
					while (GetTickCount64() < deadline && !stopping.load())
					{
						if (ReadFile(pipe, bytes, sizeof(bytes), &count, nullptr) && count > 0) break;
						if (GetLastError() == ERROR_BROKEN_PIPE) break;
						Sleep(5);
					}
				}
				break;
			}
			DisconnectNamedPipe(pipe);
		}
	}

	bool CreatePrivatePipe()
	{
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
		DWORD size = 0;
		GetTokenInformation(token, TokenGroups, nullptr, 0, &size);
		std::vector<unsigned char> bytes(size);
		if (!GetTokenInformation(token, TokenGroups, bytes.data(), size, &size)) { CloseHandle(token); return false; }
		CloseHandle(token);
		auto* groups = reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
		LPSTR logonSid = nullptr;
		for (DWORD index = 0; index < groups->GroupCount; ++index)
		{
			if ((groups->Groups[index].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
			{
				ConvertSidToStringSidA(groups->Groups[index].Sid, &logonSid);
				break;
			}
		}
		if (logonSid == nullptr) return false;
		const std::string sddl = std::string("D:P(A;;GA;;;") + logonSid + ")";
		LocalFree(logonSid);
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
		SECURITY_ATTRIBUTES attributes = { sizeof(attributes), descriptor, FALSE };
		pipe = CreateNamedPipeA("\\\\.\\pipe\\RSModsPlus.Research", PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1, 65536, 65536, 0, &attributes);
		LocalFree(descriptor);
		return pipe != INVALID_HANDLE_VALUE;
	}
}

extern "C" uint32_t __cdecl RSMP_StartDebugBridge(const DebugHostApi* api)
{
	if (api == nullptr || api->version != 1 || api->size != sizeof(DebugHostApi) || api->HandleRequest == nullptr || server.joinable()) return 0;
	if (!CreatePrivatePipe()) return 0;
	host = *api;
	stopping.store(false);
	try { server = std::thread(RunServer); }
	catch (...) { CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; return 0; }
	return 1;
}

extern "C" void __cdecl RSMP_StopDebugBridge()
{
	stopping.store(true);
	if (server.joinable()) server.join();
	if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
	pipe = INVALID_HANDLE_VALUE;
}
