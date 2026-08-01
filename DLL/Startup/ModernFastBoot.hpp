#pragma once

namespace Startup::ModernFastBoot
{
	void TryHookWbemLocator(REFCLSID classId, REFIID interfaceId, void* createdObject);
	void CompleteStartupSuppression();
}
