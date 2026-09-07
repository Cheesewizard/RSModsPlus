#include "../stdafx.h"
#include "DebugToolsLoader.hpp"
#include "DebugToolsIdentity.hpp"
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace
{
	HMODULE module = nullptr;
	StopDebugBridge stopBridge = nullptr;

	bool Verify(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream) return false;
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
		bool valid = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
		char buffer[65536];
		while (valid && stream)
		{
			stream.read(buffer, sizeof(buffer));
			if (stream.gcount() > 0) valid = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer), static_cast<ULONG>(stream.gcount()), 0) >= 0;
		}
		unsigned char digest[32];
		valid = valid && !stream.bad() && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
		if (hash != nullptr) BCryptDestroyHash(hash);
		BCryptCloseAlgorithmProvider(algorithm, 0);
		if (!valid) return false;
		char text[65];
		for (int index = 0; index < 32; ++index) sprintf_s(text + index * 2, 3, "%02X", digest[index]);
		return strcmp(text, DEBUG_TOOLS_SHA256) == 0;
	}
}

bool DebugToolsLoader::Start(const DebugHostApi& api)
{
	wchar_t path[MAX_PATH];
	const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
	if (length == 0 || length >= MAX_PATH) return false;
	const auto library = std::filesystem::path(path).parent_path() / L"rsmodsdebug.dll";
	std::error_code error;
	if (!std::filesystem::exists(library, error)) return false;
	// Hold the file against writes/replacement between verification and loading.
	const auto file = CreateFileW(library.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	if (Verify(library)) module = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	CloseHandle(file);
	if (module == nullptr)
	{
		LOG_ERROR("(DEBUG TOOLS) Debug DLL missing, incompatible, or hash verification failed; bridge remains disabled." << std::endl);
		return false;
	}
	const auto startBridge = reinterpret_cast<StartDebugBridge>(GetProcAddress(module, "RSMP_StartDebugBridge"));
	stopBridge = reinterpret_cast<StopDebugBridge>(GetProcAddress(module, "RSMP_StopDebugBridge"));
	if (startBridge == nullptr || stopBridge == nullptr || !startBridge(&api))
	{
		LOG_ERROR("(DEBUG TOOLS) Debug DLL rejected initialization; bridge remains disabled." << std::endl);
		FreeLibrary(module);
		module = nullptr;
		stopBridge = nullptr;
		return false;
	}
	return true;
}

void DebugToolsLoader::Stop()
{
	if (module == nullptr) return;
	stopBridge();
	FreeLibrary(module);
	module = nullptr;
	stopBridge = nullptr;
}
