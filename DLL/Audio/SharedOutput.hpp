#pragma once

#include <Windows.h>
#include <audioclient.h>
#include <string>

namespace Audio::SharedOutput
{
	struct Configuration
	{
		bool enabled = false;
		std::wstring inputDeviceId;
		std::wstring outputDeviceId;
	};

	Configuration ReadConfiguration();
	HRESULT CreateClient(const std::wstring& endpointId, IAudioClient** client);
}
