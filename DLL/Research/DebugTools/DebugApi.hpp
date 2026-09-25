#pragma once
#include <cstdint>

struct DebugHostApi
{
	uint32_t version;
	uint32_t size;
	uint32_t(__cdecl* HandleRequest)(const char* request, char* response, uint32_t capacity);
};

using StartDebugBridge = uint32_t(__cdecl*)(const DebugHostApi*);
using StopDebugBridge = void(__cdecl*)();
