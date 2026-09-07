#include "../stdafx.h"
#include "ResearchBridge.hpp"
#include "DebugToolsLoader.hpp"

#include "../Mods/NoteByNoteNativeScoring.hpp"
#include "../Mods/NoteByNoteProbe.hpp"
#include "../Mods/NoteByNoteHostServices.hpp"
#include "../Mods/NoteByNoteMenu.hpp"
#include "../D3D/NoteByNoteHighwayRenderer.hpp"
#include "../D3D/D3DHooks.hpp"
#include "../Audio/MlAudioExporter.hpp"
#include "../Audio/MlStringFretReader.hpp"
#include "../Audio/RawPitchVerifier.hpp"
#include "../OverlayToggles.hpp"
#include "../Mods/DropPedal/DropPedal.hpp"
#include "../Mods/DropPedal/DropPedalInput.hpp"
#include "../Mods/FakeGuitar/FakeGuitarInjector.hpp"
#include "../GameState.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>

// The scoring engine is linked into this module (the in-process build), so the host calls its
// ProbeApi factory directly instead of resolving it from a loaded DLL. Declared here with the same
// signature the reloadable probe exports.
extern "C" const ResearchProtocol::ProbeApi* __cdecl RSMP_GetResearchProbeApi(
	uint32_t hostApiVersion,
	const ResearchProtocol::HostApi* hostApi);

namespace
{
	constexpr size_t MAX_RETAINED_EVENTS = 2048;

	struct ResearchEvent
	{
		uint64_t sequence = 0;
		nlohmann::json value;
	};

	// The in-process Note by Note controller reuses the loaded-probe dispatch surface. Its module
	// handle is this sentinel (never a real HMODULE), so DisposeProbe never FreeLibrary's it and
	// the host treats it as loaded through the same `module != nullptr` guards.
	const HMODULE INPROCESS_PROBE_MODULE = reinterpret_cast<HMODULE>(1);

	struct LoadedProbe
	{
		HMODULE module = nullptr;
		ResearchProtocol::ProbeApi api;
		std::filesystem::path loadedPath;
		std::string name;
		std::string buildId;
		// Identity of the file that was actually loaded (2026-09-02): which source path, its
		// content hash, and whether it links the DEBUG CRT. Three "stale probe" incidents in
		// three days all had the same shape: a file that hash-matched a build output but was
		// not the build anyone thought it was, and nothing in the running game could say so.
		// buildId cannot (it is a per-TU __TIME__ stamp); this can.
		std::filesystem::path sourcePath;
		uint64_t sourceHash = 0;
		bool isDebugBuild = false;
	};

#if defined(_DEBUG)
	constexpr bool HOST_IS_DEBUG_BUILD = true;
#else
	constexpr bool HOST_IS_DEBUG_BUILD = false;
#endif

	// FNV-1a over the file plus a scan of its import names for the debug CRT. The import
	// names are plain ASCII in the image, so a substring scan is enough to tell a Debug link
	// (ucrtbased / VCRUNTIME140D / MSVCP140D) from a Release one.
	bool InspectProbeFile(
		const std::filesystem::path& path,
		uint64_t& hash,
		bool& isDebugBuild)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream) return false;
		std::vector<char> bytes(
			(std::istreambuf_iterator<char>(stream)),
			std::istreambuf_iterator<char>());
		uint64_t h = 1469598103934665603ULL;
		for (const char byte : bytes)
		{
			h ^= static_cast<unsigned char>(byte);
			h *= 1099511628211ULL;
		}
		hash = h;

		// Walk the PE import directory for the linked DLL names. A substring scan over the
		// image is not good enough: this very function carries the debug CRT names as string
		// literals, so any binary containing this code would flag itself.
		isDebugBuild = false;
		const auto* base = reinterpret_cast<const unsigned char*>(bytes.data());
		const size_t size = bytes.size();
		auto readU16 = [&](size_t at) -> uint32_t {
			return at + 2 <= size ? base[at] | (base[at + 1] << 8) : 0; };
		auto readU32 = [&](size_t at) -> uint32_t {
			return at + 4 <= size
				? base[at] | (base[at + 1] << 8) | (base[at + 2] << 16)
					| (static_cast<uint32_t>(base[at + 3]) << 24)
				: 0; };
		if (size < 0x40 || readU16(0) != 0x5A4D) return true;
		const size_t pe = readU32(0x3C);
		if (pe + 24 > size || readU32(pe) != 0x00004550) return true;
		const uint32_t sectionCount = readU16(pe + 6);
		const uint32_t optionalSize = readU16(pe + 20);
		const size_t optional = pe + 24;
		const bool is64 = readU16(optional) == 0x20B;
		const uint32_t importRva = readU32(optional + (is64 ? 112 : 96) + 8);
		if (importRva == 0) return true;
		struct Section { uint32_t va, size, raw, rawSize; };
		std::vector<Section> sections;
		const size_t sectionTable = optional + optionalSize;
		for (uint32_t i = 0; i < sectionCount; ++i)
		{
			const size_t s = sectionTable + i * 40;
			sections.push_back({ readU32(s + 12), readU32(s + 8), readU32(s + 20), readU32(s + 16) });
		}
		auto rvaToOffset = [&](uint32_t rva) -> size_t {
			for (const auto& s : sections)
			{
				const uint32_t extent = s.size > s.rawSize ? s.size : s.rawSize;
				if (rva >= s.va && rva < s.va + extent) return rva - s.va + s.raw;
			}
			return rva; };
		for (size_t descriptor = rvaToOffset(importRva);
			descriptor + 20 <= size;
			descriptor += 20)
		{
			const uint32_t nameRva = readU32(descriptor + 12);
			if (nameRva == 0) break;
			const size_t nameOffset = rvaToOffset(nameRva);
			if (nameOffset >= size) break;
			std::string name;
			for (size_t at = nameOffset; at < size && base[at] != 0 && name.size() < 64; ++at)
			{
				name.push_back(static_cast<char>(std::tolower(base[at])));
			}
			if (name == "ucrtbased.dll" || name == "vcruntime140d.dll" || name == "msvcp140d.dll")
			{
				isDebugBuild = true;
				break;
			}
		}
		return true;
	}

	std::recursive_mutex probeMutex;
	LoadedProbe loadedProbe;

	// Full draw feed (the doubles capture forensics): marshals a per-draw struct to the
	// probe on EVERY DrawPrimitive, which the comment below always warned could show up
	// in frame rate. It did (2026-09-01, first NBN-in-Release build: 60 -> 45 FPS even
	// with NBN off) - it is research capture, unrelated to NBN's own visuals (the
	// physical-guide suppression and stale-marker filter run separately in the draw
	// hook). So it defaults ON only in a research (Debug) build and OFF in Release;
	// set_full_draw_feed still enables it on demand for a capture session, and
	// hasFullDrawConsumer mirrors whether the loaded probe supplies ObserveFullDraw so
	// the hot path skips the probe mutex when nobody is listening.
#if defined(_DEBUG)
	std::atomic<bool> isFullDrawFeedEnabled{ true };
#else
	std::atomic<bool> isFullDrawFeedEnabled{ false };
#endif
	std::atomic<bool> hasFullDrawConsumer{ false };

	// Native draw feed (2026-09-02): the filtered note-head forward behind the probe's
	// render-snapshot and screen-map captures. It was dispatched on every note-asset draw
	// whenever a probe was loaded (probe mutex per draw, plus the struct build with two
	// SongTimer walks per draw in the hook), and the probe then discarded it unless a
	// capture was armed. Same policy as the full feed now: Debug on, Release off, the two
	// captures that consume it switch it on when armed, and set_native_draw_feed toggles it.
#if defined(_DEBUG)
	std::atomic<bool> isNativeDrawFeedEnabled{ true };
#else
	std::atomic<bool> isNativeDrawFeedEnabled{ false };
#endif
	std::atomic<bool> hasNativeDrawConsumer{ false };

	// Fake-guitar autoplay: when on, the host loop reads the frozen Note by Note target every
	// tick and injects the matching signal (note, chord, or bend glide) into the fake guitar,
	// so the harness walks a section hands-free with no external driver. autoPlayLastRecord is
	// touched only from the host-loop thread.
	std::atomic<bool> fakeGuitarAutoPlay{ false };
	uintptr_t autoPlayLastRecord = 0;
	// The highest pitch the detector reported for the current bend, so a short attempt can be
	// corrected by bending higher next time (closed loop, not a blind repeat).
	float autoPlayBendPeak = -1.0f;
	std::mutex eventMutex;
	std::deque<ResearchEvent> events;
	uint64_t nextEventSequence = 1;
	uint64_t loadSequence = 1;
	std::atomic<bool> isStopping = false;
	std::atomic<bool> debugToolsActive{ false };

	// Direct memory access over the pipe, so "read this struct" and "flip this flag"
	// experiments cost a command instead of a probe build. Lives in the bridge rather than
	// the probe so it works with no probe loaded and survives probe reloads.
	constexpr size_t MAX_MEMORY_READ_BYTES = 64 * 1024;
	constexpr size_t MAX_MEMORY_WRITE_BYTES = 4 * 1024;
	constexpr size_t MAX_MEMORY_WATCH_BYTES = 256;
	constexpr size_t MAX_MEMORY_WATCHES = 32;

	struct MemoryWatch
	{
		uint64_t id = 0;
		uintptr_t address = 0;
		size_t size = 0;
		uint32_t intervalMilliseconds = 100;
		std::string label;
		std::vector<uint8_t> lastBytes;
		bool hasSample = false;
		bool wasReadable = false;
		ULONGLONG nextDueTick = 0;
		uint64_t sampleCount = 0;
		uint64_t changeCount = 0;
	};

	std::mutex watchMutex;
	std::vector<MemoryWatch> memoryWatches;
	uint64_t nextWatchId = 1;
	std::thread watchThread;

	const char* GetGatePhaseName(ResearchProtocol::GatePhase phase)
	{
		switch (phase)
		{
			case ResearchProtocol::GatePhase::Idle:
				return "idle";
			case ResearchProtocol::GatePhase::Armed:
				return "armed";
			case ResearchProtocol::GatePhase::WaitingForInputRelease:
				return "waiting-for-input-release";
			case ResearchProtocol::GatePhase::Holding:
				return "holding";
			case ResearchProtocol::GatePhase::CommitBeforeRelease:
				return "commit-before-release";
			case ResearchProtocol::GatePhase::DenseRebuildPending:
				return "dense-rebuild-pending";
			case ResearchProtocol::GatePhase::DenseRecommitAfterRebuild:
				return "dense-recommit-after-rebuild";
			case ResearchProtocol::GatePhase::DensePlayerSongStartPending:
				return "dense-player-song-start-pending";
			case ResearchProtocol::GatePhase::PostRelease:
				return "post-release";
			case ResearchProtocol::GatePhase::RecommitAfterRelease:
				return "recommit-after-release";
			default:
				return "unknown";
		}
	}

	const char* GetExpectedEventName(NoteByNoteProbe::NativeExpectedAttackEventKind kind)
	{
		switch (kind)
		{
			case NoteByNoteProbe::NativeExpectedAttackEventKind::CandidateChanged:
				return "candidate-changed";
			case NoteByNoteProbe::NativeExpectedAttackEventKind::HoldEstablished:
				return "hold-established";
			case NoteByNoteProbe::NativeExpectedAttackEventKind::ScoringStateChanged:
				return "scoring-state-changed";
			default:
				return "unknown";
		}
	}

	void PublishEvent(nlohmann::json value)
	{
		if (!debugToolsActive.load(std::memory_order_relaxed)) return;
		std::lock_guard<std::mutex> lock(eventMutex);
		value["sequence"] = nextEventSequence;
		events.push_back({ nextEventSequence, std::move(value) });
		++nextEventSequence;
		while (events.size() > MAX_RETAINED_EVENTS) events.pop_front();
	}

	// Installed as the host-services log sink while the bridge is up, so host log lines a shipping
	// build only writes to the file logger also reach a connected research tool's event stream.
	void __cdecl MirrorHostLogToEvents(ResearchProtocol::LogLevel level, const char* message)
	{
		PublishEvent({
			{ "type", "probe-log" },
			{ "level", static_cast<uint32_t>(level) },
			{ "message", message == nullptr ? std::string() : std::string(message) }
		});
	}

	// Accepts a JSON number or a hex string ("0x7E2880" or "7E2880"). Strings are always
	// read as hex because every address in this workflow is quoted from Ghidra.
	bool TryParseAddress(const nlohmann::json& value, uintptr_t& address)
	{
		if (value.is_number_unsigned() || value.is_number_integer())
		{
			const auto parsed = value.get<uint64_t>();
			if (parsed > (std::numeric_limits<uintptr_t>::max)()) return false;
			address = static_cast<uintptr_t>(parsed);
			return true;
		}
		if (!value.is_string()) return false;
		auto text = value.get<std::string>();
		if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) text.erase(0, 2);
		if (text.empty() || text.size() > sizeof(uintptr_t) * 2) return false;
		uintptr_t parsed = 0;
		for (const char character : text)
		{
			uint8_t digit = 0;
			if (character >= '0' && character <= '9') digit = character - '0';
			else if (character >= 'a' && character <= 'f') digit = character - 'a' + 10;
			else if (character >= 'A' && character <= 'F') digit = character - 'A' + 10;
			else return false;
			parsed = (parsed << 4) | digit;
		}
		address = parsed;
		return true;
	}

	std::string ToHex(const uint8_t* data, size_t size)
	{
		static constexpr char DIGITS[] = "0123456789abcdef";
		std::string text;
		text.reserve(size * 2);
		for (size_t index = 0; index < size; ++index)
		{
			text.push_back(DIGITS[data[index] >> 4]);
			text.push_back(DIGITS[data[index] & 0x0F]);
		}
		return text;
	}

	bool TryParseHexBytes(const std::string& text, std::vector<uint8_t>& bytes)
	{
		if (text.empty() || (text.size() % 2) != 0) return false;
		bytes.clear();
		bytes.reserve(text.size() / 2);
		uint8_t assembled = 0;
		for (size_t index = 0; index < text.size(); ++index)
		{
			const char character = text[index];
			uint8_t digit = 0;
			if (character >= '0' && character <= '9') digit = character - '0';
			else if (character >= 'a' && character <= 'f') digit = character - 'a' + 10;
			else if (character >= 'A' && character <= 'F') digit = character - 'A' + 10;
			else return false;
			assembled = (assembled << 4) | digit;
			if ((index % 2) == 1) bytes.push_back(assembled);
		}
		return true;
	}

	// ReadProcessMemory on the own process respects page protection and fails cleanly on
	// unmapped addresses, so a typo'd address returns an error instead of crashing the game.
	bool TryReadGameMemory(uintptr_t address, size_t size, std::vector<uint8_t>& bytes)
	{
		bytes.resize(size);
		SIZE_T bytesRead = 0;
		return ReadProcessMemory(
			GetCurrentProcess(),
			reinterpret_cast<LPCVOID>(address),
			bytes.data(),
			size,
			&bytesRead) != FALSE
			&& bytesRead == size;
	}

	// Samples every armed watch on its own interval and publishes an event only when the
	// bytes change (or readability flips), so a watch on a hot value does not flood the
	// event queue. Runs from Initialize so the thread exists whether or not a probe does.
	void RunMemoryWatcher()
	{
		while (!isStopping)
		{
			{
				std::lock_guard<std::mutex> lock(watchMutex);
				const auto now = GetTickCount64();
				for (auto& watch : memoryWatches)
				{
					if (now < watch.nextDueTick) continue;
					watch.nextDueTick = now + watch.intervalMilliseconds;

					std::vector<uint8_t> bytes;
					const bool isReadable = TryReadGameMemory(watch.address, watch.size, bytes);
					++watch.sampleCount;

					const bool isFirstSample = !watch.hasSample;
					const bool readabilityChanged = watch.hasSample && isReadable != watch.wasReadable;
					const bool bytesChanged = isReadable
						&& watch.hasSample
						&& watch.wasReadable
						&& bytes != watch.lastBytes;
					if (!isFirstSample && !readabilityChanged && !bytesChanged)
					{
						continue;
					}
					if (bytesChanged) ++watch.changeCount;

					nlohmann::json event =
					{
						{ "type", "memory-watch" },
						{ "id", watch.id },
						{ "label", watch.label },
						{ "address", static_cast<uint64_t>(watch.address) },
						{ "size", watch.size },
						{ "readable", isReadable },
						{ "sampleCount", watch.sampleCount }
					};
					if (isReadable) event["bytes"] = ToHex(bytes.data(), bytes.size());
					if (bytesChanged)
					{
						event["previous"] = ToHex(watch.lastBytes.data(), watch.lastBytes.size());
					}
					PublishEvent(std::move(event));

					watch.hasSample = true;
					watch.wasReadable = isReadable;
					if (isReadable) watch.lastBytes = std::move(bytes);
				}
			}
			Sleep(10);
		}
	}

	std::filesystem::path GetHostDirectory()
	{
		HMODULE hostModule = nullptr;
		const auto address = reinterpret_cast<LPCSTR>(&ResearchBridge::Initialize);
		if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			address,
			&hostModule))
		{
			throw std::runtime_error("Could not resolve the loaded RSModsPlus module");
		}

		std::vector<wchar_t> pathBuffer(32768);
		const auto pathLength = GetModuleFileNameW(
			hostModule,
			pathBuffer.data(),
			static_cast<DWORD>(pathBuffer.size()));
		if (pathLength == 0 || pathLength >= pathBuffer.size())
		{
			throw std::runtime_error("Could not resolve the RSModsPlus module path");
		}
		return std::filesystem::path(pathBuffer.data()).parent_path();
	}

	std::filesystem::path GetDefaultProbePath()
	{
		return GetHostDirectory() / "RSModsResearch" / "NoteByNoteProbe.dll";
	}

	bool IsValidProbeApi(const ResearchProtocol::ProbeApi* api, std::string& error)
	{
		if (api == nullptr)
		{
			error = "The probe did not return an API";
			return false;
		}
		// Accept any probe at or above the minimum whose struct covers the required prefix,
		// rather than an exact match. A newer probe with a larger struct loads (the extra
		// entries are copied only up to what this host knows); an older probe missing the
		// required prefix is rejected. This is what lets optional entries be appended without
		// forcing every probe and the host to rebuild in lockstep.
		if (api->version < ResearchProtocol::PROBE_API_MIN_VERSION
			|| api->structSize < ResearchProtocol::REQUIRED_PROBE_API_SIZE)
		{
			error = "The probe API is older than the resident bridge accepts";
			return false;
		}
		if (api->name == nullptr
			|| api->buildId == nullptr
			|| api->Initialize == nullptr
			|| api->Shutdown == nullptr
			|| api->ProcessScoringUpdate == nullptr
			|| api->ProcessHitDecision == nullptr
			|| api->ObserveRenderedAttack == nullptr
			|| api->Stop == nullptr
			|| api->GetState == nullptr
			|| api->ArmRenderSnapshot == nullptr
			|| api->ObserveNativeDraw == nullptr
			|| api->NotifyRenderFrameComplete == nullptr
			|| api->ArmNoteDrawListSnapshot == nullptr
			|| api->ObserveNoteDrawList == nullptr
			|| api->ArmScreenMapSnapshot == nullptr
			|| api->HandleProbeCommand == nullptr
			|| api->PrepareNoteDrawList == nullptr
			|| api->CompleteNoteDrawList == nullptr)
		{
			error = "The probe API is incomplete";
			return false;
		}
		return true;
	}

	void DisposeProbe(LoadedProbe& probe)
	{
		if (probe.module == nullptr) return;
		probe.api.Shutdown();
		// The in-process controller has no library or on-disk copy to release; its module handle is
		// a sentinel, so only unload/remove for a real reloadable-probe DLL.
		if (probe.module != INPROCESS_PROBE_MODULE)
		{
			FreeLibrary(probe.module);
			std::error_code removeError;
			std::filesystem::remove(probe.loadedPath, removeError);
		}
		probe = {};
		// The probe's generic hooks (including the visual-destructor eviction detour) are
		// gone or about to be reconciled away; any remembered dim-candidate visual can now
		// die unnoticed, so it must be dropped here rather than trusted into the next hold.
		NoteByNoteHighwayRenderer::EvictRememberedDimCandidates();
	}

	// Hands the currently loaded probe's requested generic hooks to the hook host, or an empty
	// list when no probe is loaded. Called after every load and unload so the host installs new
	// addresses and unclaims slots the current probe no longer wants. The host stages the work
	// and applies it from a main-thread render seam, so this is safe to call from the pipe
	// thread. Must be called with probeMutex held.
	void ReconcileGenericHooksLocked()
	{
		const ResearchProtocol::HookRequest* requests = nullptr;
		uint32_t count = 0;
		if (loadedProbe.module != nullptr && loadedProbe.api.GetRequestedHooks != nullptr)
		{
			loadedProbe.api.GetRequestedHooks(&requests, &count);
		}
		NoteByNoteNativeScoring::SetRequestedGenericHooks(requests, count);

		// Runs after every probe load and unload, so it doubles as the reconcile point for
		// the full-draw feed's consumer flag.
		hasFullDrawConsumer.store(
			loadedProbe.module != nullptr && loadedProbe.api.ObserveFullDraw != nullptr,
			std::memory_order_release);
		hasNativeDrawConsumer.store(
			loadedProbe.module != nullptr && loadedProbe.api.ObserveNativeDraw != nullptr,
			std::memory_order_release);
	}

	bool CanReplaceProbe(std::string& error)
	{
		if (NoteByNoteProbe::IsAutomaticEnabled())
		{
			error = "Disable Note by Note before reloading its controller";
			return false;
		}

		ResearchProtocol::NoteByNoteState state;
		if (ResearchBridge::TryGetNoteByNoteState(state) && state.ownsNativeHold != 0)
		{
			error = "The controller still owns a native hold; wait for its coordinated release";
			return false;
		}
		return true;
	}

	bool ReloadProbe(const std::filesystem::path& sourcePath, std::string& error)
	{
		if (!CanReplaceProbe(error)) return false;

		LoadedProbe candidate;
		try
		{
			const auto absoluteSource = std::filesystem::absolute(sourcePath);
			if (!std::filesystem::is_regular_file(absoluteSource))
			{
				error = "Probe DLL does not exist: " + absoluteSource.string();
				return false;
			}
			if (absoluteSource.extension() != ".dll")
			{
				error = "The probe path must point to a DLL";
				return false;
			}

			const auto cacheDirectory = GetHostDirectory() / "RSModsResearch" / "Loaded";
			std::filesystem::create_directories(cacheDirectory);
			std::ostringstream fileName;
			fileName << "NoteByNoteProbe-" << GetCurrentProcessId() << '-' << loadSequence++ << ".dll";
			candidate.loadedPath = cacheDirectory / fileName.str();
			std::filesystem::copy_file(
				absoluteSource,
				candidate.loadedPath,
				std::filesystem::copy_options::overwrite_existing);

			candidate.module = LoadLibraryW(candidate.loadedPath.c_str());
			if (candidate.module == nullptr)
			{
				error = "LoadLibrary failed for the copied probe with error "
					+ std::to_string(GetLastError());
				std::error_code removeError;
				std::filesystem::remove(candidate.loadedPath, removeError);
				return false;
			}

			const auto getApi = reinterpret_cast<ResearchProtocol::GetProbeApi>(
				GetProcAddress(candidate.module, "RSMP_GetResearchProbeApi"));
			if (getApi == nullptr)
			{
				error = "The DLL does not export RSMP_GetResearchProbeApi";
				FreeLibrary(candidate.module);
				candidate.module = nullptr;
				std::error_code removeError;
				std::filesystem::remove(candidate.loadedPath, removeError);
				return false;
			}

			const auto api = getApi(
				ResearchProtocol::HOST_API_VERSION, &NoteByNoteHostServices::GetHostApi());
			if (!IsValidProbeApi(api, error))
			{
				FreeLibrary(candidate.module);
				candidate.module = nullptr;
				std::error_code removeError;
				std::filesystem::remove(candidate.loadedPath, removeError);
				return false;
			}
			// Copy only what both sides agree on: a probe with a smaller struct than this host
			// knows must not be read past its end, and a probe with a larger struct has entries
			// this host cannot call. Zero-initialised first, so any entry the probe omits reads
			// back as null and is treated as absent.
			candidate.api = {};
			std::memcpy(
				&candidate.api,
				api,
				(std::min)(static_cast<size_t>(api->structSize), sizeof(ResearchProtocol::ProbeApi)));
			candidate.name = api->name;
			candidate.buildId = api->buildId;
			candidate.sourcePath = absoluteSource;
			if (!InspectProbeFile(absoluteSource, candidate.sourceHash, candidate.isDebugBuild))
			{
				LOG_WARNING("(RESEARCH BRIDGE) Could not inspect the probe file for its build"
					<< " configuration: " << absoluteSource.string() << std::endl);
			}
			else if (candidate.isDebugBuild != HOST_IS_DEBUG_BUILD)
			{
				// Loud on purpose. The probe still loads (a research session may want it),
				// but every measurement script refuses to run while status reports
				// probeConfigurationMismatch, so a wrong pair can no longer produce a number.
				LOG_ERROR("(RESEARCH BRIDGE) PROBE/HOST CONFIGURATION MISMATCH: the host is a "
					<< (HOST_IS_DEBUG_BUILD ? "Debug" : "Release") << " build but the probe at "
					<< absoluteSource.string() << " is a "
					<< (candidate.isDebugBuild ? "Debug" : "Release")
					<< " build (debug CRT " << (candidate.isDebugBuild ? "present" : "absent")
					<< "). Frame-rate and timing measurements taken with this pair are invalid."
					<< " Rebuild both in one configuration (msbuild DLL/DLL.vcxproj"
					<< " /p:Configuration=" << (HOST_IS_DEBUG_BUILD ? "Debug" : "Release")
					<< ") or reload from Installer/Resources/RSModsResearch/"
					<< (HOST_IS_DEBUG_BUILD ? "Debug" : "Release") << "/." << std::endl);
			}
			else
			{
				LOG_INFO("(RESEARCH BRIDGE) Probe loaded: " << absoluteSource.string()
					<< " hash=" << std::hex << candidate.sourceHash << std::dec
					<< " configuration=" << (candidate.isDebugBuild ? "Debug" : "Release")
					<< " (matches host)." << std::endl);
			}
			if (candidate.api.Initialize() == 0)
			{
				error = "The probe rejected initialization";
				candidate.api.Shutdown();
				FreeLibrary(candidate.module);
				candidate.module = nullptr;
				std::error_code removeError;
				std::filesystem::remove(candidate.loadedPath, removeError);
				return false;
			}
		}
		catch (const std::exception& exception)
		{
			error = exception.what();
			if (candidate.module != nullptr)
			{
				candidate.api.Shutdown();
				FreeLibrary(candidate.module);
			}
			return false;
		}

		LoadedProbe previous;
		{
			std::lock_guard<std::recursive_mutex> lock(probeMutex);
			if (NoteByNoteProbe::IsAutomaticEnabled())
			{
				error = "Note by Note was enabled while the replacement probe was loading";
				candidate.api.Shutdown();
				FreeLibrary(candidate.module);
				return false;
			}
			previous = std::move(loadedProbe);
			loadedProbe = std::move(candidate);
		}
		DisposeProbe(previous);
		{
			std::lock_guard<std::recursive_mutex> lock(probeMutex);
			ReconcileGenericHooksLocked();
		}

		LOG_INFO("(RESEARCH BRIDGE) Loaded " << loadedProbe.name << " build "
			<< loadedProbe.buildId << " from " << sourcePath.string() << "." << std::endl);
		PublishEvent({
			{ "type", "probe-loaded" },
			{ "name", loadedProbe.name },
			{ "buildId", loadedProbe.buildId },
			{ "source", std::filesystem::absolute(sourcePath).string() }
		});
		return true;
	}

	// In the Release/in-process build the scoring engine is linked into this module, so instead of
	// loading a probe DLL the host asks it for its ProbeApi directly. The rest of the bridge is
	// unchanged: the returned dispatch table lives in loadedProbe exactly as a reloaded DLL's would,
	// and the sentinel module keeps DisposeProbe from trying to unload it.
	bool LoadInProcessProbe(std::string& error)
	{
		if (!CanReplaceProbe(error)) return false;

		const auto api = RSMP_GetResearchProbeApi(
			ResearchProtocol::HOST_API_VERSION, &NoteByNoteHostServices::GetHostApi());
		if (!IsValidProbeApi(api, error)) return false;

		LoadedProbe candidate;
		candidate.module = INPROCESS_PROBE_MODULE;
		candidate.api = {};
		std::memcpy(
			&candidate.api,
			api,
			(std::min)(static_cast<size_t>(api->structSize), sizeof(ResearchProtocol::ProbeApi)));
		candidate.name = api->name;
		candidate.buildId = api->buildId;
		candidate.isDebugBuild = HOST_IS_DEBUG_BUILD;
		if (candidate.api.Initialize() == 0)
		{
			error = "The in-process controller rejected initialization";
			candidate.api.Shutdown();
			return false;
		}

		LoadedProbe previous;
		{
			std::lock_guard<std::recursive_mutex> lock(probeMutex);
			previous = std::move(loadedProbe);
			loadedProbe = std::move(candidate);
		}
		DisposeProbe(previous);
		{
			std::lock_guard<std::recursive_mutex> lock(probeMutex);
			ReconcileGenericHooksLocked();
		}
		LOG_INFO("(RESEARCH BRIDGE) Loaded in-process controller " << loadedProbe.name
			<< " build " << loadedProbe.buildId << "." << std::endl);
		PublishEvent({
			{ "type", "probe-loaded" },
			{ "name", loadedProbe.name },
			{ "buildId", loadedProbe.buildId },
			{ "source", "in-process" }
		});
		return true;
	}

	bool UnloadProbe(std::string& error)
	{
		if (!CanReplaceProbe(error)) return false;

		LoadedProbe previous;
		{
			std::lock_guard<std::recursive_mutex> lock(probeMutex);
			if (NoteByNoteProbe::IsAutomaticEnabled())
			{
				error = "Note by Note was enabled while the controller was being unloaded";
				return false;
			}
			previous = std::move(loadedProbe);
			// Moving a struct of raw pointers copies them, so without this reset the bridge
			// would keep dispatching into the freed module after an unload.
			loadedProbe = {};
		}
		DisposeProbe(previous);
		{
			std::lock_guard<std::recursive_mutex> lock(probeMutex);
			ReconcileGenericHooksLocked();
		}
		PublishEvent({ { "type", "probe-unloaded" } });
		return true;
	}

	nlohmann::json SerializeState()
	{
		nlohmann::json response =
		{
			{ "ok", true },
			{ "processId", GetCurrentProcessId() },
			{ "noteByNoteEnabled", NoteByNoteProbe::IsAutomaticEnabled() },
			{ "controllerAvailable", NoteByNoteNativeScoring::IsAvailable() }
		};

		// Reported so a run's log can always be read against the mode that produced it.
		{
			const auto mode = NoteByNoteHighwayRenderer::GetGridGateMode();
			uint32_t sentinelValue = 0;
			float sentinelKey = 0.0f;
			NoteByNoteHighwayRenderer::GetGridSentinel(sentinelValue, sentinelKey);
			response["gridGateMode"] = NoteByNoteHighwayRenderer::DescribeGridGateMode(mode);
			response["gridSentinelValue"] = sentinelValue;
			response["gridSentinelKey"] = sentinelKey;
			response["highwayMode"] = NoteByNoteHighwayRenderer::DescribeHighwayMode(
				NoteByNoteHighwayRenderer::GetHighwayMode());
			response["highwayWindowSeconds"] =
				NoteByNoteHighwayRenderer::GetHighwayWindowSeconds();
			response["neckPlacementMode"] = NoteByNoteHighwayRenderer::GetNeckPlacementMode();
			response["neckPlacementSiteMask"] =
				NoteByNoteHighwayRenderer::GetNeckPlacementSiteMask();

			const auto capture = D3DHooks::GetPhysicalMarkerCaptureState();
			response["physicalMarkerCapture"] =
			{
				{ "armGeneration", capture.armGeneration },
				{ "callbackCount", capture.callbackCount },
				{ "armCallbackBase", capture.armCallbackBase },
				{ "matchedDrawCount", capture.matchedDrawCount },
				{ "armMatchedDrawBase", capture.armMatchedDrawBase },
				{ "publishedDrawCount", capture.publishedDrawCount },
				{ "armPublishedDrawBase", capture.armPublishedDrawBase },
				{ "armed", capture.isArmed },
				{ "active", capture.isActive }
			};
			response["physicalGuideSuppression"] =
			{
				{ "enabled", capture.guideSuppressionEnabled },
				{ "consideredDrawCount", capture.guideConsideredDrawCount },
				{ "suppressedDrawCount", capture.guideSuppressedDrawCount }
			};
			const auto staleMarker = D3DHooks::GetStaleMarkerFilterState();
			response["staleMarkerFilter"] =
			{
				{ "enabled", staleMarker.enabled },
				{ "consideredQuads", staleMarker.consideredQuads },
				{ "droppedQuads", staleMarker.droppedQuads },
				{ "filteredDraws", staleMarker.filteredDraws },
				{ "lockWaits", staleMarker.lockWaits },
				{ "lockNoWaits", staleMarker.lockNoWaits }
			};
		}

		{
			std::lock_guard<std::recursive_mutex> lock(probeMutex);
			response["probeLoaded"] = loadedProbe.module != nullptr;
			if (loadedProbe.module != nullptr)
			{
				response["probeName"] = loadedProbe.name;
				response["probeBuildId"] = loadedProbe.buildId;
				response["probeSourcePath"] = loadedProbe.sourcePath.string();
				std::ostringstream hashText;
				hashText << std::hex << loadedProbe.sourceHash;
				response["probeSourceHash"] = hashText.str();
				response["probeIsDebugBuild"] = loadedProbe.isDebugBuild;
				response["probeConfigurationMismatch"] =
					loadedProbe.isDebugBuild != HOST_IS_DEBUG_BUILD;
			}
			response["hostIsDebugBuild"] = HOST_IS_DEBUG_BUILD;
			response["fullDrawFeedEnabled"] =
				isFullDrawFeedEnabled.load(std::memory_order_relaxed);
			response["fullDrawConsumerLoaded"] =
				hasFullDrawConsumer.load(std::memory_order_relaxed);
			response["nativeDrawFeedEnabled"] =
				isNativeDrawFeedEnabled.load(std::memory_order_relaxed);
			response["nativeDrawConsumerLoaded"] =
				hasNativeDrawConsumer.load(std::memory_order_relaxed);
		}

		// The native Riff Repeater NOTE BY NOTE rocker hook (issue #62): what it last saw, so a
		// menu test reads the focused row name and the toggle outcome instead of guessing.
		{
			const auto menu = NoteByNoteMenu::GetDiagnostics();
			response["riffRepeaterMenu"] =
			{
				{ "hooked", menu.hooked },
				{ "controllerCaptured", menu.controllerCaptured },
				{ "focusedReads", menu.focusedReads },
				{ "repairs", menu.repairs },
				{ "toggles", menu.toggles },
				{ "lastRowValue", menu.lastRowValue },
				{ "lastToggleAccepted", menu.lastToggleAccepted },
				{ "stageBuilds", menu.stageBuilds },
				{ "lastChildCount", menu.lastChildCount },
				{ "lastRowSortOrderFrom", menu.lastRowSortOrderFrom },
				{ "lastRowSortOrderTo", menu.lastRowSortOrderTo },
				{ "lastRowPreset", menu.lastRowPreset },
				{ "builderListing", menu.builderListing },
				{ "preBuilderContainer", menu.preBuilderContainer },
				{ "preBuilderVtable", menu.preBuilderVtable },
				{ "preBuilderSlot74", menu.preBuilderSlot74 },
				{ "lastBuilderController", menu.lastBuilderController },
				{ "lastBuilderCounter", menu.lastBuilderCounter },
				{ "probes", menu.probes },
				{ "probeContainer", menu.probeContainer },
				{ "probeRangeBegin", menu.probeRangeBegin },
				{ "probeRangeEnd", menu.probeRangeEnd },
				{ "probeChildCount", menu.probeChildCount },
				{ "probeListing", menu.probeListing },
				{ "builderContainer", menu.builderContainer },
				{ "builderVtable", menu.builderVtable },
				{ "builderSlot74", menu.builderSlot74 },
				{ "probeVtable", menu.probeVtable },
				{ "probeSlot74", menu.probeSlot74 }
			};
		}

		// Frame time from the EndScene seam, so FPS work is measured (see D3DHooks).
		{
			const auto frameTime = D3DHooks::GetFrameTimeStats();
			response["frameTime"] =
			{
				{ "averageMs", frameTime.averageMilliseconds },
				{ "windowMaxMs", frameTime.windowMaxMilliseconds },
				{ "fps", frameTime.averageMilliseconds > 0.0f
					? 1000.0f / frameTime.averageMilliseconds : 0.0f },
				{ "frames", frameTime.frameCount }
			};
		}

		// The synthetic-input test harness, so a driver can confirm the route is live before
		// injecting and read back what is currently sounding.
		response["fakeGuitar"] =
		{
			{ "installed", FakeGuitar::IsInstalled() },
			{ "synthEnabled", FakeGuitar::IsSynthEnabled() },
			{ "captureReady", FakeGuitar::IsCaptureReady() },
			{ "currentMidi", FakeGuitar::CurrentMidi() },
			{ "pending", FakeGuitar::PendingCount() },
			{ "autoPlay", fakeGuitarAutoPlay.load(std::memory_order_relaxed) },
			{ "samplesLoaded", FakeGuitar::SampleCount() }
		};

		// Pitch route (Drop Pedal / Speaker Mode), so a test run records which input mode NBN
		// was validated under and the frame its expected notes were computed in. The input
		// shift is the drop amount in Drop Pedal mode and 0 otherwise (Speaker Mode shifts the
		// song output, not the input).
		{
			const auto pitchMode = DropPedal::GetPitchMode();
			const int shift = DropPedal::GetShiftSemitones();
			response["dropPedal"] =
			{
				{ "configuredEnabled", DropPedal::IsConfiguredEnabled() },
				{ "pitchMode", DropPedal::GetPitchModeName() },
				{ "speakerMode", DropPedal::IsSpeakerModeEnabled() },
				{ "shiftSemitones", shift },
				{ "inputShiftSemitones",
					pitchMode == DropPedal::PitchMode::DropPedal ? shift : 0 },
				{ "routeName", DropPedal::GetPitchRouteName() }
			};
		}

		// Game-state hooks so automated navigation waits for the actual screen instead of
		// guessing with fixed delays: which menu/mode the game is in right now.
		response["gameState"] =
		{
			{ "gameLoaded", GameState::GameLoaded },
			{ "inSong", GameState::IsInSong() },
			{ "inSongModes", GameState::Menus::IsInSongModes() },
			{ "inLearnASong", GameState::Menus::IsInLearnASongModes() },
			{ "inLASPlaying", GameState::Menus::IsInLASPlayingModes() },
			{ "inLASPause", GameState::Menus::IsInLearnASongPauseModes() },
			{ "inRiffRepeaterMenus", GameState::Menus::IsInRiffRepeaterMenus() },
			{ "inPreSongTuner", GameState::Menus::IsInPreSongTuner() },
			{ "inTuning", GameState::Menus::IsInTuningMenus() },
			{ "inCalibration", GameState::Menus::IsInCalibrationMenus() },
			{ "lessonMode", GameState::LessonMode }
		};

		{
			nlohmann::json watches = nlohmann::json::array();
			std::lock_guard<std::mutex> lock(watchMutex);
			for (const auto& watch : memoryWatches)
			{
				watches.push_back({
					{ "id", watch.id },
					{ "label", watch.label },
					{ "address", static_cast<uint64_t>(watch.address) },
					{ "size", watch.size },
					{ "intervalMs", watch.intervalMilliseconds },
					{ "sampleCount", watch.sampleCount },
					{ "changeCount", watch.changeCount }
				});
			}
			response["memoryWatches"] = std::move(watches);
		}

		ResearchProtocol::NoteByNoteState state;
		if (ResearchBridge::TryGetNoteByNoteState(state))
		{
			response["state"] =
			{
				{ "initialized", state.isInitialized != 0 },
				{ "epochConfirmed", state.isEpochConfirmed != 0 },
				{ "ownsNativeHold", state.ownsNativeHold != 0 },
				{ "phase", GetGatePhaseName(state.gatePhase) },
				{ "trackedOwner", state.trackedOwner },
				{ "selectedRecord", state.selectedRecord },
				{ "epoch", state.epoch },
				{ "holdTickCount", state.holdTickCount },
				{ "lastUpdateTime", state.lastUpdateTime },
				{ "selectedRecordTime", state.selectedRecordTime },
				{ "selectedHoldTime", state.selectedHoldTime },
				{ "heldEpoch", state.heldEpoch },
				{ "selectedString", state.selectedString },
				{ "selectedFret", state.selectedFret },
				{ "selectedChordId", state.selectedChordId },
				{ "selectedChordNotesId", state.selectedChordNotesId },
				{ "expectedMidi", state.expectedMidi },
				{ "visualRecord", state.visualRecord },
				{ "visualString", state.visualString },
				{ "visualFret", state.visualFret },
				{ "visualChordId", state.visualChordId },
				{ "visualChordNotesId", state.visualChordNotesId }
			};

			// The current chord target's tones (physical MIDI), for the fake-guitar harness
			// to inject a matching synthetic strum. Empty for a single-note target.
			nlohmann::json chordTones = nlohmann::json::array();
			for (uint32_t i = 0; i < state.expectedChordToneCount
				&& i < ResearchProtocol::NoteByNoteState::MaxChordTones; ++i)
			{
				chordTones.push_back(state.expectedChordTones[i]);
			}
			response["state"]["expectedChordTones"] = std::move(chordTones);
			response["state"]["isBendTarget"] = state.isBendTarget != 0;
			response["state"]["bendAcceptMidi"] = state.bendAcceptMidi;
			response["state"]["soundingMidi"] = state.soundingMidi;
		}
		return response;
	}

	nlohmann::json HandleCommand(const nlohmann::json& request)
	{
		if (!request.contains("command") || !request["command"].is_string())
		{
			return { { "ok", false }, { "error", "A string command is required" } };
		}

		const auto command = request["command"].get<std::string>();
		if (command == "ping" || command == "status") return SerializeState();

		if (command == "events")
		{
			const auto after = request.value("after", static_cast<uint64_t>(0));
			const auto requestedMaximum = request.value("maximum", static_cast<size_t>(64));
			const auto maximum = (std::min)(requestedMaximum, static_cast<size_t>(64));
			nlohmann::json result = nlohmann::json::array();
			uint64_t latest = after;
			{
				std::lock_guard<std::mutex> lock(eventMutex);
				for (const auto& event : events)
				{
					if (event.sequence <= after) continue;
					result.push_back(event.value);
					latest = event.sequence;
					if (result.size() >= maximum) break;
				}
			}
			return { { "ok", true }, { "latest", latest }, { "events", result } };
		}

		if (command == "set_note_by_note_enabled")
		{
			if (!request.contains("enabled") || !request["enabled"].is_boolean())
			{
				return { { "ok", false }, { "error", "A boolean enabled value is required" } };
			}
			const auto enabled = request["enabled"].get<bool>();
			const auto changed = NoteByNoteProbe::SetAutomaticEnabled(enabled);
			if (!changed)
			{
				return { { "ok", false }, { "error", "Rocksmith rejected the requested Note by Note state" } };
			}
			return SerializeState();
		}

		// Synthetic-input test harness. Arms or disarms the fake guitar; when armed the
		// Player 1 cable is replaced by whatever notes inject_note / inject_chord queue.
		if (command == "set_fake_guitar")
		{
			if (!request.contains("enabled") || !request["enabled"].is_boolean())
			{
				return { { "ok", false }, { "error", "enabled must be a boolean" } };
			}
			if (!FakeGuitar::IsInstalled())
			{
				return { { "ok", false }, { "error", "The fake-guitar harness is not installed "
					"this session (Drop Pedal owns the input route, or this is not a Debug build)" } };
			}
			FakeGuitar::SetSynthEnabled(request["enabled"].get<bool>());
			return SerializeState();
		}

		// Hands-free autoplay: the host loop reads the frozen Note by Note target every tick
		// and injects the matching signal (note / chord / bend glide) with no external driver.
		if (command == "set_fake_guitar_autoplay")
		{
			if (!request.contains("enabled") || !request["enabled"].is_boolean())
			{
				return { { "ok", false }, { "error", "enabled must be a boolean" } };
			}
			if (!FakeGuitar::IsInstalled())
			{
				return { { "ok", false }, { "error", "The fake-guitar harness is not installed this session" } };
			}
			ResearchBridge::SetFakeGuitarAutoPlay(request["enabled"].get<bool>());
			return SerializeState();
		}

		// Queue one synthetic note (inject_note) or a strum of up to six pitches
		// (inject_chord). Arms the harness if it is idle, so a driver can inject in one call.
		if (command == "inject_note" || command == "inject_chord")
		{
			if (!FakeGuitar::IsInstalled())
			{
				return { { "ok", false }, { "error", "The fake-guitar harness is not installed "
					"this session (Drop Pedal owns the input route, or this is not a Debug build)" } };
			}

			const auto amplitude = std::clamp(request.value("amplitude", 0.6f), 0.0f, 1.0f);
			const auto durationMs = request.value("durationMs", static_cast<uint32_t>(400));
			const auto leadSilenceMs = request.value("leadSilenceMs", static_cast<uint32_t>(60));

			std::vector<int> midis;
			if (command == "inject_chord")
			{
				if (!request.contains("midis") || !request["midis"].is_array()
					|| request["midis"].empty())
				{
					return { { "ok", false }, { "error", "midis must be a non-empty array of MIDI numbers" } };
				}
				for (const auto& entry : request["midis"])
				{
					if (!entry.is_number_integer()) continue;
					midis.push_back(entry.get<int>());
				}
			}
			else
			{
				if (!request.contains("midi") || !request["midi"].is_number_integer())
				{
					return { { "ok", false }, { "error", "midi must be an integer MIDI number" } };
				}
				midis.push_back(request["midi"].get<int>());
			}

			if (midis.empty())
			{
				return { { "ok", false }, { "error", "no valid MIDI numbers were supplied" } };
			}

			if (!FakeGuitar::IsSynthEnabled()) FakeGuitar::SetSynthEnabled(true);
			const bool queued = FakeGuitar::QueueChord(
				midis.data(), static_cast<int>(midis.size()), amplitude, durationMs, leadSilenceMs);
			if (!queued)
			{
				return { { "ok", false }, { "error", "the inject queue is full; clear it or slow down" } };
			}

			nlohmann::json response = SerializeState();
			response["queuedMidis"] = midis;
			response["captureReady"] = FakeGuitar::IsCaptureReady();
			return response;
		}

		// Queue one synthetic bend through the injector's glide path (the same QueueBend the
		// autoplay driver uses): sound midi, then rise to endMidi and hold - the shape the
		// game's bend tracker requires. External drivers (the harness `walk`) use this for
		// bend targets instead of approximating with two separate flat injections.
		if (command == "inject_bend")
		{
			if (!FakeGuitar::IsInstalled())
			{
				return { { "ok", false }, { "error", "The fake-guitar harness is not installed "
					"this session (Drop Pedal owns the input route, or this is not a Debug build)" } };
			}
			if (!request.contains("midi") || !request["midi"].is_number_integer()
				|| !request.contains("endMidi") || !request["endMidi"].is_number_integer())
			{
				return { { "ok", false }, { "error", "midi and endMidi must be integer MIDI numbers" } };
			}
			const int baseMidi = request["midi"].get<int>();
			const int endMidi = request["endMidi"].get<int>();
			const auto amplitude = std::clamp(request.value("amplitude", 1.0f), 0.0f, 1.0f);
			const auto durationMs = request.value("durationMs", static_cast<uint32_t>(1100));
			const auto bendHoldMs = request.value("bendHoldMs", static_cast<uint32_t>(110));
			const auto bendRampMs = request.value("bendRampMs", static_cast<uint32_t>(200));

			if (!FakeGuitar::IsSynthEnabled()) FakeGuitar::SetSynthEnabled(true);
			if (!FakeGuitar::QueueBend(baseMidi, endMidi, amplitude, durationMs, bendHoldMs, bendRampMs))
			{
				return { { "ok", false }, { "error", "the inject queue is full; clear it or slow down" } };
			}

			nlohmann::json response = SerializeState();
			response["queuedBend"] = { { "midi", baseMidi }, { "endMidi", endMidi } };
			response["captureReady"] = FakeGuitar::IsCaptureReady();
			return response;
		}

		// Cycle the Drop Pedal pitch route Off -> Drop Pedal -> Speaker Mode -> Off, exactly as
		// the F7 hotkey does, so a test run can validate NBN under each input mode. Speaker Mode
		// transitions are refused during gameplay (set the mode from the menu first); the
		// returned state reports the mode actually reached.
		if (command == "cycle_pitch_mode")
		{
			if (!DropPedal::IsConfiguredEnabled())
			{
				return { { "ok", false }, { "error", "Drop Pedal is not enabled this session "
					"(EnableDropPedal=off in RSMods.ini); the pitch route cannot be cycled" } };
			}
			DropPedalInput::ToggleEnabled();
			return SerializeState();
		}

		// Load (or reload) the guitar sample bank so injected notes are real recordings. Point
		// it at a folder of note_<midi>.wav files; default is RSModsResearch\GuitarSamples.
		if (command == "load_guitar_samples")
		{
			if (!FakeGuitar::IsInstalled())
			{
				return { { "ok", false }, { "error", "The fake-guitar harness is not installed this session" } };
			}
			const std::string folder = request.value("folder", std::string());
			const int n = FakeGuitar::LoadSamples(folder.c_str());
			nlohmann::json response = SerializeState();
			response["samplesLoaded"] = n;
			return response;
		}

		// Drop the note now sounding and everything queued behind it.
		if (command == "inject_clear")
		{
			if (!FakeGuitar::IsInstalled())
			{
				return { { "ok", false }, { "error", "The fake-guitar harness is not installed this session" } };
			}
			FakeGuitar::ClearQueue();
			return SerializeState();
		}

		// The neck-diagram grid gate. Settable at runtime because its detour is installed at
		// startup and cannot be hot-reloaded, so compiling the mode in cost a main-DLL rebuild
		// and a game restart for every experiment on that surface.
		if (command == "set_grid_gate")
		{
			using Mode = NoteByNoteHighwayRenderer::GridGateMode;
			if (request.contains("mode") && request["mode"].is_string())
			{
				const auto requested = request["mode"].get<std::string>();
				Mode mode = Mode::Off;
				if (requested == "off") mode = Mode::Off;
				else if (requested == "restore") mode = Mode::Restore;
				else if (requested == "sentinel") mode = Mode::Sentinel;
				else
				{
					return { { "ok", false },
						{ "error", "mode must be off, restore or sentinel" } };
				}
				NoteByNoteHighwayRenderer::SetGridGateMode(mode);
			}

			// Optional, so the cleared-state words can be set once the (NBN GRID) line has
			// reported what an untouched cell actually holds.
			if (request.contains("sentinelValue") && request["sentinelValue"].is_number())
			{
				uint32_t existingValue = 0;
				float existingKey = 0.0f;
				NoteByNoteHighwayRenderer::GetGridSentinel(existingValue, existingKey);
				const auto value = request["sentinelValue"].get<uint32_t>();
				const auto key = request.contains("sentinelKey")
					&& request["sentinelKey"].is_number()
					? request["sentinelKey"].get<float>()
					: existingKey;
				NoteByNoteHighwayRenderer::SetGridSentinel(value, key);
			}

			return SerializeState();
		}

		// The upcoming-marker dim (v8). Runtime-toggleable for the colour-vs-double-marker
		// A/B (Philip 2026-08-25: upcoming notes gray while frozen is the top visual
		// complaint; the dim was the double-marker fix).
		if (command == "set_upcoming_dim")
		{
			if (!request.contains("enabled") || !request["enabled"].is_boolean())
			{
				return { { "ok", false }, { "error", "enabled must be a boolean" } };
			}
			NoteByNoteHighwayRenderer::SetUpcomingDimEnabled(request["enabled"].get<bool>());
			return SerializeState();
		}

		// Host-drawn finger numerals for chord holds. ON by default per test policy;
		// off is the A/B revert.
		if (command == "set_finger_numerals")
		{
			if (!request.contains("enabled") || !request["enabled"].is_boolean())
			{
				return { { "ok", false }, { "error", "enabled must be a boolean" } };
			}
			D3DHooks::SetFingerNumeralsEnabled(request["enabled"].get<bool>());
			return SerializeState();
		}

		// Per-feature overlay toggles (OverlayToggles): flip one overlay live so it can be
		// screenshotted or tested in isolation. `list_overlays` returns the current tags;
		// `set_overlay {name, on}` flips one. .ini "Overlay_<name>" is the persistent gate.
		if (command == "list_overlays")
		{
			return { { "ok", true }, { "overlays", OverlayToggles::List() } };
		}
		if (command == "set_overlay")
		{
			if (!request.contains("name") || !request["name"].is_string())
				return { { "ok", false }, { "error", "name must be a string" } };
			if (!request.contains("on") || !request["on"].is_boolean())
				return { { "ok", false }, { "error", "on must be a boolean" } };
			const std::string name = request["name"].get<std::string>();
			if (!OverlayToggles::Set(name, request["on"].get<bool>()))
				return { { "ok", false }, { "error", "unknown overlay tag: " + name },
						 { "overlays", OverlayToggles::List() } };
			return { { "ok", true }, { "overlays", OverlayToggles::List() } };
		}

		// The stale-marker quad filter (the doubles kill). ON by default per test policy;
		// off is the A/B revert.
		if (command == "set_stale_marker_filter")
		{
			if (!request.contains("enabled") || !request["enabled"].is_boolean())
			{
				return { { "ok", false }, { "error", "enabled must be a boolean" } };
			}
			D3DHooks::SetStaleMarkerFilterEnabled(request["enabled"].get<bool>());
			return SerializeState();
		}

		// The full-draw feed behind the probe's transition capture. ON by default; this is
		// the revert if the per-draw dispatch cost shows up in frame rate or ASIO health.
		if (command == "set_full_draw_feed")
		{
			if (!request.contains("enabled") || !request["enabled"].is_boolean())
			{
				return { { "ok", false }, { "error", "enabled must be a boolean" } };
			}
			isFullDrawFeedEnabled.store(
				request["enabled"].get<bool>(),
				std::memory_order_relaxed);
			return SerializeState();
		}

		// The native (filtered note-head) draw feed. Release default OFF; the captures that
		// consume it arm it themselves, this is the manual switch either way.
		// Menu row probe (issue #62): the UI thread enumerates the current Riff Repeater
		// screen's row container on its next poll; call again to read the result in status.
		if (command == "probe_menu_rows")
		{
			NoteByNoteMenu::RequestRowProbe();
			return SerializeState();
		}

		if (command == "set_native_draw_feed")
		{
			if (!request.contains("enabled") || !request["enabled"].is_boolean())
			{
				return { { "ok", false }, { "error", "enabled must be a boolean" } };
			}
			isNativeDrawFeedEnabled.store(
				request["enabled"].get<bool>(),
				std::memory_order_relaxed);
			return SerializeState();
		}

		// The highway gate. Same reasoning as the grid gate: the detours are installed at
		// startup, so without a runtime switch each experiment costs a rebuild and a restart.
		if (command == "set_highway")
		{
			using Mode = NoteByNoteHighwayRenderer::HighwayMode;
			if (!request.contains("mode") || !request["mode"].is_string())
			{
				return { { "ok", false },
					{ "error", "mode must be off, all or near-target" } };
			}
			const auto requested = request["mode"].get<std::string>();
			Mode mode = Mode::Off;
			if (requested == "off") mode = Mode::Off;
			else if (requested == "all") mode = Mode::All;
			else if (requested == "near-target") mode = Mode::NearTargetWindow;
			else
			{
				return { { "ok", false },
					{ "error", "mode must be off, all or near-target" } };
			}
			NoteByNoteHighwayRenderer::SetHighwayMode(mode);
			// Optional, so the window can be tuned by feel while playing.
			if (request.contains("windowSeconds") && request["windowSeconds"].is_number())
			{
				NoteByNoteHighwayRenderer::SetHighwayWindowSeconds(
					request["windowSeconds"].get<float>());
			}
			return SerializeState();
		}

		// Arms the per-draw render snapshot.
		//
		// This is the search for where the fretboard markers are actually drawn, after the
		// presentability predicate, the fretboard buffer, the highway draw and the near pool
		// were each eliminated by measurement. The snapshot records each note-head draw's
		// fingerprint and, critically, the native return address that issued it, which names
		// the function rather than leaving it to be guessed at.
		//
		// The capture now lives in the reloadable probe rather than the startup DLL, so what it
		// records and how it logs iterate without a game restart. The host only forwards the
		// draw it already filters and a per-frame tick; arming reaches the probe from here.
		if (command == "arm_render_snapshot")
		{
			ResearchBridge::ArmRenderSnapshot();
			return SerializeState();
		}

		if (command == "arm_physical_marker_draws")
		{
			D3DHooks::ArmPhysicalMarkerDrawCapture();
			return SerializeState();
		}

		// Causal draw-suppression test by runtime-selected mesh signature: while enabled,
		// draws matching the given stride/vertexCount/primitiveCount triple are skipped.
		// Defaults off; the toggle is per-run state and does not persist.
		if (command == "set_physical_guide_suppression")
		{
			if (!request.contains("enabled") || !request["enabled"].is_boolean())
			{
				return { { "ok", false }, { "error", "A boolean enabled value is required" } };
			}
			const bool enabled = request["enabled"].get<bool>();
			const uint32_t suppressStride = request.value("stride", 0u);
			const uint32_t suppressVertexCount = request.value("vertexCount", 0u);
			const uint32_t suppressPrimitiveCount = request.value("primitiveCount", 0u);
			if (enabled
				&& (suppressStride == 0
					|| suppressVertexCount == 0
					|| suppressPrimitiveCount == 0))
			{
				return { { "ok", false },
					{ "error", "Enabling requires nonzero stride, vertexCount and primitiveCount" } };
			}
			D3DHooks::SetPhysicalGuideSuppression(
				enabled,
				suppressStride,
				suppressVertexCount,
				suppressPrimitiveCount);
			return SerializeState();
		}

		// Arms the native note-list snapshot: the next call into the FUN_00BEBCD0 detour dumps
		// param_2's notes with their transform and time field, so a neck-placed upcoming note
		// can be distinguished from legitimate highway read-ahead. One-shot, probe-owned.
		if (command == "arm_note_draw_list")
		{
			ResearchBridge::ArmNoteDrawListSnapshot();
			return SerializeState();
		}

		// The neck-placement relocation at 0x7A8B10 (handover-5 addendum 12): the writer
		// that anchors a note visual's quads on the neck by (string, fret). Modes: off,
		// dry (log placements), target (relocate non-target placements off-screen during a
		// hold). Runtime-switchable because the detour is installed at startup.
		if (command == "set_neck_placement")
		{
			if (!request.contains("mode") || !request["mode"].is_string())
			{
				return { { "ok", false },
					{ "error", "mode must be off, dry, target or window" } };
			}
			const auto requested = request["mode"].get<std::string>();
			long mode = -1;
			if (requested == "off") mode = 0;
			else if (requested == "dry") mode = 1;
			else if (requested == "target") mode = 2;
			else if (requested == "window") mode = 3;
			else
			{
				return { { "ok", false },
					{ "error", "mode must be off, dry, target or window" } };
			}
			if (request.contains("sites") && request["sites"].is_number())
			{
				NoteByNoteHighwayRenderer::SetNeckPlacementSiteMask(
					request["sites"].get<long>());
			}
			if (request.contains("fadeDry") && request["fadeDry"].is_boolean()
				&& request["fadeDry"].get<bool>())
			{
				NoteByNoteHighwayRenderer::ArmFadeDryLog();
			}
			NoteByNoteHighwayRenderer::SetNeckPlacementMode(mode);
			return SerializeState();
		}


		// Arms the screen-map snapshot: for each note-head draw, the probe joins the note
		// pointer seen at the FUN_00BEBCD0 detour with the vertex-shader constants of the D3D
		// draw it issues, and projects the note position to screen coordinates. This is the
		// capture that tells the two fretboard markers apart from the ~138 highway notes.
		if (command == "arm_screen_map")
		{
			ResearchBridge::ArmScreenMapSnapshot();
			return SerializeState();
		}

		// Direct memory access, so most "what is in this struct" and "what does this flag do"
		// experiments cost a pipe command instead of a probe build and reload. Addresses are a
		// JSON number or a hex string; bytes travel as lowercase hex.
		if (command == "read_memory")
		{
			uintptr_t address = 0;
			if (!request.contains("address") || !TryParseAddress(request["address"], address))
			{
				return { { "ok", false }, { "error", "A numeric or hex-string address is required" } };
			}
			const auto size = request.value("size", static_cast<size_t>(64));
			if (size == 0 || size > MAX_MEMORY_READ_BYTES)
			{
				return { { "ok", false }, { "error", "size must be between 1 and "
					+ std::to_string(MAX_MEMORY_READ_BYTES) } };
			}
			std::vector<uint8_t> bytes;
			if (!TryReadGameMemory(address, size, bytes))
			{
				return { { "ok", false }, { "error", "The address range is not readable" } };
			}
			return {
				{ "ok", true },
				{ "address", static_cast<uint64_t>(address) },
				{ "size", size },
				{ "bytes", ToHex(bytes.data(), bytes.size()) }
			};
		}

		// Writes through page protection (code patches included) and returns the previous
		// bytes, so any experiment can be reverted by writing them back.
		if (command == "write_memory")
		{
			uintptr_t address = 0;
			if (!request.contains("address") || !TryParseAddress(request["address"], address))
			{
				return { { "ok", false }, { "error", "A numeric or hex-string address is required" } };
			}
			if (!request.contains("bytes") || !request["bytes"].is_string())
			{
				return { { "ok", false }, { "error", "A hex string of bytes is required" } };
			}
			std::vector<uint8_t> bytes;
			if (!TryParseHexBytes(request["bytes"].get<std::string>(), bytes)
				|| bytes.size() > MAX_MEMORY_WRITE_BYTES)
			{
				return { { "ok", false }, { "error", "bytes must be an even-length hex string of"
					" at most " + std::to_string(MAX_MEMORY_WRITE_BYTES) + " bytes" } };
			}

			std::vector<uint8_t> previous;
			if (!TryReadGameMemory(address, bytes.size(), previous))
			{
				return { { "ok", false }, { "error", "The address range is not readable" } };
			}

			DWORD oldProtection = 0;
			if (!VirtualProtect(
				reinterpret_cast<LPVOID>(address),
				bytes.size(),
				PAGE_EXECUTE_READWRITE,
				&oldProtection))
			{
				return { { "ok", false }, { "error", "VirtualProtect failed with error "
					+ std::to_string(GetLastError()) } };
			}
			SIZE_T bytesWritten = 0;
			const bool didWrite = WriteProcessMemory(
				GetCurrentProcess(),
				reinterpret_cast<LPVOID>(address),
				bytes.data(),
				bytes.size(),
				&bytesWritten) != FALSE
				&& bytesWritten == bytes.size();
			DWORD restoredProtection = 0;
			VirtualProtect(
				reinterpret_cast<LPVOID>(address),
				bytes.size(),
				oldProtection,
				&restoredProtection);
			FlushInstructionCache(
				GetCurrentProcess(),
				reinterpret_cast<LPCVOID>(address),
				bytes.size());
			if (!didWrite)
			{
				return { { "ok", false }, { "error", "The write did not complete" } };
			}
			return {
				{ "ok", true },
				{ "address", static_cast<uint64_t>(address) },
				{ "size", bytes.size() },
				{ "previous", ToHex(previous.data(), previous.size()) }
			};
		}

		// Arms a change-triggered sampler on an address range; results arrive on the
		// existing events stream as "memory-watch" entries.
		if (command == "watch_memory")
		{
			uintptr_t address = 0;
			if (!request.contains("address") || !TryParseAddress(request["address"], address))
			{
				return { { "ok", false }, { "error", "A numeric or hex-string address is required" } };
			}
			const auto size = request.value("size", static_cast<size_t>(4));
			if (size == 0 || size > MAX_MEMORY_WATCH_BYTES)
			{
				return { { "ok", false }, { "error", "size must be between 1 and "
					+ std::to_string(MAX_MEMORY_WATCH_BYTES) } };
			}
			const auto requestedInterval = request.value("intervalMs", static_cast<uint32_t>(100));
			const auto interval = (std::min)(
				(std::max)(requestedInterval, static_cast<uint32_t>(10)),
				static_cast<uint32_t>(10000));

			MemoryWatch watch;
			watch.address = address;
			watch.size = size;
			watch.intervalMilliseconds = interval;
			watch.label = request.value("label", std::string());
			{
				std::lock_guard<std::mutex> lock(watchMutex);
				if (memoryWatches.size() >= MAX_MEMORY_WATCHES)
				{
					return { { "ok", false }, { "error", "The watch list is full; unwatch first" } };
				}
				watch.id = nextWatchId++;
				memoryWatches.push_back(std::move(watch));
			}
			return SerializeState();
		}

		if (command == "unwatch_memory")
		{
			const auto removeAll = request.value("all", false);
			uint64_t id = 0;
			if (!removeAll)
			{
				if (!request.contains("id") || !request["id"].is_number_unsigned())
				{
					return { { "ok", false }, { "error", "A watch id (or all: true) is required" } };
				}
				id = request["id"].get<uint64_t>();
			}
			size_t removedCount = 0;
			{
				std::lock_guard<std::mutex> lock(watchMutex);
				const auto originalSize = memoryWatches.size();
				memoryWatches.erase(
					std::remove_if(
						memoryWatches.begin(),
						memoryWatches.end(),
						[&](const MemoryWatch& watch)
						{
							return removeAll || watch.id == id;
						}),
					memoryWatches.end());
				removedCount = originalSize - memoryWatches.size();
			}
			if (removedCount == 0 && !removeAll)
			{
				return { { "ok", false }, { "error", "No watch has id " + std::to_string(id) } };
			}
			return SerializeState();
		}

		// Any probe_* command is the probe's to answer. This keeps future captures at probe
		// rebuild + reload cost: a new verb needs no bridge change and therefore no restart.
		if (command.rfind("probe_", 0) == 0)
		{
			std::string response;
			if (!ResearchBridge::TryHandleProbeCommand(request.dump(), response))
			{
				return { { "ok", false },
					{ "error", "The probe did not accept the command: " + command } };
			}
			try
			{
				return nlohmann::json::parse(response);
			}
			catch (const std::exception&)
			{
				return { { "ok", false },
					{ "error", "The probe returned malformed JSON for " + command } };
			}
		}

		if (command == "reload_probe")
		{
			const auto source = request.contains("path")
				? std::filesystem::path(request["path"].get<std::string>())
				: GetDefaultProbePath();
			// Debug-iteration convenience: preserve the player's NBN enable state across the
			// reload so they do not have to re-press N every iteration. The reload gate
			// requires NBN off (the scoring controller must not be swapped mid-hold), so
			// capture the state, disable for the safe swap, reload, then re-arm - bypassing
			// the Riff-Repeater-menu gate because the player already passed it. The transient
			// disable here does NOT persist (only the N-key writes the debug-state file), so
			// the player's true intent is untouched. `reload` alone now suffices; a separate
			// `disable` first would defeat this by clearing the state before we capture it.
			const bool wasAutomaticEnabled = NoteByNoteProbe::IsAutomaticEnabled();
			if (wasAutomaticEnabled) NoteByNoteProbe::SetAutomaticEnabled(false);
			std::string error;
			const bool reloaded = ReloadProbe(source, error);
			if (wasAutomaticEnabled) NoteByNoteProbe::ForceRestoreAutomatic();
			if (!reloaded) return { { "ok", false }, { "error", error } };
			return SerializeState();
		}

		// Tier-0 raw-tone measurement, callable ad hoc from the driver scripts: energy at an
		// exact frequency (or a MIDI note's equal-tempered frequency) and its +/-1/+/-2
		// semitone neighbours over the last `window` seconds of the Player 1 route.
		if (command == "raw_tone")
		{
			double frequency = request.value("frequency", 0.0);
			if (frequency <= 0.0 && request.contains("midi") && request["midi"].is_number())
			{
				frequency = 440.0 * std::pow(2.0, (request["midi"].get<double>() - 69.0) / 12.0);
			}
			const float window = request.value("window", 0.15f);
			RawPitchVerifier::ToneEvidence evidence;
			if (!RawPitchVerifier::QueryToneEvidence(frequency, window, evidence))
			{
				return { { "ok", false },
					{ "error", "No route audio observed yet, or the frequency is invalid" } };
			}
			return { { "ok", true },
				{ "frequencyHz", frequency },
				{ "sampleRate", evidence.sampleRate },
				{ "windowSamples", evidence.windowSampleCount },
				{ "rms", evidence.totalRms },
				{ "target", evidence.targetPower },
				{ "minusOne", evidence.minusOnePower },
				{ "plusOne", evidence.plusOnePower },
				{ "minusTwo", evidence.minusTwoPower },
				{ "plusTwo", evidence.plusTwoPower } };
		}

		// Tier-1 ML pitch observation, for eyeballing the companion service from the
		// driver scripts: the latest mailbox estimate (observed route frame) and whether
		// the service counts as alive (updated within the last second of audio). ok is
		// true even with no estimate - the absence is the observation.
		if (command == "ml_pitch")
		{
			float midi = 0.0f;
			float confidence = 0.0f;
			double ageSeconds = 0.0;
			const bool hasResult = MlAudioExporter::QueryResult(midi, confidence, ageSeconds);
			nlohmann::json response = { { "ok", true },
				{ "alive", MlAudioExporter::IsServiceAlive() } };
			if (hasResult)
			{
				response["midi"] = midi;
				response["confidence"] = confidence;
				response["ageSeconds"] = ageSeconds;
			}
			return response;
		}

		if (command == "unload_probe")
		{
			std::string error;
			if (!UnloadProbe(error)) return { { "ok", false }, { "error", error } };
			return SerializeState();
		}

		return { { "ok", false }, { "error", "Unknown command: " + command } };
	}

	uint32_t __cdecl HandleDebugRequest(const char* request, char* response, uint32_t capacity)
	{
		try
		{
			const auto result = HandleCommand(nlohmann::json::parse(request)).dump();
			if (result.size() >= capacity) return 0;
			std::memcpy(response, result.c_str(), result.size() + 1);
			return static_cast<uint32_t>(result.size());
		}
		catch (const std::exception& exception)
		{
			const auto result = nlohmann::json({ { "ok", false }, { "error", exception.what() } }).dump();
			if (result.size() >= capacity) return 0;
			std::memcpy(response, result.c_str(), result.size() + 1);
			return static_cast<uint32_t>(result.size());
		}
	}
}

void ResearchBridge::Initialize()
{
	isStopping = false;
	// Mirror host log lines onto the event stream while the bridge is up. The host services and the
	// in-process controller both run without the bridge in a shipping build; this only adds the
	// research telemetry side on top.

	std::string error;
	// The scoring engine ships inside this module, so the controller comes up in-process at startup.
	// A research session can still hot-swap a reloadable probe DLL over it through the reload_probe
	// pipe verb.
	if (!LoadInProcessProbe(error))
	{
		LOG_ERROR("(RESEARCH BRIDGE) In-process Note by Note controller failed to load: "
			<< error << "." << std::endl);
	}

	const DebugHostApi api = { 1, sizeof(DebugHostApi), &HandleDebugRequest };
	debugToolsActive.store(DebugToolsLoader::Start(api));
	if (debugToolsActive.load())
	{
		NoteByNoteHostServices::SetLogTelemetrySink(&MirrorHostLogToEvents);
		watchThread = std::thread(RunMemoryWatcher);
		LOG_INFO("(DEBUG TOOLS) Verified private debug DLL activated the local bridge." << std::endl);
	}
	else
	{
		isNativeDrawFeedEnabled.store(false);
		isFullDrawFeedEnabled.store(false);
		LOG_INFO("(DEBUG TOOLS) No active debug DLL. Gameplay runs without developer endpoints." << std::endl);
	}
}

void ResearchBridge::Shutdown()
{
	isStopping = true;
	DebugToolsLoader::Stop();
	debugToolsActive.store(false);
	if (watchThread.joinable()) watchThread.join();
	NoteByNoteHostServices::SetLogTelemetrySink(nullptr);

	std::string error;
	if (!UnloadProbe(error) && !error.empty())
	{
		LOG_ERROR("(RESEARCH BRIDGE) Probe shutdown was rejected: " << error << "." << std::endl);
	}
}

bool ResearchBridge::IsProbeLoaded()
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	return loadedProbe.module != nullptr;
}

void ResearchBridge::DispatchGenericHook(
	uint32_t slotId,
	const ResearchProtocol::HookContext* context)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module == nullptr || loadedProbe.api.ObserveGenericHook == nullptr) return;
	loadedProbe.api.ObserveGenericHook(slotId, context);
}

void ResearchBridge::DispatchNeckPlacementStep(uint32_t site, void* stepContext)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module == nullptr
		|| loadedProbe.api.ObserveNeckPlacementStep == nullptr)
	{
		return;
	}
	loadedProbe.api.ObserveNeckPlacementStep(site, stepContext);
}

void ResearchBridge::DispatchScoringUpdate(
	void* owner,
	float updateTime,
	ResearchProtocol::ScoringUpdate original)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module == nullptr)
	{
		original(owner, updateTime);
		return;
	}
	loadedProbe.api.ProcessScoringUpdate(owner, updateTime, original);
}

void ResearchBridge::DispatchHitDecision(
	void* owner,
	void* unusedEdx,
	void* note,
	ResearchProtocol::HitDecision original,
	bool& result)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	result = loadedProbe.module == nullptr
		? original(owner, unusedEdx, note)
		: loadedProbe.api.ProcessHitDecision(owner, unusedEdx, note, original);
}

void ResearchBridge::DispatchRenderedAttack(
	const NoteByNoteProbe::NativeRenderedAttack& attack)
{
	std::vector<ResearchProtocol::RenderedNote> notes;
	notes.reserve(attack.notes.size());
	for (const auto& source : attack.notes)
	{
		notes.push_back({ source.stringIndex, source.fret });
	}

	const ResearchProtocol::RenderedAttack researchAttack =
	{
		attack.isTransition ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0),
		attack.renderFrame,
		attack.songTime,
		attack.longitudinalPosition,
		notes.data(),
		notes.size()
	};

	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module != nullptr) loadedProbe.api.ObserveRenderedAttack(&researchAttack);
}

void ResearchBridge::DispatchStop()
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module != nullptr) loadedProbe.api.Stop();
}

void ResearchBridge::DispatchRequestReArm()
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	// Optional ProbeApi entry: a probe built before the seam leaves it null, so the
	// re-arm is simply skipped there rather than faulting.
	if (loadedProbe.module != nullptr && loadedProbe.api.RequestReArm != nullptr)
	{
		loadedProbe.api.RequestReArm();
	}
}

void ResearchBridge::ArmRenderSnapshot()
{
	// The render snapshot consumes ObserveNativeDraw, so arming it switches the feed on.
	isNativeDrawFeedEnabled.store(true, std::memory_order_relaxed);
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module != nullptr) loadedProbe.api.ArmRenderSnapshot();
}

void ResearchBridge::DispatchNativeDraw(const ResearchProtocol::NativeDrawObservation& draw)
{
	// Same fast path as DispatchFullDraw: no probe, no mutex.
	if (!hasNativeDrawConsumer.load(std::memory_order_acquire)) return;
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module != nullptr) loadedProbe.api.ObserveNativeDraw(&draw);
}

void ResearchBridge::DispatchFullDraw(const ResearchProtocol::NativeDrawObservation& draw)
{
	if (!hasFullDrawConsumer.load(std::memory_order_acquire)) return;
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module == nullptr || loadedProbe.api.ObserveFullDraw == nullptr) return;
	loadedProbe.api.ObserveFullDraw(&draw);
}

bool ResearchBridge::IsFullDrawFeedEnabled()
{
	return isFullDrawFeedEnabled.load(std::memory_order_relaxed);
}

bool ResearchBridge::IsNativeDrawFeedEnabled()
{
	return isNativeDrawFeedEnabled.load(std::memory_order_relaxed);
}

bool ResearchBridge::IsDrawFeedWanted()
{
	return (isFullDrawFeedEnabled.load(std::memory_order_relaxed)
			&& hasFullDrawConsumer.load(std::memory_order_relaxed))
		|| (isNativeDrawFeedEnabled.load(std::memory_order_relaxed)
			&& hasNativeDrawConsumer.load(std::memory_order_relaxed));
}

void ResearchBridge::DispatchRenderFrameComplete(uint64_t renderFrame)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module != nullptr) loadedProbe.api.NotifyRenderFrameComplete(renderFrame);
}

void ResearchBridge::ArmNoteDrawListSnapshot()
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module != nullptr) loadedProbe.api.ArmNoteDrawListSnapshot();
}

void ResearchBridge::ArmScreenMapSnapshot()
{
	// The screen map consumes ObserveNativeDraw, so arming it switches the feed on.
	isNativeDrawFeedEnabled.store(true, std::memory_order_relaxed);
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module != nullptr) loadedProbe.api.ArmScreenMapSnapshot();
}

bool ResearchBridge::TryHandleProbeCommand(const std::string& requestJson, std::string& response)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module == nullptr) return false;

	std::vector<char> buffer(64 * 1024, '\0');
	if (loadedProbe.api.HandleProbeCommand(
		requestJson.c_str(),
		buffer.data(),
		static_cast<uint32_t>(buffer.size())) == 0)
	{
		return false;
	}
	buffer.back() = '\0';
	response = buffer.data();
	return true;
}

void ResearchBridge::ProcessNoteDrawList(
	void* renderCtx,
	void* unusedEdx,
	int* noteArray,
	ResearchProtocol::NoteHeadDraw original)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module == nullptr)
	{
		original(renderCtx, unusedEdx, noteArray);
		return;
	}

	loadedProbe.api.ObserveNoteDrawList(renderCtx, noteArray);
	const bool wasPrepared = loadedProbe.api.PrepareNoteDrawList(renderCtx, noteArray) != 0;
	original(renderCtx, unusedEdx, noteArray);
	if (wasPrepared) loadedProbe.api.CompleteNoteDrawList();
}

bool ResearchBridge::TryGetNoteByNoteState(ResearchProtocol::NoteByNoteState& state)
{
	std::lock_guard<std::recursive_mutex> lock(probeMutex);
	if (loadedProbe.module == nullptr) return false;
	state = {};
	return loadedProbe.api.GetState(&state) != 0;
}

void ResearchBridge::SetFakeGuitarAutoPlay(bool enabled)
{
	fakeGuitarAutoPlay.store(enabled, std::memory_order_release);
	autoPlayLastRecord = 0;
	if (enabled) FakeGuitar::SetSynthEnabled(true);
}

bool ResearchBridge::IsFakeGuitarAutoPlay()
{
	return fakeGuitarAutoPlay.load(std::memory_order_acquire);
}

// Called every host-loop tick. When autoplay is on and Note by Note is frozen waiting on a
// note, inject the matching signal into the fake guitar - a bend glide, a chord strum, or a
// single tone - once per note, re-injecting only if it went silent before the note accepted.
void ResearchBridge::PollFakeGuitarAutoPlay()
{
	if (!fakeGuitarAutoPlay.load(std::memory_order_acquire)) return;
	if (!FakeGuitar::IsInstalled()) return;

	ResearchProtocol::NoteByNoteState state;
	if (!TryGetNoteByNoteState(state)) return;

	// Only inject while the freeze is actively holding a target.
	if (state.gatePhase != ResearchProtocol::GatePhase::Holding || state.selectedRecord == 0)
	{
		autoPlayLastRecord = 0;
		autoPlayBendPeak = -1.0f;
		return;
	}

	const bool newNote = state.selectedRecord != autoPlayLastRecord;
	if (newNote) autoPlayBendPeak = -1.0f;
	// Every tick: track how high the detector actually read this bend, so a short attempt is
	// corrected rather than repeated.
	if (state.isBendTarget && state.soundingMidi > autoPlayBendPeak)
		autoPlayBendPeak = state.soundingMidi;

	const bool silent = FakeGuitar::CurrentMidi() < 0 && FakeGuitar::PendingCount() == 0;
	if (!newNote && !silent) return;   // the current note is still sounding
	autoPlayLastRecord = state.selectedRecord;

	// A retry (same note went silent without accepting) gets a short release-gap first, so the
	// re-pick reads as a fresh attack. Notes are kept short: the game only advances after the
	// input releases, so a shorter note ends the hold sooner - the whole loop runs faster.
	const uint32_t leadSilence = newNote ? 0u : 90u;

	if (state.isBendTarget && state.bendAcceptMidi > 0
		&& state.bendAcceptMidi != state.expectedMidi && state.expectedMidi > 0)
	{
		// Glide the fretted note up to the bend accept pitch and hold there. Closed loop: if
		// the previous attempt on this same bend read short (the detector never got near the
		// target), bend higher by the shortfall so it reaches this time.
		int endMidi = state.bendAcceptMidi;
		if (!newNote && autoPlayBendPeak >= 0.0f
			&& autoPlayBendPeak < static_cast<float>(state.bendAcceptMidi) - 0.25f)
		{
			endMidi += static_cast<int>(
				static_cast<float>(state.bendAcceptMidi) - autoPlayBendPeak + 0.999f);
		}
		FakeGuitar::QueueBend(state.expectedMidi, endMidi, 1.0f, 1100, 110, 200);
	}
	else if (state.selectedChordId >= 0 && state.expectedChordToneCount >= 2)
	{
		FakeGuitar::QueueChord(state.expectedChordTones,
			static_cast<int>(state.expectedChordToneCount), 1.0f, 650, leadSilence);
	}
	else if (state.expectedMidi > 0)
	{
		FakeGuitar::QueueNote(state.expectedMidi, 1.0f, 520, leadSilence);
	}
}

void ResearchBridge::PublishPhysicalMarkerDraw(const PhysicalMarkerDrawEvent& event)
{
	nlohmann::json transform = nlohmann::json::array();
	for (size_t row = 0; row < 4; ++row)
	{
		transform.push_back({
			event.transform[row][0],
			event.transform[row][1],
			event.transform[row][2],
			event.transform[row][3]
		});
	}

	nlohmann::json instances = nlohmann::json::array();
	for (uint32_t index = 0; index < event.decodedInstanceCount && index < 32; ++index)
	{
		instances.push_back({
			event.instanceTranslations[index][0],
			event.instanceTranslations[index][1],
			event.instanceTranslations[index][2]
		});
	}

	PublishEvent({
		{ "type", "physical-marker-draw" },
		{ "renderFrame", event.renderFrame },
		{ "caller", event.nativeCaller },
		{ "stream", event.streamIdentity },
		{ "texture", event.textureIdentity },
		{ "vertexShader", event.vertexShaderIdentity },
		{ "pixelShader", event.pixelShaderIdentity },
		{ "primitiveType", event.primitiveType },
		{ "baseVertexIndex", event.baseVertexIndex },
		{ "minimumVertexIndex", event.minimumVertexIndex },
		{ "vertexCount", event.vertexCount },
		{ "startIndex", event.startIndex },
		{ "primitiveCount", event.primitiveCount },
		{ "stride", event.stride },
		{ "string", event.stringIndex },
		{ "fret", event.fret },
		{ "targetString", event.targetString },
		{ "targetFret", event.targetFret },
		{ "decoded", event.decoded },
		{ "decodeFailure", event.decodeFailure == nullptr ? "" : event.decodeFailure },
		{ "instanced", event.instanced },
		{ "streamFrequency", event.streamFrequency },
		{ "transformStreamStride", event.transformStreamStride },
		{ "instanceCount", event.instanceCount },
		{ "instances", std::move(instances) },
		{ "userPointer", event.userPointer },
		{ "vertexSampleStatus", event.vertexSampleStatus },
		{ "hasBounds", event.hasBounds },
		{ "boundsMin", { event.boundsMin[0], event.boundsMin[1], event.boundsMin[2] } },
		{ "boundsMax", { event.boundsMax[0], event.boundsMax[1], event.boundsMax[2] } },
		{ "vertexSample", [&event] {
			nlohmann::json sample = nlohmann::json::array();
			for (uint32_t index = 0; index < event.vertexSampleCount && index < 24; ++index)
			{
				sample.push_back(event.vertexSample[index]);
			}
			return sample;
		}() },
		{ "transform", std::move(transform) }
	});
}

void ResearchBridge::PublishNoteByNoteEvent(
	const NoteByNoteProbe::NativeExpectedAttackEvent& event)
{
	PublishEvent({
		{ "type", "note-by-note" },
		{ "event", GetExpectedEventName(event.kind) },
		{ "epoch", event.epoch },
		{ "owner", event.ownerAddress },
		{ "updateTime", event.updateTime },
		{ "record", event.note.recordAddress },
		{ "authoredTime", event.note.authoredTime },
		{ "nativeEventTime", event.note.nativeEventTime },
		{ "string", event.note.stringIndex },
		{ "fret", event.note.fret },
		{ "chordId", event.note.chordId },
		{ "stateC0", event.note.stateC0 },
		{ "stateC1", event.note.stateC1 },
		{ "stateC2", event.note.stateC2 },
		{ "stateC3", event.note.stateC3 }
	});
}
