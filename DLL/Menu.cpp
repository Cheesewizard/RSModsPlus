#include "stdafx.h"
#include "Menu.hpp"
#include "Mods/AudioDevices.hpp"
#include "Mods/VoiceOverControl.hpp"
#include "Mods/VolumeControl.hpp"
#include "Mods/Midi.hpp"
#include "Audio/SharedOutput.hpp"
#include "Audio/CableInput.hpp"
#include "Research/ResearchBridge.hpp"
#include "GameState.hpp"
#include "Keybindings.hpp"
#include <filesystem>
#include <cmath>
#include <shellapi.h>

namespace Menu {
	void GenerateTestingTextures(IDirect3DDevice9* pDevice) {
		// **DEPRECATED** Generate solid color textures. Useful for testing
		D3D::GenerateSolidTexture(pDevice, &Red, D3DCOLOR_ARGB(255, 000, 255, 255));
		D3D::GenerateSolidTexture(pDevice, &Green, D3DCOLOR_ARGB(255, 0, 255, 0));
		D3D::GenerateSolidTexture(pDevice, &Blue, D3DCOLOR_ARGB(255, 0, 0, 255));
		D3D::GenerateSolidTexture(pDevice, &Yellow, D3DCOLOR_ARGB(255, 255, 255, 0));

		// **DEPRECATED** Generate texture from dds file.
		D3DXCreateTextureFromFile(pDevice, L"notes_gradient_normal.dds", &gradientTextureNormal); //if those don't exist, note heads will be "invisible" | 6-String Model
		D3DXCreateTextureFromFile(pDevice, L"notes_gradient_seven.dds", &gradientTextureSeven); // 7-String Note Colors
		D3DXCreateTextureFromFile(pDevice, L"gradient_map_additive.dds", &additiveNoteTexture); // Note Stems
	}

	/// <summary>
	/// Checks if the EndScene call originates from an overlay (e.g., Steam)
	/// instead of the game to prevent rendering our UI twice.
	/// </summary>
	/// <returns>True if the call is from an overlay, false otherwise.</returns>
	bool IsOverlayCall() {
		return (uint32_t)_ReturnAddress() > Offsets::baseEnd.Get();
	}

	/// <summary>
	/// Names the real device the game is playing out to in passthrough (direct) mode, so the overlay can show
	/// it instead of a generic label. This reads the exact same source the desktop bridge reports from: the
	/// RS_ASIO.ini [Asio.Output] Driver line (see GUI AsioProxySetup.ReadDriver). RS_ASIO binds the game's
	/// output to that driver, so it IS the current output. If the ini points at our proxy, resolve the wrapped
	/// hardware driver from HKCU\Software\RSMods\AsioProxy\Target (the value the GUI stores) so we name what
	/// actually plays. Returns empty on any miss, and the caller falls back to the old generic text.
	/// </summary>
	static std::wstring ReadPassthroughOutputName() {
		wchar_t executable[MAX_PATH]{};
		GetModuleFileNameW(nullptr, executable, MAX_PATH);
		const auto ini = std::filesystem::path(executable).parent_path() / L"RS_ASIO.ini";

		wchar_t driver[256]{};
		GetPrivateProfileStringW(L"Asio.Output", L"Driver", L"", driver, 256, ini.c_str());
		std::wstring name = driver;

		// If RS_ASIO plays through our proxy, the real device is the one HKCU says the proxy forwards to.
		if (_wcsicmp(name.c_str(), L"Rocksmith Audio Bridge") == 0) {
			HKEY key{};
			if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RSMods\\AsioProxy", 0,
				KEY_READ | KEY_WOW64_32KEY, &key) == ERROR_SUCCESS) {
				wchar_t target[256]{}; DWORD size = sizeof(target); DWORD type = 0;
				if (RegQueryValueExW(key, L"Target", nullptr, &type, reinterpret_cast<LPBYTE>(target), &size) == ERROR_SUCCESS
					&& type == REG_SZ && target[0])
					name = target;
				RegCloseKey(key);
			}
		}
		return name;
	}

	/// <summary>
	/// Renders the ImGui frame, including the main mod menu.
	/// </summary>
	void RenderImGuiMenu() {
		// Rocksmith hides the OS cursor in-song, so draw a software cursor while a panel is open (only then,
		// so gameplay is untouched otherwise). This is what makes the overlay clickable over the game.
		ImGui::GetIO().MouseDrawCursor = Menu::menuEnabled || Menu::audioBridgeMenuEnabled;

		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		if (Menu::menuEnabled) {
			ImGui::Begin("RS Mods");
			Menu::AddMidiMenu();
			Menu::AddCalibrationMenu();
			Menu::AddMicrophonesMenu();
			Menu::AddVoicelinesMenu();
			ImGui::End();
		}

		if (Menu::audioBridgeMenuEnabled)
			Menu::AddAudioBridgeMenu();

		ImGui::EndFrame();
		ImGui::Render();
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

		// NB: the old CaptureKeyboardFromApp(false)/CaptureMouseFromApp(false) calls were removed here. They
		// force-cleared io.WantCapture* every frame, which made the WndProc unable to tell when a panel wanted
		// the input, so clicks/keys always fell through to the game. The WndProc now honours WantCaptureMouse /
		// WantCaptureKeyboard so panel input is swallowed while ImGui owns it (see dllmain.cpp WndProc).
	}

	// ---- In-game Audio Bridge overlay -------------------------------------------------------------
	// Drives the same audio engine the desktop bridge does, in-process via SharedOutput::DispatchControl
	// (no pipe). Operation codes match the control protocol the desktop app sends over the pipe. This is the
	// mid-song control surface that replaces the separate bridge window over exclusive-fullscreen Rocksmith.

	// Mixer buses in control-channel order (op 7 + index); matches ControlResponse::volumes[].
	static const char* const kBridgeChannels[7] = {
		"Song", "Player 1", "Master", "Player 2", "Microphone", "Voice-over", "Effects"
	};

	static Audio::ControlResponse BridgeControl(uint32_t op, const wchar_t* value = L"") {
		Audio::ControlRequest request;
		request.operation = op;
		wcsncpy_s(request.value, value, _TRUNCATE);
		return Audio::SharedOutput::DispatchControl(request);
	}

	// The recording folder the desktop bridge saved in AudioRouting.ini (next to the game exe). Empty until
	// the user has picked one, in which case the overlay shows a hint instead of a Record button.
	static std::wstring BridgeRecordingFolder() {
		wchar_t exe[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		const std::filesystem::path ini = std::filesystem::path(exe).parent_path() / L"AudioRouting.ini";
		wchar_t value[MAX_PATH]{};
		GetPrivateProfileStringW(L"Audio", L"RecordingDirectory", L"", value, MAX_PATH, ini.c_str());
		return value;
	}

	// Opens the recording folder in Explorer. Rocksmith runs exclusive-fullscreen, so the window lands behind
	// the game; the user alt-tabs to it. Creates the folder first if the bridge has not written to it yet.
	static void OpenBridgeRecordingFolder(const std::wstring& folder) {
		if (folder.empty()) return;
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	// A readable name for the configured in-game record hotkey (e.g. "F9"), so the overlay can show it.
	static std::string RecordHotkeyName() {
		const int vk = static_cast<int>(Settings::GetKeyBind("RecordingHotkey"));
		if (vk > 0) {
			const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
			wchar_t name[64]{};
			if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) {
				char out[64]{};
				if (WideCharToMultiByte(CP_UTF8, 0, name, -1, out, 64, nullptr, nullptr) > 0) return out;
			}
		}
		return "F9";
	}

	// A dark "studio" look so the overlay reads like the desktop bridge rather than raw ImGui grey. Applied
	// once; ImGui keeps the style across device resets.
	static void ApplyBridgeTheme() {
		ImGuiStyle& s = ImGui::GetStyle();
		s.WindowRounding = 6.0f; s.FrameRounding = 4.0f; s.GrabRounding = 4.0f;
		s.ChildRounding = 4.0f; s.ScrollbarRounding = 4.0f; s.PopupRounding = 4.0f;
		s.WindowPadding = ImVec2(12, 12); s.FramePadding = ImVec2(9, 5);
		s.ItemSpacing = ImVec2(8, 7); s.GrabMinSize = 12.0f; s.ScrollbarSize = 13.0f;
		ImVec4* c = s.Colors;
		const ImVec4 accent = ImVec4(0.24f, 0.60f, 0.90f, 1.0f);
		const ImVec4 accentHi = ImVec4(0.36f, 0.72f, 1.00f, 1.0f);
		const ImVec4 panel = ImVec4(0.15f, 0.17f, 0.21f, 1.0f);
		c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.13f, 0.97f);
		c[ImGuiCol_TitleBg] = panel;
		c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.21f, 0.31f, 1.0f);
		c[ImGuiCol_FrameBg] = panel;
		c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.24f, 0.30f, 1.0f);
		c[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.29f, 0.36f, 1.0f);
		c[ImGuiCol_SliderGrab] = accent;
		c[ImGuiCol_SliderGrabActive] = accentHi;
		c[ImGuiCol_CheckMark] = accentHi;
		c[ImGuiCol_Button] = ImVec4(0.20f, 0.24f, 0.30f, 1.0f);
		c[ImGuiCol_ButtonHovered] = accent;
		c[ImGuiCol_ButtonActive] = accentHi;
		c[ImGuiCol_Header] = ImVec4(0.17f, 0.27f, 0.39f, 1.0f);
		c[ImGuiCol_HeaderHovered] = accent;
		c[ImGuiCol_HeaderActive] = accentHi;
		c[ImGuiCol_PlotHistogram] = accent;
		c[ImGuiCol_PlotHistogramHovered] = accentHi;
		c[ImGuiCol_Separator] = ImVec4(0.24f, 0.27f, 0.33f, 1.0f);
	}

	// Hover tooltip for the item just submitted, so the overlay explains itself with the same wording as the
	// desktop bridge's tips. ImGui 1.85 has neither SetItemTooltip nor a built-in hover delay, so this is
	// IsItemHovered + a wrapped tooltip, gated on how long this same item has been hovered so the tip does not
	// flicker as the cursor sweeps across controls (the desktop ToolTip waits ~400 ms; this uses 500 ms).
	static void BridgeHint(const char* text) {
		if (!text || !text[0] || !ImGui::IsItemHovered()) return;
		// Only one item is hovered per frame, so these statics track whichever item that is. Identity is the
		// item's top-left corner; a gap in the frame count means the cursor left every item and came back.
		static ImVec2 hoveredItem(-1.0f, -1.0f);
		static double hoverStart = 0.0;
		static int lastFrame = -2;
		const ImVec2 itemMin = ImGui::GetItemRectMin();
		const int frame = ImGui::GetFrameCount();
		const double t = ImGui::GetTime();
		const bool sameItem = itemMin.x == hoveredItem.x && itemMin.y == hoveredItem.y;
		if (!sameItem || frame - lastFrame > 1) { hoveredItem = itemMin; hoverStart = t; }
		lastFrame = frame;
		if (t - hoverStart < 0.5) return;   // hover delay before the tip appears
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

	// Maps a linear guitar-input peak (0..1) onto the meter bar the same way the D3D diagnostics overlay does:
	// on a dB scale from -60 dBFS (empty) to 0 dBFS (full). A raw linear peak barely moves for a guitar (a
	// loud strum is around -12 dBFS, or 0.25 linear), so the dB mapping is what makes the bar read as a level.
	static float InputMeterFraction(float meterPeak) {
		if (meterPeak <= 0.0f) return 0.0f;
		const float db = 20.0f * std::log10(meterPeak);
		return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
	}

	// One on/off + slider DSP row. The value is kept in whatever integer unit the op wants (see callers);
	// toggling off sends the op's "off" value, toggling on or dragging sends the live value. Returns nothing;
	// each caller owns the actual send so the op-specific formatting stays in one place.
	void AddAudioBridgeMenu() {
		static bool themed = false;
		if (!themed) { ApplyBridgeTheme(); themed = true; }

		ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Audio Bridge", &Menu::audioBridgeMenuEnabled)) {
			ImGui::End();
			return;
		}

		// Live status (op 1): meters and recording state. Polled ~10 Hz off the render thread, not every
		// frame, so the per-frame touch of the audio engine stays cheap.
		static Audio::ControlResponse status;
		static Audio::CableInput::Diagnostics inputDiag;   // guitar input meters (P1 + P2), separate from the output mix
		static ULONGLONG lastPoll = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - lastPoll >= 100) { status = BridgeControl(1); inputDiag = Audio::CableInput::GetDiagnostics(); lastPoll = now; }

		// Editable state, seeded once from the saved settings (RSMods.ini) and live volumes the first time the
		// panel is built, then user-driven. Seeding from the same keys the DLL applies at launch means the
		// sliders show the real current values, not defaults.
		static bool seeded = false;
		static float volumes[7] = {};
		static bool gainOn = false;   static int gainTenths = 0;      // op 15, tenths of dB (0..200)
		static bool gateOn = false;   static int gateTenths = -500;   // op 19, suppressor open threshold in signed tenths of dB
		static bool compOn = false;   static int compPct = 0;         // op 20, 0..100
		static bool humOn = false;    static int humHz = 50;          // op 24, Hz
		static bool rgOn = false;     static int rgTenths = -593;     // op 23, tenths of dB (P1_NoiseFloor)
		static bool limOn = false;    static int limTenths = -60;     // op 16, ceiling in tenths of dBFS
		static bool p2CableOn = false;                                // op 26, Real Tone Cable for Player 2
		static bool diagOverlayOn = true;                             // op 27, in-game diagnostics HUD
		static bool detectionOverlayOn = true;                        // op 29, Note-by-Note detection HUD
		static int targetPosition = 0;                                // 0 = Left, 1 = Center
		if (!seeded) {
			for (int i = 0; i < 7; ++i) volumes[i] = status.volumes[i];
			gainTenths = Settings::GetModSetting("AsioInputGain");        gainOn = gainTenths > 0;
			gateTenths = Settings::GetModSetting("NoiseGateThreshold");   gateOn = gateTenths < 0; if (!gateOn) gateTenths = -500;
			compPct = Settings::GetModSetting("CompressorStrength");      compOn = compPct > 0;
			humHz = Settings::GetModSetting("HumFilter");                 humOn = humHz >= 20; if (!humOn) humHz = 50;
			rgOn = Settings::GetModSetting("RocksmithGateOverride") != 0; rgTenths = Settings::GetModSetting("RocksmithGateThreshold"); if (rgTenths == 0) rgTenths = -593;
			limOn = Settings::GetModSetting("AudioBridgeLimiter") != 0;   limTenths = Settings::GetModSetting("AudioBridgeLimiterLevel"); if (limTenths == 0) limTenths = -60;
			p2CableOn = Settings::ReturnSettingValue("CableForPlayerTwo") == "on";
			diagOverlayOn = Audio::CableInput::IsOverlayEnabled();
			detectionOverlayOn = Settings::IsNoteByNoteDetectionVisible();
			targetPosition = Settings::GetNoteByNoteTargetPosition() == Settings::NoteByNoteTargetPosition::Center ? 1 : 0;
			seeded = true;
		}

		// Meters sit above the tabs so the levels are always visible whichever tab is open. The input meters
		// read the guitar signal from the input tap (CableInput diagnostics), not the output mix: status.peak
		// is the game's output level, so using it here left the "input" bar dead no matter how hard you played.
		// Player 2's meter is only drawn in two-player, matching the D3D diagnostics overlay's second row.
		const bool showPlayerTwo = GameState::IsMultiplayer();
		ImGui::ProgressBar(InputMeterFraction(inputDiag.meterPeak), ImVec2(-1.0f, 0.0f), showPlayerTwo ? "input 1" : "input");
		BridgeHint("Player 1's guitar input level, after the input gain and before Rocksmith hears it.");
		if (showPlayerTwo) {
			ImGui::ProgressBar(InputMeterFraction(inputDiag.playerTwoMeterPeak), ImVec2(-1.0f, 0.0f), "input 2");
			BridgeHint("Player 2's guitar input level. Shown only in two-player. Fed by the Real Tone Cable for Player 2 or the second ASIO input.");
		}
		ImGui::ProgressBar(status.outputPeak[0], ImVec2(-1.0f, 0.0f), "output L");
		BridgeHint("The game's left-channel output level, after the volume cap.");
		ImGui::ProgressBar(status.outputPeak[1], ImVec2(-1.0f, 0.0f), "output R");
		BridgeHint("The game's right-channel output level, after the volume cap.");

		if (ImGui::BeginTabBar("bridgeTabs")) {
			// --- Mixer ----------------------------------------------------------------------------------
			if (ImGui::BeginTabItem("Mixer")) {
				for (int i = 0; i < 7; ++i) {
					ImGui::SetNextItemWidth(-110.0f);   // fill the window width, leaving room for the channel name
					if (ImGui::SliderFloat(kBridgeChannels[i], &volumes[i], 0.0f, 100.0f, "%.0f%%")) {
						wchar_t value[16]; swprintf_s(value, L"%d", (int)(volumes[i] + 0.5f));
						BridgeControl(7 + i, value);
					}
					BridgeHint("Playback volume for this bus. Changes what you hear, not what Note by Note or a recording of the game mix captures.");
				}
				ImGui::EndTabItem();
			}

			// --- Guitar input processing ----------------------------------------------------------------
			if (ImGui::BeginTabItem("Guitar")) {
				// Each row is a toggle, then a slider that fills the window width (SetNextItemWidth(-44) leaves a
				// fixed margin for the unit label so the value and unit stay readable at any window size).
				const float sliderMargin = -44.0f;
				// Input make-up gain (op 15, tenths of dB; off = 0).
				if (ImGui::Checkbox("Input gain", &gainOn)) { wchar_t v[16]; swprintf_s(v, L"%d", gainOn ? gainTenths : 0); BridgeControl(15, v); }
				BridgeHint("Adds gain to the guitar signal before Rocksmith hears it. 0 dB leaves the input unchanged. Applies live.");
				ImGui::SameLine(); float gainDb = gainTenths / 10.0f; ImGui::SetNextItemWidth(sliderMargin);
				if (ImGui::SliderFloat("dB##gain", &gainDb, 0.0f, 20.0f, "%.1f")) { gainTenths = (int)(gainDb * 10 + 0.5f); if (gainOn) { wchar_t v[16]; swprintf_s(v, L"%d", gainTenths); BridgeControl(15, v); } }
				BridgeHint("Input gain from 0 to +20 dB.");

				// Transient-resistant noise suppressor (op 19, signed tenths of dB; off = 0).
				if (ImGui::Checkbox("Noise suppressor", &gateOn)) { wchar_t v[16]; swprintf_s(v, L"%d", gateOn ? gateTenths : 0); BridgeControl(19, v); }
				BridgeHint("Requires a clear note attack, rejects weaker false-signal bursts, then keeps the note open down to the sustain threshold. Applies live.");
				ImGui::SameLine(); float gateDb = gateTenths / 10.0f; ImGui::SetNextItemWidth(sliderMargin);
				if (ImGui::SliderFloat("dB##gate", &gateDb, -80.0f, -20.0f, "%.1f")) { gateTenths = (int)(gateDb * 10 - 0.5f); if (gateOn) { wchar_t v[16]; swprintf_s(v, L"%d", gateTenths); BridgeControl(19, v); } }
				BridgeHint("Sustain threshold from -80 to -20 dBFS. Lower values preserve quieter note tails after a valid attack.");

				// Compressor (op 20, 0..100; off = 0).
				if (ImGui::Checkbox("Compressor", &compOn)) { wchar_t v[16]; swprintf_s(v, L"%d", compOn ? compPct : 0); BridgeControl(20, v); }
				BridgeHint("Compresses the guitar input to even out level swings during sustained notes. Applies live.");
				ImGui::SameLine(); ImGui::SetNextItemWidth(sliderMargin);
				if (ImGui::SliderInt("%##comp", &compPct, 0, 100)) { if (compOn) { wchar_t v[16]; swprintf_s(v, L"%d", compPct); BridgeControl(20, v); } }
				BridgeHint("Compressor strength from 0 to 100%.");

				// Mains-hum notch (op 24, Hz; off = 0).
				if (ImGui::Checkbox("Hum filter", &humOn)) { wchar_t v[16]; swprintf_s(v, L"%d", humOn ? humHz : 0); BridgeControl(24, v); }
				BridgeHint("Notches out mains hum and its harmonics, continuously and even during notes. Applies live.");
				ImGui::SameLine(); ImGui::SetNextItemWidth(sliderMargin);
				if (ImGui::SliderInt("Hz##hum", &humHz, 20, 120)) { if (humOn) { wchar_t v[16]; swprintf_s(v, L"%d", humHz); BridgeControl(24, v); } }
				BridgeHint("Mains frequency from 20 to 120 Hz. 50 Hz in the UK, EU and AU; 60 Hz in the US.");

				// Rocksmith gate override (op 23, "<on>,<tenths>").
				bool rgChanged = ImGui::Checkbox("Rocksmith gate", &rgOn);
				BridgeHint("Takes over Rocksmith's own amp noise gate so the game stops cutting a note as it decays. Applies live.");
				ImGui::SameLine(); float rgDb = rgTenths / 10.0f; ImGui::SetNextItemWidth(sliderMargin);
				bool rgSlid = ImGui::SliderFloat("dB##rg", &rgDb, -100.0f, 10.0f, "%.1f");
				BridgeHint("Rocksmith gate threshold from -100 to +10 dB. Lower values keep the gate open for longer sustain.");
				if (rgSlid) rgTenths = (int)(rgDb * 10 + (rgDb < 0 ? -0.5f : 0.5f));
				if (rgChanged || rgSlid) { wchar_t v[24]; swprintf_s(v, L"%d,%d", rgOn ? 1 : 0, rgTenths); BridgeControl(23, v); }
				ImGui::EndTabItem();
			}

			// --- Output (pinned to the far right of the tab bar) -----------------------------------------
			if (ImGui::BeginTabItem("Output", nullptr, ImGuiTabItemFlags_Trailing)) {
				// Translate the engine's endpoint sentinels into plain words. "(passthrough)" is the normal,
				// high-performance state: the game plays straight out its own device (your ASIO interface).
				const std::wstring ep = status.endpoint;
				if (ep == L"(passthrough)") {
					// Passthrough means the game plays straight out its own ASIO device; the engine never opened
					// it, so name it from RS_ASIO.ini instead (same source the desktop bridge shows).
					const std::wstring device = ReadPassthroughOutputName();
					if (!device.empty()) ImGui::Text("Playing to: %ls (direct)", device.c_str());
					else ImGui::TextUnformatted("Playing to: your audio device (direct)");
				}
				else if (ep == L"(silent)") ImGui::TextUnformatted("Playing to: no output");
				else if (ep == L"(starting)" || ep == L"(downgrading)") ImGui::TextUnformatted("Playing to: switching...");
				else if (ep.rfind(L"(route", 0) == 0) ImGui::TextUnformatted("Playing to: a routed device");
				else if (ep[0]) ImGui::Text("Playing to: %ls", status.endpoint);
				ImGui::Spacing();
				// Output protection (op 16 limiter; ceiling sent linear = pow(10, tenths/200)).
				bool limChanged = ImGui::Checkbox("Cap the maximum volume", &limOn);
				BridgeHint("Sets a maximum on the game's output level. A look-ahead limiter watches for anything louder than the ceiling and eases the level down before it arrives, so a sudden loud tone or spike never reaches full volume. It caps the digital signal, not your interface's volume knob, so set that knob to a comfortable level first. Adds a few milliseconds of latency. Optional and off by default.");
				ImGui::SetNextItemWidth(-96.0f);
				float ceilDb = limTenths / 10.0f;
				bool ceilSlid = ImGui::SliderFloat("ceiling dBFS", &ceilDb, -24.0f, 0.0f, "%.1f");
				BridgeHint("The volume ceiling, from -24 to 0 dBFS. Nothing in the game's output rises above this. Lower is quieter and safer; 0 is full scale. Your interface volume still sets how loud that actually is.");
				if (ceilSlid) limTenths = (int)(ceilDb * 10 + (ceilDb < 0 ? -0.5f : 0.5f));
				if (limChanged || ceilSlid) {
					const double ceilingLinear = std::pow(10.0, limTenths / 200.0);
					wchar_t v[48]; swprintf_s(v, L"%d,%.6f,0,0.1", limOn ? 1 : 0, ceilingLinear);
					BridgeControl(16, v);
				}
				ImGui::Spacing();
				// Output-device selection stays in the desktop bridge on purpose: routing the game mix to the
				// same physical device the game already uses over ASIO silences it (and RS_ASIO input does not
				// hot-recover), so this is not offered in the overlay. Cable/ASIO input mode is in the main
				// RSMods window (RSModsPlus tab).
				ImGui::TextDisabled("Change the output device and Cable/ASIO input mode in the\ndesktop bridge and the main RSMods window (game closed).");
				ImGui::Spacing();
				if (ImGui::Button("Open the desktop bridge"))
					Keybindings::BringAudioBridgeToFront();
				BridgeHint("Switches focus to the desktop bridge so you can change the output device, drivers and heavier setup.");
				ImGui::TextDisabled("Switches to the desktop bridge window.");
				ImGui::EndTabItem();
			}

			// --- Recording --------------------------------------------------------------------------------
			// Record/Stop presses the desktop bridge's own record (the same toggle the F9 hotkey posts), so it
			// captures exactly what the bridge is set to: the chosen audio source and, when Format is Video, the
			// MP4 screen capture (Windows Graphics Capture, which records the game even in exclusive fullscreen).
			if (ImGui::BeginTabItem("Record")) {
				const bool recording = status.recording != 0;
				const double seconds = status.recordedFrames / 48000.0;
				ImGui::Text("%s   %02d:%04.1f", recording ? "Recording" : "Idle", (int)(seconds / 60.0), std::fmod(seconds, 60.0));

				// Wet and dry WAVs are always paired. The overlay only chooses whether to add video.
				static int recFormat = 1;   // 0 = audio (WAV), 1 = video (MP4); default to video
				ImGui::TextUnformatted("Audio"); ImGui::SameLine();
				ImGui::TextDisabled("Wet + dry WAVs");
				BridgeHint("Saves the full game mix and Player 1's dry guitar together.");
				ImGui::TextUnformatted("Format"); ImGui::SameLine();
				ImGui::RadioButton("Audio", &recFormat, 0);
				BridgeHint("Saves a WAV of the selected source.");
				ImGui::SameLine();
				ImGui::RadioButton("Video", &recFormat, 1);
				BridgeHint("Saves an MP4 of the selected source plus the Rocksmith window. Needs the desktop bridge running.");

				if (ImGui::Button(recording ? "Stop & save" : "Record"))
					Keybindings::ToggleAudioBridgeRecording(recFormat == 1);
				BridgeHint("Starts or stops a paired wet + dry take. Same as the in-game record hotkey.");

				const std::wstring folder = BridgeRecordingFolder();
				if (!folder.empty()) {
					if (ImGui::Button("Open recordings folder")) OpenBridgeRecordingFolder(folder);
					BridgeHint("Opens the folder where takes are saved. It opens behind the game, so alt-tab to see it.");
				}
				ImGui::TextDisabled("In-game record hotkey: %s", RecordHotkeyName().c_str());
				if (FAILED(status.recordingError))
					ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "recording error 0x%08X", (unsigned)status.recordingError);
				ImGui::EndTabItem();
			}

			// --- Misc -------------------------------------------------------------------------------------
			if (ImGui::BeginTabItem("Misc")) {
				// Keep these controls synchronized with desktop-bridge changes made while this panel remains open.
				// The authoritative values are updated by the shared control handlers, so refresh immediately before
				// drawing each checkbox without interfering with the click generated by this frame.
				diagOverlayOn = Audio::CableInput::IsOverlayEnabled();
				detectionOverlayOn = Settings::IsNoteByNoteDetectionVisible();
				if (ImGui::Checkbox("In-game diagnostics overlay", &diagOverlayOn)) BridgeControl(27, diagOverlayOn ? L"1" : L"0");
				BridgeHint("Shows the audio diagnostics HUD (levels, buffer and route state) inside the game.");
				if (ImGui::Checkbox("Note-by-Note detection overlay", &detectionOverlayOn)) BridgeControl(29, detectionOverlayOn ? L"1" : L"0");
				BridgeHint("Shows the live Note-by-Note detection result while playing. Applies immediately and is remembered for the next launch.");
				ImGui::TextUnformatted("Target display position");
				if (ImGui::RadioButton("Left##targetPosition", &targetPosition, 0))
					Settings::SetNoteByNoteTargetPosition(Settings::NoteByNoteTargetPosition::Left);
				ImGui::SameLine();
				if (ImGui::RadioButton("Center##targetPosition", &targetPosition, 1))
					Settings::SetNoteByNoteTargetPosition(Settings::NoteByNoteTargetPosition::Center);
				BridgeHint("Left keeps the existing upper-left target. Center moves only the target note or chord near the upper-middle gameplay area in Note by Note mode.");
				if (ImGui::Checkbox("Real Tone Cable for Player 2", &p2CableOn)) BridgeControl(26, p2CableOn ? L"1" : L"0");
				BridgeHint("Adds the RSModsPlus Real Tone Cable device after the configured ASIO input. RS_ASIO.ini is not changed. Applies live while Rocksmith is connected.");
				ImGui::Spacing();
				if (ImGui::Button("Force update song list")) BridgeControl(28);
				BridgeHint("Re-scans your CDLC so a newly added song shows up without restarting. Open the song list once first if nothing changes.");
				ImGui::EndTabItem();
			}

			// --- Debug ------------------------------------------------------------------------------------
			if (ImGui::BeginTabItem("Debug")) {
				const bool externalAmpMode = VolumeControl::IsExternalAmpModeEnabled();
				ImGui::TextUnformatted("External amp diagnosis");
				ImGui::TextDisabled("Mutes Rocksmith's audible Player 1 Wwise bus so an external amp can be diagnosed.");
				ImGui::TextDisabled("Dry guitar detection remains active. This does not release the audio driver or guarantee Wwise DSP has stopped.");
				ImGui::Spacing();
				if (ImGui::Button("Enable external amp mode")) VolumeControl::EnableExternalAmpMode();
				BridgeHint("Saves the current Player 1 mixer volume and sets only Mixer_Player1 to 0 for diagnosis.");
				ImGui::SameLine();
				if (ImGui::Button("Restore Rocksmith guitar")) VolumeControl::RestoreRocksmithGuitar();
				BridgeHint("Restores the Player 1 mixer volume saved when external amp mode was enabled.");
				ImGui::Text("External amp mode: %s", externalAmpMode ? "enabled" : "disabled");

				// Probe refresh: only appears on builds that ship a Note by Note probe. Hot-swaps
				// the deployed probe in-process, so a freshly built probe goes live without the
				// research pipe, the N key, or a game restart.
				if (ResearchBridge::IsProbeReloadAvailable()) {
					static std::string probeReloadStatus;
					ImGui::Spacing();
					ImGui::TextDisabled("Note by Note research probe");
					if (ImGui::Button("Refresh probes")) {
						std::string reloadError;
						probeReloadStatus = ResearchBridge::ReloadDeployedProbe(reloadError)
							? "Probe reloaded from disk."
							: ("Reload failed: " + reloadError);
					}
					BridgeHint("Hot-swaps the deployed Note by Note probe DLL in place so a freshly built probe goes live without restarting the game. Note by Note is briefly disabled during the swap and restored after.");
					if (!probeReloadStatus.empty()) ImGui::TextDisabled("%s", probeReloadStatus.c_str());
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		ImGui::Spacing();
		ImGui::TextDisabled("Press \\ to close.");
		ImGui::End();
	}

	/// <summary>
	/// Regenerate string colors when the user changes their string colors in the GUI.
	/// Needed to have real-time textures.
	/// </summary>
	void UpdateStringTextures(IDirect3DDevice9* pDevice) {
		if (generateTexture) {
			D3D::GenerateTextures(pDevice, D3D::Strings);

			generateTexture = false;
		}
	}

	void Init(IDirect3DDevice9* pDevice, LONG_PTR WndProc) {
		if (ImGuiInit) {
			return;
		}
		
		ImGuiInit = true;

		// Create ImGUI
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		ImFont* font = io.Fonts->AddFontFromMemoryCompressedTTF(RobotoFont_data, RobotoFont_size, 20);
		io.FontDefault = font;

		// Hook WndProc (Keypress manager)
		D3DDEVICE_CREATION_PARAMETERS d3dcp;
		pDevice->GetCreationParameters(&d3dcp);
		D3DHooks::hThisWnd = d3dcp.hFocusWindow;
		D3DHooks::oWndProc = (WNDPROC)SetWindowLongPtr(D3DHooks::hThisWnd, GWLP_WNDPROC, (LONG_PTR)WndProc);

		ImGui_ImplWin32_Init(D3DHooks::hThisWnd);
		ImGui_ImplDX9_Init(pDevice);
		ImGui::GetIO().ImeWindowHandle = D3DHooks::hThisWnd;
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

		LOG_INFO("ImGUI Init" << std::endl);

		Settings::UpdateSettings();

		CreateTextures(pDevice);
	}

	void AddStringColorTestingUI() {
		static bool CB = false;

		static std::string previewValue = "Select a string";
		if (ImGui::BeginCombo("String Colors", previewValue.c_str()))
		{
			for (int n = 0; n < 6; n++)
			{
				const bool is_selected = (selectedString == n);
				if (ImGui::Selectable(comboStringsItems[n], is_selected, ImGuiSelectableFlags_::ImGuiSelectableFlags_DontClosePopups))
					selectedString = n;

				if (is_selected) {
					previewValue = std::to_string(selectedString);

					RSColor currColors = D3D::GetCustomColors(selectedString, CB)["Enabled"];
					strR = (int)currColors.r * 255;
					strG = (int)currColors.g * 255;
					strB = (int)currColors.b * 255;
				}
			}

			ImGui::EndCombo();
		}

		ImGui::SliderInt("R", &strR, 0, 255);
		ImGui::SliderInt("G", &strG, 0, 255);
		ImGui::SliderInt("B", &strB, 0, 255);
		ImGui::Checkbox("CB", &CB);

		if (ImGui::Button("Generate Texture")) {
			Settings::SetStringColors(selectedString, RSColor(strR, strG, strB), CB);
			generateTexture = true;
		}

		if (ImGui::Button("Restore default colors"))
			ERMode::ResetString(selectedString);
	}

	void CreateTextures(IDirect3DDevice9* pDevice) {
		D3DXCreateTextureFromFile(pDevice, L"nonexistenttexture.dds", &nonexistentTexture); // Black Notes
		D3DXCreateTextureFromFile(pDevice, L"headstock.png", &customHeadstockTexture); // Custom Headstock

		// Green Screen Wall textures. Look at the uses of the textures for more information.
		D3DXCreateTextureFromFile(pDevice, L"stage0.png", &customGreenScreenWall_Stage0); // Background Tile
		D3DXCreateTextureFromFile(pDevice, L"stage1.png", &customGreenScreenWall_Stage1); // Noise
		D3DXCreateTextureFromFile(pDevice, L"stage2.png", &customGreenScreenWall_Stage2); // Caustic (Indirect)
		D3DXCreateTextureFromFile(pDevice, L"stage3.png", &customGreenScreenWall_Stage3); // Narnia / Venue Fade In Mask.
		D3DXCreateTextureFromFile(pDevice, L"stage4.png", &customGreenScreenWall_Stage4); // White square
		D3DXCreateTextureFromFile(pDevice, L"stage5.png", &customGreenScreenWall_Stage5); // Pipes and wall trim
		D3DXCreateTextureFromFile(pDevice, L"stage6.png", &customGreenScreenWall_Stage6); // N Mask of Background tile
		D3DXCreateTextureFromFile(pDevice, L"ChordFHM.png", &customChordPanelFHMTexture); // Custom Chord Panel FHM (for Metallica93).
	}

	void AddMidiMenu() {
		if (static std::string previewValue = "Select a device"; ImGui::BeginCombo("MIDI devices", previewValue.c_str())) 
		{
			for (size_t i = 0; i < Midi::NumberOfOutPorts; ++i)
			{
				const bool isSelected = (selectedDevice == i);
				const auto& device = Midi::midiOutDevices[i];

				if (ImGui::Selectable(device.szPname, isSelected, ImGuiSelectableFlags_DontClosePopups))
				{
					selectedDevice = i;
					Midi::SelectedMidiOutDevice = i;
				}

				if (isSelected)
				{
					previewValue = device.szPname;
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		ImGui::SliderInt("Program Change", &Midi::MidiPC, 0, 127);
		ImGui::SliderInt("Control Change", &Midi::MidiCC, 0, 127);

		if (ImGui::Button("Send PC MIDI Message"))
			Midi::SendDataToThread_PC(Midi::MidiPC);

		if (ImGui::Button("Send CC MIDI Message"))
			Midi::SendDataToThread_CC(Midi::MidiCC);
	}

	void AddCalibrationMenu() {
		ImGui::Begin("Calibration");

		static float NewNoiseFloor = -59.3134f;
		static float NewToneBalance = -19.4793f;
		static float NewInputVolume = -3.22313f;
		static float NewInputVolumeReturn = -3.22313f;

		ImGui::SliderFloat("Noise Floor", &NewNoiseFloor, -100.f, 10.f);
		ImGui::SliderFloat("Tone Balance", &NewToneBalance, -100.f, 10.f);
		ImGui::SliderFloat("Input Volume", &NewInputVolume, -100.f, 10.f);
		ImGui::SliderFloat("Input Volume Return", &NewInputVolumeReturn, -100.f, 10.f);

		if (ImGui::Button("Calibrate"))
		{
			// Noise Floor
			Wwise::SoundEngine::SetRTPCValue("P1_NoiseFloor", NewNoiseFloor, 0x1234, 0, AkCurveInterpolation_Linear);
			Wwise::SoundEngine::SetRTPCValue("P1_NoiseFloor", NewNoiseFloor, AK_INVALID_GAME_OBJECT, 0, AkCurveInterpolation_Linear);

			// Tone Balance
			Wwise::SoundEngine::SetRTPCValue("Meter_Tone_Balance_Return", NewToneBalance, 0x1234, 0, AkCurveInterpolation_Linear);
			Wwise::SoundEngine::SetRTPCValue("Meter_Tone_Balance_Return", NewToneBalance, AK_INVALID_GAME_OBJECT, 0, AkCurveInterpolation_Linear);

			// Input Volume
			Wwise::SoundEngine::SetRTPCValue("P1_InputVol_Calibration", NewInputVolume, 0x1234, 0, AkCurveInterpolation_Linear);
			Wwise::SoundEngine::SetRTPCValue("P1_InputVol_Calibration", NewInputVolume, AK_INVALID_GAME_OBJECT, 0, AkCurveInterpolation_Linear);

			// Input Volume Return
			Wwise::SoundEngine::SetRTPCValue("P1_InputVol_Calibration_Return", NewInputVolumeReturn, 0x1234, 0, AkCurveInterpolation_Linear);
			Wwise::SoundEngine::SetRTPCValue("P1_InputVol_Calibration_Return", NewInputVolumeReturn, AK_INVALID_GAME_OBJECT, 0, AkCurveInterpolation_Linear);
		}

		ImGui::End();
	}

	void AddMicrophonesMenu() {
		ImGui::Begin("Microphones");

		static std::string previewMicrophone = "Select a Microphone";
		static std::string selectedMicrophone = "";
		static std::vector<std::string> microphones;

		if (microphones.empty()) 
		{
			for (const auto& [key, value] : AudioDevices::activeMicrophones)
			{
				microphones.push_back(key);
			}
		}

		if (ImGui::BeginCombo("Microphones", previewMicrophone.c_str())) 
		{
			for (const auto& microphone : microphones)
			{
				const bool isSelected = (selectedMicrophone == microphone);

				if (ImGui::Selectable(microphone.c_str(), isSelected, ImGuiSelectableFlags_DontClosePopups))
				{
					selectedMicrophone = microphone;
				}

				if (isSelected)
				{
					previewMicrophone = microphone;
					ImGui::SetItemDefaultFocus();
				}
			}

			ImGui::EndCombo();
		}

		// Button for testing setting volume on the microphone.
		if (ImGui::Button("Random Volume"))
			AudioDevices::SetMicrophoneVolume(selectedMicrophone, rand() % 100);

		ImGui::End();
	}

	void AddVoicelinesMenu() {
		ImGui::Begin("Voicelines");

		static std::string previewVoiceline = "Select a voiceline";
		static std::vector<VoiceOver> selectedVoiceOverList = VoiceOverControl::VO_ResultsScreens;

		// Drop-down list of all voice-overs in the selected Voice-Over list.
		if (ImGui::BeginCombo("Voicelines", previewVoiceline.c_str())) 
		{
			for (const auto& voiceOver : selectedVoiceOverList)
			{
				const bool isSelected = (VoiceOverControl::selectedVoiceOver.EventName == voiceOver.EventName);

				if (ImGui::Selectable(voiceOver.Text.c_str(), isSelected, ImGuiSelectableFlags_DontClosePopups))
				{
					VoiceOverControl::selectedVoiceOver = voiceOver;
				}

				if (isSelected)
				{
					previewVoiceline = voiceOver.Text;
					ImGui::SetItemDefaultFocus();
				}
			}

			ImGui::EndCombo();
		}

		if (ImGui::Button("Play selected voiceline"))
			VoiceOverControl::PlayVoiceOver(VoiceOverControl::selectedVoiceOver);

		ImGui::End();
	}
}
