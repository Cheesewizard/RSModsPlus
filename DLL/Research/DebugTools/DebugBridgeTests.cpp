#include <windows.h>
#include <cassert>
#include <cstring>
#include <string>

static std::string testPipeName;
static HANDLE CreateTestPipe(LPCSTR name, DWORD access, DWORD mode, DWORD instances,
    DWORD outputSize, DWORD inputSize, DWORD timeout, LPSECURITY_ATTRIBUTES attributes)
{
    assert(std::string(name) == "\\\\.\\pipe\\RSModsPlus.Research");
    assert((mode & PIPE_REJECT_REMOTE_CLIENTS) != 0);
    assert((access & FILE_FLAG_FIRST_PIPE_INSTANCE) != 0);
    BOOL present = FALSE, defaulted = TRUE;
    PACL acl = nullptr;
    assert(GetSecurityDescriptorDacl(attributes->lpSecurityDescriptor, &present, &acl, &defaulted));
    assert(present && !defaulted && acl != nullptr && acl->AceCount == 1);
    return CreateNamedPipeA(testPipeName.c_str(), access, mode, instances, outputSize, inputSize, timeout, attributes);
}

#define CreateNamedPipeA CreateTestPipe
#include "DebugBridge.cpp"
#undef CreateNamedPipeA

static uint32_t __cdecl Echo(const char* request, char* response, uint32_t capacity)
{
    const auto length = strlen(request);
    assert(length < capacity);
    memcpy(response, request, length);
    return static_cast<uint32_t>(length);
}

int main()
{
    testPipeName = "\\\\.\\pipe\\RSModsPlus.DebugTest." + std::to_string(GetCurrentProcessId());
    assert(RSMP_StartDebugBridge(nullptr) == 0);
    DebugHostApi api = { 1, sizeof(DebugHostApi), Echo };
    assert(RSMP_StartDebugBridge(&api) == 1);
    assert(RSMP_StartDebugBridge(&api) == 0);
    const auto client = CreateFileA(testPipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(client != INVALID_HANDLE_VALUE);
    DWORD count = 0;
    assert(WriteFile(client, "hello\n", 6, &count, nullptr) && count == 6);
    char response[64] = {};
    assert(ReadFile(client, response, sizeof(response), &count, nullptr));
    assert(count == 6 && memcmp(response, "hello\n", 6) == 0);
    assert(WriteFile(client, "ack", 3, &count, nullptr));
    CloseHandle(client);
    RSMP_StopDebugBridge();
    RSMP_StopDebugBridge();
    assert(CreateFileA(testPipeName.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE);
    return 0;
}
