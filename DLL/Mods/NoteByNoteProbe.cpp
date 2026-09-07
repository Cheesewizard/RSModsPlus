#include "../stdafx.h"
#include "NoteByNoteProbe.hpp"

#include "NoteByNoteNativeScoring.hpp"
#include "../D3D/NoteByNoteHighwayRenderer.hpp"
#include "NoteByNoteController.hpp"
#include "../Settings.hpp"


namespace
{
	constexpr uint32_t AUTOMATIC_KEY = 'N';

	// with a synthesized tone the injection commands drive, so Note by Note can be


	// key, because alt-tabbing to the bridge pauses the game and loses the
	// moment. J forwards the same probe command the bridge's native-seek-test
	// verb sends; the probe performs one main-thread idle-gated call.
	constexpr uint32_t LEAD_KEY = '1';
	constexpr uint32_t RHYTHM_KEY = '2';
	constexpr uint32_t BASS_KEY = '3';

	std::mutex controllerMutex;
	bool isAutomaticEnabled = false;
	// Lock-free mirror of isAutomaticEnabled for hot callers that must NOT take controllerMutex
	// (the localized-string resolver detour runs on the UI thread for every string, and the
	// N-key toggle resolves strings while holding controllerMutex, so a mutex read there would
	// self-deadlock). Written alongside every isAutomaticEnabled assignment; read by
	// IsAutomaticEnabledFast(). See NoteByNoteHudLabel.cpp.
	std::atomic<bool> automaticEnabledSnapshot{ false };
	// J-press feedback for the overlay: press time + whether the probe accepted,
	// so the player sees the request register without reading the console.
	std::chrono::steady_clock::time_point lastSeekRequestFlashAt{};
	bool wasLastSeekRequestAccepted = false;
	std::string selectedArrangement = "lead";
	std::string automaticTargetText;

	bool AreProbeModifiersPressed()
	{
		return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
			&& (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	}

	// Host-local copy of the probe controller's chord-identity read. The
	// authoritative TryDescribeChordTarget lives in NoteByNoteNativeScoring.cpp,
	// which only the PROBE compiles; this overlay is host code and cannot link
	// it, so the ~30 lines are duplicated rather than growing the protocol.

	// note+0x30 -> SNG chord template, stride 0x48: mask u32, frets[6] (< 0x1A
	// playable), fingers[6], notes[6] i32, name char[32] at +0x28.
	struct HostChordTemplateView
	{
		uint32_t mask;
		uint8_t frets[6];
		uint8_t fingers[6];
		int32_t notes[6];
		char name[32];
	};
	static_assert(sizeof(HostChordTemplateView) == 0x48, "SNG chord template stride is 0x48");

	template <typename T>
	bool TryReadGame(uintptr_t address, T& value)
	{
		if (address == 0) return false;
		MEMORY_BASIC_INFORMATION memory = {};
		if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) == 0) return false;
		if (memory.State != MEM_COMMIT
			|| (memory.Protect & PAGE_GUARD) != 0
			|| (memory.Protect & PAGE_NOACCESS) != 0)
		{
			return false;
		}
		const auto regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
		if (address > regionEnd || sizeof(T) > regionEnd - address) return false;
		value = *reinterpret_cast<const T*>(address);
		return true;
	}

	bool TryDescribeChordTargetHost(uintptr_t noteAddress, char* buffer, size_t bufferLength)
	{
		if (buffer == nullptr || bufferLength == 0) return false;
		buffer[0] = '\0';
		uintptr_t templateAddress = 0;
		if (!TryReadGame(noteAddress + 0x30, templateAddress) || templateAddress == 0) return false;
		HostChordTemplateView view = {};
		if (!TryReadGame(templateAddress, view)) return false;

		char name[sizeof(view.name) + 1] = {};
		for (size_t i = 0; i < sizeof(view.name) && view.name[i] != '\0'; ++i)
		{
			if (view.name[i] < 0x20 || view.name[i] > 0x7E) { name[0] = '\0'; break; }
			name[i] = view.name[i];
		}

		char frets[24] = {};
		size_t at = 0;
		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			const uint8_t fret = view.frets[stringIndex];
			char fretText[4];
			if (fret < 0x1A) std::snprintf(fretText, sizeof(fretText), "%u", fret);
			else { fretText[0] = 'x'; fretText[1] = '\0'; }
			at += std::snprintf(frets + at, sizeof(frets) - at, "%s%s",
				stringIndex == 0 ? "" : "/", fretText);
			if (at >= sizeof(frets)) break;
		}


		// fingering panels paint the WRONG chord's numbers during frozen chord holds
		// and are suppressed there, so this line is where the player reads which
		// fingers to use. Same template, fingers[6] parallel to frets[6].
		char fingers[24] = {};
		at = 0;
		bool hasFingering = false;
		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			const uint8_t fret = view.frets[stringIndex];
			const uint8_t finger = view.fingers[stringIndex];
			char fingerText[4];
			if (fret < 0x1A && finger >= 1 && finger <= 4)
			{
				std::snprintf(fingerText, sizeof(fingerText), "%u", finger);
				hasFingering = true;
			}
			else { fingerText[0] = 'x'; fingerText[1] = '\0'; }
			at += std::snprintf(fingers + at, sizeof(fingers) - at, "%s%s",
				stringIndex == 0 ? "" : "/", fingerText);
			if (at >= sizeof(fingers)) break;
		}

		if (name[0] != '\0')
		{
			std::snprintf(buffer, bufferLength, hasFingering
				? "%s [%s] fingers [%s]" : "%s [%s]", name, frets, fingers);
		}
		else
		{
			std::snprintf(buffer, bufferLength, hasFingering
				? "[%s] fingers [%s]" : "[%s]", frets, fingers);
		}
		return true;
	}

	// The same template read as per-string frets (playable fret or -1), for the
	// stale-marker quad filter's chord keep set, plus the parallel fingers (1..4,
	// 0 when the template gives none) for the host-drawn finger numerals.
	bool TryReadChordTemplateFretsHost(
		uintptr_t noteAddress,
		int (&frets)[6],
		int (&fingers)[6])
	{
		uintptr_t templateAddress = 0;
		if (!TryReadGame(noteAddress + 0x30, templateAddress) || templateAddress == 0)
		{
			return false;
		}
		HostChordTemplateView view = {};
		if (!TryReadGame(templateAddress, view)) return false;
		for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
		{
			const bool fretted = view.frets[stringIndex] < 0x1A;
			frets[stringIndex] = fretted
				? static_cast<int>(view.frets[stringIndex])
				: -1;
			fingers[stringIndex] = fretted
				&& view.fingers[stringIndex] >= 1 && view.fingers[stringIndex] <= 4
				? static_cast<int>(view.fingers[stringIndex])
				: 0;
		}
		return true;
	}

	void DisableAutomatic()
	{
		{
			std::lock_guard<std::mutex> lock(controllerMutex);
			if (!isAutomaticEnabled) return;
			isAutomaticEnabled = false;
			automaticEnabledSnapshot.store(false, std::memory_order_relaxed);
			automaticTargetText.clear();
		}

		NoteByNoteNativeScoring::Stop();
		// Remembered dim-candidate visuals must not survive a disable: the next enable
		// (often the far side of a probe reload) would hand the synthetic dim a pointer
		// whose object may have been rebuilt in the meantime.
		NoteByNoteHighwayRenderer::EvictRememberedDimCandidates();
		LOG_INFO("(NBN AUTO) Disabled." << std::endl);
	}

	// Detection strategy is no longer a user setting. Note by Note ships as one tuned blend
	// (native + tier-0 + ML), which is the probe's compile-time default, so the host pushes
	// nothing on enable. The per-technique NativeOnly/MlOnly overrides remain INTERNAL, driven

	// for testing and isolating a regression. See docs/designs/nbn-detection-blend.md.

	bool ToggleAutomaticInternal()
	{
		bool didEnable = false;
		{
			std::lock_guard<std::mutex> lock(controllerMutex);
			if (!isAutomaticEnabled)
			{
				if (!NoteByNoteNativeScoring::IsAvailable())
				{
					LOG_ERROR("(NBN AUTO) Start ignored because the authoritative native controller is unavailable."
						<< std::endl);
					return false;
				}
				if (!GameState::Menus::IsInRiffRepeaterMenus())
				{
					LOG_ERROR("(NBN AUTO) Start ignored because Note by Note can only be enabled from the Riff Repeater menu."
						<< std::endl);
					return false;
				}

				isAutomaticEnabled = true;
				automaticEnabledSnapshot.store(true, std::memory_order_relaxed);
				automaticTargetText = "Waiting for Rocksmith's native section epoch";
				LOG_INFO("(NBN AUTO) Enabled for " << selectedArrangement
					<< ". Rocksmith's live scoring vector is the only attack iterator; waiting for its native section-entry rollback."
					<< std::endl);

				// the mode instead of depending on a bridge command each launch. It is
				// inert outside an owned hold, so nothing changes if the toggle is bounced.
				NoteByNoteHighwayRenderer::SetNeckPlacementMode(3);
				didEnable = true;
			}
		}

		if (didEnable)
		{
			// Re-arm the controller for the section the player is about to enter. The
			// enable is edge-driven and off the scoring loop, so a same-owner section
			// change (enable in one Riff Repeater section, cycle to another, re-enable)
			// otherwise leaves stale confirmed bounds and never arms. Called after the
			// host lock is released: the dispatch reaches the probe's controller lock,
			// which the scoring thread already holds while reading IsNoteByNoteEnabled
			// under this host lock, so taking both here in the other order could deadlock.
			NoteByNoteNativeScoring::RequestReArm();
			return true;
		}

		DisableAutomatic();
		return true;
	}

	// Debug-iteration persistence. Physically playing to test means every probe
	// hot-reload otherwise costs an N-press plus re-navigation; persisting the player's
	// NBN enable intent lets a `reload` bring NBN straight back on. Only the N-key path
	// writes this (the user's real intent), so the reload workflow's transient `disable`
	// - which goes through SetAutomaticEnabled, not the N-key - cannot clobber the saved
	// value. It also survives a full game restart (the file is on disk), which doubles as
	// the sticky-enabled behavior. Deliberately NOT a whole-controller serialization: the
	// live section/hold state and its owner pointers must re-bootstrap fresh, never be
	// revived stale across a reload.
	std::string GetStatePath()
	{
		// This code compiles into the HOST (xinput1_3.dll), which sits at the game root, so

		// the trace log - regardless of the process working directory. (An earlier version
		// wrongly reused the probe's two-levels-up logic, which put the file above the game
		// folder where nothing could read it.)
		std::string path = "RSMods\\nbn-state.json";
		char modulePath[MAX_PATH] = {};
		HMODULE module = nullptr;
		if (GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
					| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&GetStatePath), &module)
			&& GetModuleFileNameA(module, modulePath, MAX_PATH) != 0)
		{
			std::string full(modulePath);
			const auto lastSlash = full.find_last_of('\\');
			if (lastSlash != std::string::npos)
			{
				path = full.substr(0, lastSlash + 1) + "RSMods\\nbn-state.json";
			}
		}
		return path;
	}

	void PersistAutomaticState(bool automaticEnabled)
	{
		try
		{
			nlohmann::json state = { { "automaticEnabled", automaticEnabled } };
			std::ofstream file(GetStatePath(), std::ios::trunc);
			if (file) file << state.dump();
		}
		catch (...)
		{
			// A debug convenience must never be able to destabilize the probe.
		}
	}

	bool RestoreAutomaticEnabled()
	{
		try
		{
			std::ifstream file(GetStatePath());
			if (!file) return false;
			nlohmann::json state;
			file >> state;
			return state.value("automaticEnabled", false);
		}
		catch (...)
		{
			return false;
		}
	}

	void SelectArrangement(const std::string& arrangement)
	{
		std::lock_guard<std::mutex> lock(controllerMutex);
		selectedArrangement = arrangement;
		LOG_INFO("(NBN AUTO) Selected " << arrangement
			<< ". The native SessionMode arrangement remains authoritative." << std::endl);
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

	const char* GetStringColour(uint8_t stringIndex)
	{
		static const char* STRING_COLOURS[] =
		{
			"Red",
			"Yellow",
			"Blue",
			"Orange",
			"Green",
			"Purple"
		};
		if (stringIndex >= sizeof(STRING_COLOURS) / sizeof(STRING_COLOURS[0])) return "Unknown";
		return STRING_COLOURS[stringIndex];
	}

	std::string RenderedAttackText(const NoteByNoteProbe::NativeRenderedAttack& attack)
	{
		std::ostringstream stream;
		for (size_t index = 0; index < attack.notes.size(); ++index)
		{
			if (index != 0) stream << ',';
			stream << attack.notes[index].stringIndex << ':' << attack.notes[index].fret;
		}
		return stream.str();
	}
}

void NoteByNoteProbe::HandleNativeControllerFault(const std::string& reason)
{
	LOG_ERROR("(NBN AUTO) Authoritative native controller fault: " << reason
		<< ". Note by Note has been disabled." << std::endl);
	DisableAutomatic();
}

void NoteByNoteProbe::HandleNativeRenderedAttack(const NativeRenderedAttack& attack)
{
	NoteByNoteNativeScoring::ObserveRenderedAttack(attack);
}

void NoteByNoteProbe::HandleNativeExpectedAttackEvent(const NativeExpectedAttackEvent& event)
{
	NoteByNoteController::PublishNoteByNoteEvent(event);

	NativeRenderedAttack renderedAttack = {};
	bool isEnabled = false;
	{
		std::lock_guard<std::mutex> lock(controllerMutex);
		isEnabled = isAutomaticEnabled;
		if (!isEnabled) return;
		if (event.kind == NativeExpectedAttackEventKind::CandidateChanged
			|| event.kind == NativeExpectedAttackEventKind::HoldEstablished)
		{
			std::ostringstream target;
			if (event.note.stringIndex == 0xFF)
			{
				char chordIdentity[64];
				if (TryDescribeChordTargetHost(
					event.note.nativeNoteAddress, chordIdentity, sizeof(chordIdentity)))
				{
					target << "Target: " << chordIdentity << " chord";
				}
				else
				{
					target << "Target: chord (strum)";
				}
				// Feed the same template's per-string frets to the stale-marker quad
				// filter, which otherwise sits dormant during chord holds and lets
				// future-note markers paint. Fingers ride along for the host-drawn
				// numerals (the game's own numeral glyphs are screen-space and never

				int chordFrets[6];
				int chordFingers[6];
				if (TryReadChordTemplateFretsHost(
					event.note.nativeNoteAddress, chordFrets, chordFingers))
				{
					NoteByNoteHighwayRenderer::SetChordTargetShape(
						event.note.chordId, chordFrets, chordFingers);
				}
			}
			else
			{
				target << "Target: " << GetStringColour(event.note.stringIndex)
					<< " string - fret " << event.note.fret;
				if (event.note.bendSemitones > 0)
				{
					target << " - bend +" << event.note.bendSemitones
						<< (event.note.bendSemitones == 1 ? " (half)"
							: event.note.bendSemitones == 2 ? " (full)" : "");
				}
				else if (event.note.bendSemitones < 0)
				{
					target << " - bend";
				}
			}
			automaticTargetText = target.str();
		}
	}

	const auto& note = event.note;
	LOG_INFO("(NBN NATIVE EXPECTED) stage=" << (event.isAfterUpdate ? "post" : "pre")
		<< " event=" << GetExpectedEventName(event.kind)
		<< " epoch=" << event.epoch
		<< " owner=0x" << std::hex << event.ownerAddress
		<< " note=0x" << note.nativeNoteAddress
		<< " record=0x" << note.recordAddress
		<< " mask=0x" << note.noteMask
		<< " flags=0x" << note.noteFlags
		<< " hash=0x" << note.noteHash << std::dec
		<< " updateTime=" << std::fixed << std::setprecision(6) << event.updateTime
		<< " authoredTime=" << note.authoredTime
		<< " nativeEventTime=" << note.nativeEventTime
		<< " rangeA4=" << note.rangeA4
		<< " rangeA8=" << note.rangeA8
		<< " rangeB0=" << note.rangeB0
		<< " string=" << note.stringIndex
		<< " fret=" << note.fret
		<< " chord=" << note.chordId
		<< " chordNotes=" << note.chordNotesId
		<< " phraseIteration=" << note.phraseIterationId
		<< " states=" << static_cast<int>(note.state50)
		<< static_cast<int>(note.state51)
		<< static_cast<int>(note.state52)
		<< '/' << static_cast<int>(note.stateC0)
		<< static_cast<int>(note.stateC1)
		<< static_cast<int>(note.stateC2)
		<< static_cast<int>(note.stateC3)
		<< '/' << static_cast<int>(note.state144)
		<< " previousRecord=0x" << std::hex
		<< (event.hasPreviousNote ? event.previousNote.recordAddress : 0) << std::dec
		<< " previousAuthoredTime=" << (event.hasPreviousNote
			? event.previousNote.authoredTime
			: 0.0f)
		<< " notePresent=" << std::boolalpha << event.isNotePresent
		<< " playableRange=" << event.isInPlayableRange
		<< " lastHit=" << event.wasLastNoteHit
		<< "." << std::endl);
}

bool NoteByNoteProbe::HandleKeyUp(uint32_t keyPressed)
{
#if !defined(_DEBUG)

	// Ctrl+Shift+1/2/3 (arrangement intent) are debug-only. In release the feature is
	// toggled exclusively from the in-game Riff Repeater menu slider (NoteByNoteMenu),

	// "restrict pressing N shortcut for debug build only").
	(void)keyPressed;
	return false;
#else
	if (keyPressed == AUTOMATIC_KEY
		&& (IsAutomaticEnabled() || GameState::Menus::IsInRiffRepeaterMenus()))
	{
		ToggleAutomaticInternal();
		// Persist the user's real intent so the next hot-reload restores it. Only this
		// N-key path writes it; the reload workflow's transient disable does not, so a
		// disable-before-reload cannot clobber the saved state.
		PersistAutomaticState(IsAutomaticEnabled());
		return true;
	}


	// recommended test state - N off, section playing - and with no feedback he
	// could not tell whether a press registered). The real safety gates live
	// downstream: the probe only fires while the controller is idle and the
	// owner vtable-checks as GamePlaysongLAS.
	if (!AreProbeModifiersPressed()) return false;
	switch (keyPressed)
	{
		case LEAD_KEY:
			SelectArrangement("lead");
			return true;
		case RHYTHM_KEY:
			SelectArrangement("rhythm");
			return true;
		case BASS_KEY:
			SelectArrangement("bass");
			return true;
		default:
			return false;
	}
#endif
}

void NoteByNoteProbe::Initialize()
{
	NoteByNoteNativeScoring::Initialize();
	if (NoteByNoteNativeScoring::IsAvailable())
	{
		LOG_INFO("(NBN PROBE) Authoritative native Note by Note controller ready. Enable it from Riff Repeater; N toggles it, and Ctrl+Shift+1/2/3 select Lead/Rhythm/Bass menu intent."
			<< std::endl);
		// Restore the player's last NBN enable intent across the hot-reload so iteration
		// does not cost an N-press each time. The enable side-effects are replicated
		// directly (target text + previous-target dim), bypassing ToggleAutomaticInternal's
		// Riff-Repeater-menu gate because the user already passed it - we are only
		// re-arming the state they had, not enabling anew. Inert outside an owned hold, so
		// restoring while not in a section changes nothing until play resumes.
		if (RestoreAutomaticEnabled())
		{
			ForceRestoreAutomatic();
			LOG_INFO("(NBN PROBE) Restored NBN=ON from the saved state file across the"
				<< " game restart; no N-press needed." << std::endl);
		}
	}
	else
	{
		LOG_ERROR("(NBN PROBE) Authoritative native Note by Note controller is unavailable."
			<< std::endl);
	}
}

void NoteByNoteProbe::ForceRestoreAutomatic()
{
	{
		std::lock_guard<std::mutex> lock(controllerMutex);
		isAutomaticEnabled = true;
		automaticEnabledSnapshot.store(true, std::memory_order_relaxed);
		automaticTargetText = "Waiting for Rocksmith's native section epoch";
		// Same enable side-effect ToggleAutomaticInternal applies (previous-target dim).
		NoteByNoteHighwayRenderer::SetNeckPlacementMode(3);
	}
	// Re-arm for the current section, matching the N-toggle enable path. Outside the
	// host lock for the same lock-ordering reason (see ToggleAutomaticInternal).
	NoteByNoteNativeScoring::RequestReArm();
}

bool NoteByNoteProbe::IsAutomaticEnabled()
{
	std::lock_guard<std::mutex> lock(controllerMutex);
	return isAutomaticEnabled;
}

// Lock-free read of the enabled state for hot paths that must never take controllerMutex
// (see the automaticEnabledSnapshot note above). Eventually-consistent with
// IsAutomaticEnabled(): a toggle updates the atomic under the lock, so a reader sees the
// new value within one relaxed store.
bool NoteByNoteProbe::IsAutomaticEnabledFast()
{
	return automaticEnabledSnapshot.load(std::memory_order_relaxed);
}

bool NoteByNoteProbe::IsFreezeActive()
{
	if (!IsAutomaticEnabled()) return false;
	const bool practicing = GameState::Menus::IsInSongModes()
		&& !GameState::Menus::IsInRiffRepeaterMenus();

	// Log each menu-state transition so the exact practice-vs-preview strings are verifiable
	// from the console (throttled to transitions only).
	static std::string lastLoggedMenu;
	if (GameState::currentMenu != lastLoggedMenu)
	{
		lastLoggedMenu = GameState::currentMenu;
		LOG_INFO("(NBN LIFECYCLE) menu=" << GameState::currentMenu
			<< " practicing=" << practicing
			<< " -> freeze " << (practicing ? "ALLOWED" : "blocked") << "." << std::endl);
	}
	return practicing;
}

void NoteByNoteProbe::TickLifecycle()
{
	static uint32_t outOfSongTicks = 0;
	if (!IsAutomaticEnabled() || GameState::Menus::IsInSongModes())
	{
		outOfSongTicks = 0;
		return;
	}
	// ~1.5s at a 100ms cadence: too long for a momentary dialog to trip, short enough that the
	// overlay retires promptly on a real exit.
	constexpr uint32_t RETIRE_AFTER_TICKS = 15;
	if (++outOfSongTicks >= RETIRE_AFTER_TICKS)
	{
		LOG_INFO("(NBN LIFECYCLE) Sustained absence from the song/RR context (menu="
			<< GameState::currentMenu << ", " << outOfSongTicks
			<< " ticks); retiring Note by Note." << std::endl);
		outOfSongTicks = 0;
		DisableAutomatic();
	}
}

bool NoteByNoteProbe::ToggleAutomatic()
{
	return ToggleAutomaticInternal();
}

bool NoteByNoteProbe::SetAutomaticEnabled(bool shouldEnable)
{
	if (IsAutomaticEnabled() == shouldEnable) return true;
	return ToggleAutomaticInternal();
}

bool NoteByNoteProbe::TryGetSeekRequestFlash(double& secondsAgo, bool& accepted)
{
	std::lock_guard<std::mutex> lock(controllerMutex);
	if (lastSeekRequestFlashAt == std::chrono::steady_clock::time_point{}) return false;
	secondsAgo = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - lastSeekRequestFlashAt).count();
	accepted = wasLastSeekRequestAccepted;
	return true;
}

std::string NoteByNoteProbe::GetAutomaticTargetText()
{
	std::lock_guard<std::mutex> lock(controllerMutex);
	return automaticTargetText;
}
