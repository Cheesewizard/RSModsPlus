#pragma once

#include "NoteByNoteTypes.hpp"

namespace NoteByNoteHostServices
{
	// The HostApi the scoring engine is initialized with. Stable for the life of the module.
	const NoteByNoteProtocol::HostApi& GetHostApi();

	using LogTelemetrySink = void (*)(NoteByNoteProtocol::LogLevel level, const char* message);
	void SetLogTelemetrySink(LogTelemetrySink sink);
}
