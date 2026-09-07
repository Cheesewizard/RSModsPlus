#include "stdafx.h"
#include "Main.hpp"
#include "Mods/NoteByNoteNativeScoring.hpp"
#include "Research/ResearchBridge.hpp"
#include "Audio/MlServiceLauncher.hpp"

#if defined(_DEBUG) || defined(_WWISE_LOGS)
bool debug = true;
#else
bool debug = false;
#endif

#ifdef _WWISE_LOGS
bool wwiseLogging = true;
#else
bool wwiseLogging = false;
#endif

#ifndef _RSMODS_VERSION
#define _RSMODS_VERSION "RSModsPlus 4 (based on RSMods 1.2.8.2). DEBUG: " << std::boolalpha << debug << ". Wwise Logs: " << std::boolalpha << wwiseLogging << "."
#endif

/// <summary>
/// Handle Force Enumeration
/// </summary>
/// <returns>NULL. Loops while game is open.</returns>
unsigned WINAPI EnumerationThread() {
	while (!GameState::GameLoaded)
		Sleep(5000);

	int oldDLCCount = Enumeration::GetCurrentDLCCount();
	int newDLCCount = oldDLCCount;

	while (!GameState::GameClosing) {
		if (Settings::ReturnSettingValue("ForceReEnumerationEnabled") == "automatic") {
			oldDLCCount = newDLCCount;
			newDLCCount = Enumeration::GetCurrentDLCCount();

			if (oldDLCCount != newDLCCount)
				Enumeration::ForceEnumeration();
		}

		Sleep(Settings::GetModSetting("CheckForNewSongsInterval"));
	}

	return 0;
}

/// <summary>
/// Send Midi Data Async. Only really used in debug builds to test MIDI commands.
/// Secondary purpose of remaking D3D textures every 32 ticks (~ 1 second).
/// </summary>
/// <returns>NULL. Loops while game is open.</returns>
unsigned WINAPI MidiThread() {
	// Initial some values.
	int currentCount = 0;

	while (!GameState::GameClosing) {
		// If this is the 32nd loop, remake the D3D textures.
		// This allows us to have real-time updates to textures.
		if (currentCount == 31) {
			currentCount = 0;
			D3DHooks::RecreateTextureTimer = true;
		}

		// If we have sent a Midi PC/CC value to this thread, send the Midi value.
		if (Midi::sendPC)
			Midi::SendProgramChange(Midi::dataToSendPC);

		if (Midi::sendCC)
			Midi::SendControlChange(Midi::dataToSendCC);

		Sleep(Midi::sleepFor);
		currentCount++;
	}

	return 0;
}

unsigned WINAPI RiffRepeaterThread() {
	// Wait for the game to enter the main menu before attempting to read the current song info, in order to prevent crashes
	while (!GameState::GameLoaded)
		Sleep(5000);

	std::string previousSongKey = "";

	// We can only user Riff Repeater while the game is open, so verify it's open. Runs every 100 ms.
	while (!GameState::GameClosing) {
		Sleep(100);

		const auto songKey = GameState::GetSongKey();
		if (songKey != previousSongKey) {
			previousSongKey = songKey;

			RiffRepeater::HandleSongChange(previousSongKey);
		}

		RiffRepeater::SaveSpeedToFileOnChange();

		// Retire Note by Note when the player leaves the song / Riff Repeater context, so it
		// does not persist onto results, song select, or the main menu (per-song lifecycle).
		NoteByNoteProbe::TickLifecycle();
	}

	return 0;
}

const bool ensureForcedTopMode = false;

/// <summary>
/// Handle Keypress Inputs. Used to toggle mods on / off. WARNING: RUNS (almost) EVERY FRAME
/// </summary>
/// <param name="hWnd"> - ID of Rocksmith</param>
/// <param name="msg"> - Reason Function was called. KEYUP = Keypress | COPYDATA = Settings Update | CLOSE = Game Closing.</param>
/// <param name="keyPressed"> - Virtual Key of the key pressed. Reference here: https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes </param>
/// <param name="lParam"> - Data Sent</param>
/// <returns>Verification that message was sent.</returns>
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM keyPressed, LPARAM lParam) {
	if (Menu::menuEnabled && ImGui_ImplWin32_WndProcHandler(hWnd, msg, keyPressed, lParam))
		return true;

	if (Settings::ReturnSettingValue("PreventMidSongPause") == "on" && D3DHooks::cachedIsInSong) {
		switch (msg) {
			case WM_NCACTIVATE:
			case WM_ACTIVATEAPP:
			case WM_ACTIVATE:
				return CallWindowProc(D3DHooks::oWndProc, hWnd, msg, TRUE, lParam);
			case WM_KILLFOCUS:
				return false;
		}
	}

	switch (msg) {
		case WM_SYSCOMMAND:
			// Makes ALT + ENTER cause F11 to be pressed.
			// This is mainly so a user can use a common shortcut, that works in most games now-a-days.
			if (keyPressed == SC_KEYMENU && lParam == VK_RETURN) {
				WndProc(hWnd, WM_KEYUP, VK_F11, 0);
				return true;
			}

			// Prevent a weird bug when trying to play Guitarcade or Score Attack with a key combination.
			if (keyPressed == SC_MOVE + 0x2 && GameState::Menus::IsInOnlineModes()) {
				return true;
			}

			break;
		case WM_KEYUP:
		#if defined(_DEBUG)
			if (NoteByNoteProbe::HandleKeyUp(static_cast<uint32_t>(keyPressed))) return 0;
		#endif
			Keybindings::HandleKeyUp(keyPressed);
			break;
		case WM_KEYDOWN:
			Keybindings::HandleKeyDown(keyPressed);
			break;
		case WM_CLOSE:
			GameState::GameClosing = true;
			break;
		case WM_COPYDATA:
			Keybindings::UpdateSettingsOnGUIChange(lParam);
			break;

		default:
			POINT mPos;
			GetCursorPos(&mPos);
			ScreenToClient(hWnd, &mPos);
			ImGui::GetIO().MousePos.x = mPos.x;
			ImGui::GetIO().MousePos.y = mPos.y;
			break;
	}

	return CallWindowProc(D3DHooks::oWndProc, hWnd, msg, keyPressed, lParam);
}

void UpdateGameWindowStacking() {
	if (Settings::ReturnSettingValue("PreventMidSongPause") == "on") {
		bool actuallyInSong = GameState::IsInSong();

		if (ensureForcedTopMode) {
			static bool lastState = false;
			if (actuallyInSong != lastState) {
				HWND position = actuallyInSong ? HWND_TOPMOST : HWND_NOTOPMOST;

				SetWindowPos(D3DHooks::GetGameWindow(), position, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

				lastState = actuallyInSong;
			}
		}
	
		D3DHooks::cachedIsInSong = actuallyInSong;
	}
}

/// <summary>
/// Hook on game boot. Initialize all our own UI elements (text on screen, and ImGUI).
/// </summary>
/// <param name="pDevice"> - Device Pointer</param>
/// <returns>HRESULT of the official EndScene</returns>
HRESULT APIENTRY D3DHooks::Hook_EndScene(IDirect3DDevice9* pDevice) {
	HRESULT originalReturn = oEndScene(pDevice);
	if (Menu::IsOverlayCall()) {
		return originalReturn;
	}
	D3DHooks::FinishNoteByNoteRenderFrame();

	Menu::Init(pDevice, (LONG_PTR)WndProc);
	Menu::RenderImGuiMenu();
	Menu::UpdateStringTextures(pDevice);
	UpdateGameWindowStacking();
	GameOverlay::RenderOverlay(pDevice);
	D3DHooks::RegenerateTwitchNoteColors(pDevice);
	
	return originalReturn;
}

/// <summary>
/// Manage Queue of Twitch Effects.
/// </summary>
/// <returns>NULL. Loops while game is open.</returns>
unsigned WINAPI HandleEffectQueueThread() {
	while (!GameState::GameClosing) {
		if (Twitch::effectQueue.empty() && GameState::IsInSong()) {
			Twitch::ParseEffectQueue();
		}

		Sleep(250);
	}
	return 0;
}

/// <summary>
/// Main Thread where we trigger the mods to startup.
/// </summary>
/// <returns>NULL. Loops while game is open.</returns>
unsigned WINAPI MainThread() {
	LOG_NOHEAD(_RSMODS_VERSION << std::endl);

	GameLoopState loopState = {};

	Keybindings::InitializeCommands();
	ModManager::InitializeConfiguration();
	ModManager::InitializeMods(debug);
	ModManager::ApplyStartupMods();

	// Note by Note is a FEATURE and must initialize in every configuration: the
	// _DEBUG gate here (in place since the 2026-08-14 checkpoint) shipped a Release
	// build whose Riff Repeater menu showed the NOTE BY NOTE entry but with a dead
	// rocker and broken enumeration, because the menu item renders unconditionally
	// while its backing state never initialized (beta blocker, found 2026-09-01).
	// The probe host lives inside ResearchBridge, so the bridge initializes in
	// Release too for now; splitting the research-only control surface out of
	// shipping builds is the RSMODS_RESEARCH separation planned in
	// docs/designs/release-debug-suite-separation.md.
	ResearchBridge::Initialize();
	NoteByNoteProbe::Initialize();
	NoteByNoteMenu::Initialize();
	NoteByNoteHudLabel::Initialize();

	// Spawn the FretNet ML string/fret companion once, bound to this game's lifetime.
	// It is what Note-by-Note reads for its ML "stuck hold" rescue; making it a default
	// removes the manual start step. The service uses the main-folder managed library.
	MlServiceLauncher::EnsureStarted();

	while (!GameState::GameClosing) {
		Sleep(250);
		MlServiceLauncher::EnsureStarted();

		if (GameState::GameLoaded) {
			ModManager::HandlePostGameLoadedMods(loopState);
		}
		else {
			ModManager::UpdateGameLoadingState(loopState);
		}
	}

	MlServiceLauncher::Shutdown();
	ResearchBridge::Shutdown();

	return 0;
}

/// <summary>
/// Unhook all threads to take advantage of multi-threading.
/// </summary>
void Initialize() {
	LogSettings::startupTime = clock();

	Wwise::Exports::Initialize();

	// Read before any thread is spawned. Every mod thread reads these maps, so
	// rebuilding them later frees the strings a reader is still holding.
	Settings::ReadKeyBinds();
	Settings::ReadModSettings();

	std::thread(MainThread).detach(); // Mod Toggle based on menus
	std::thread(EnumerationThread).detach(); // Force Enumeration
	std::thread(HandleEffectQueueThread).detach(); // Twitch Effects
	std::thread(MidiThread).detach(); // MIDI Auto Tuning / True Tuning
	std::thread(RiffRepeaterThread).detach(); // RR Speed Above 100% Log
}

void SetupLogging() {
	bool debugLogPresent = std::ifstream("RSMods_debug.txt").good();

	// Keep the previous launch's evidence. The game truncates audiodump.txt when its audio
	// initialises (after this DLL loads) and we truncate RSMods_debug.txt below, so a
	// "no sound / no cable" launch used to erase the very log that explained it.
	if (debugLogPresent) CopyFileA("RSMods_debug.txt", "RSMods_debug.previous.txt", FALSE);
	CopyFileA("audiodump.txt", "audiodump.previous.txt", FALSE);

	auto clearDebugLog = std::ofstream("RSMods_debug.txt");

	FILE* streamRead;
	FILE* streamConsole;

	if (debug) {
		AllocConsole();

		// Quick-edit mode freezes the WHOLE GAME on a stray click: selecting text
		// in the console blocks WriteFile on stdout, and the log writes are
		// synchronous on the main thread (live incident 2026-08-25 - the game
		// "hung" mid-session until the console selection was cleared). Selection
		// stays available through right-click > Mark.
		HANDLE consoleInput = GetStdHandle(STD_INPUT_HANDLE);
		DWORD consoleMode = 0;
		if (consoleInput != INVALID_HANDLE_VALUE && GetConsoleMode(consoleInput, &consoleMode)) {
			SetConsoleMode(consoleInput, (consoleMode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
		}

		// Connect stdin, stdout to the debug console.
		freopen_s(&streamRead, "CONIN$", "r", stdin);
		freopen_s(&streamConsole, "CONOUT$", "w", stdout);
	}

	// Create log file to both help with debugging release builds,
	// and allow the user to examine their debug logs after a crash.
	if (debugLogPresent) {
		// Clear log so it isn't full of junk from the last launch
		clearDebugLog.open("RSMods_debug.txt", std::ofstream::out | std::ofstream::trunc);
		clearDebugLog.close();

		// freopen (not freopen_s): the _s variant opens with _SH_SECURE, which for a write
		// stream denies every other reader for the life of the process, so the log could not
		// be read while the game ran. Plain freopen shares read/write, so
		// `Get-Content RSMods_debug.txt -Wait` tails it live.
#pragma warning(suppress: 4996)
		freopen("RSMods_debug.txt", "w", stderr);
	}
}

/// <summary>
/// Hook into the game for us to run our own code. **DISPLAYS DEBUG CONSOLE ON DEBUG BUILD**
/// </summary>
/// <param name="hModule"></param>
/// <param name="dwReason"></param>
/// <param name="lpReserved"></param>
/// <returns>Always returns TRUE</returns>
BOOL APIENTRY DllMain(HMODULE hModule, uint32_t dwReason, LPVOID lpReserved) {
	switch (dwReason) {
		case DLL_PROCESS_ATTACH:
			SetupLogging();
			DisableThreadLibraryCalls(hModule); // Disables the DLL_THREAD_ATTACH and DLL_THREAD_DETACH notifications. | https://docs.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-disablethreadlibrarycalls
			Proxy::Init(); // Proxy all real XInput commands to the actual xinput1_3.dll.
			Initialize(); // Inject our mod code.
			return TRUE;
		case DLL_PROCESS_DETACH:
			Proxy::Shutdown(); // Kill Proxy to xinput1_3.dll

			if (Menu::ImGuiInit)
			{
				ImGui_ImplWin32_Shutdown();
				ImGui_ImplDX9_Shutdown();
				ImGui::DestroyContext();
			}
			return TRUE;
		default:
			// Should never happen, but handle gracefully
			break;
	}
	
	return TRUE;
}
