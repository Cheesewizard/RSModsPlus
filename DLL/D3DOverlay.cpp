#include "stdafx.h"
#include "D3DOverlay.hpp"
#include "Mods/DropPedal/DropPedalOverlay.hpp"
#include "Mods/DropPedal/DropPedal.hpp"
#include "Mods/NoteByNoteNativeScoring.hpp"
#include "D3D/NoteByNoteHighwayRenderer.hpp"
#include "Audio/MlStringFretReader.hpp"
#include "Audio/MlFretDisplay.hpp"
#include "Mods/NoteByNoteController.hpp"
#include "Audio/CableInput.hpp"
#include "OverlayToggles.hpp"

/// <returns>Size of Rocksmith Window</returns>
Resolution GameOverlay::GetWindowSize() {
	RECT windowSize;

	Resolution currentSize;
	if (GetWindowRect(D3DHooks::GetGameWindow(), &windowSize))
	{
		currentSize.width = windowSize.right - windowSize.left;
		currentSize.height = windowSize.bottom - windowSize.top;
	}

	return currentSize;
}

/// <summary>
/// Draw text on screen
/// </summary>
/// <param name="textToDraw"> - What text should be written?</param>
/// <param name="textColorHex"> - What color? Given in hex in the AA,RR,GG,BB format.</param>
/// <param name="topLeftX"> - top LEFT of textbox</param>
/// <param name="topLeftY"> - TOP left of textbox</param>
/// <param name="bottomRightX"> - bottom RIGHT of textbox</param>
/// <param name="bottomRightY"> - BOTTOM right of textbox</param>
/// <param name="pDevice"> - Device Pointer</param>
/// <param name="setFontSize"> - Override font size</param>
/// <param name="format"> - DrawText format</param>
void GameOverlay::DX9DrawText(const std::string& textToDraw, int textColorHex, int topLeftX, int topLeftY, int bottomRightX, int bottomRightY, LPDIRECT3DDEVICE9 pDevice, Resolution setFontSize, DWORD format)
{
	CComPtr<ID3DXFont> font;
	bool useInputFontSize = setFontSize.height != 0;

	if (useInputFontSize) {
		int targetH = setFontSize.height;
		const std::string face = Settings::ReturnSettingValue("OnScreenFont");
		FontKey key = FontKey::Make(face, targetH, 0, FW_NORMAL, false);

		if (!fontCache.Get(pDevice, key, font)) {
			LOG_ERROR("Could not acquire custom-sized font." << std::endl);
			return;
		}
	}
	else {
		if (cachedFont) {
			font = cachedFont;
		}
		else {
			LOG_ERROR("Default font is not cached!" << std::endl);
			return;
		}
	}

	RECT TextRectangle{ topLeftX, topLeftY, bottomRightX, bottomRightY }; // Left, Top, Right, Bottom

	// Preload And Draw The Text (Supposed to reduce the performance hit (It's D3D/DX9 but still good practice))
	font->PreloadTextA(textToDraw.c_str(), textToDraw.length());
	font->DrawTextA(nullptr, textToDraw.c_str(), -1, &TextRectangle, format, textColorHex);
}

// Wide-string sibling of DX9DrawText. The font objects are Unicode-capable regardless of
// having been created through D3DXCreateFontA, so a glyph like U+266A (musical note) renders
// correctly here where the ANSI path would mangle it. Font size is always explicit (a badge
// picks its own size); `weight` reaches the cache key so a bold face is a distinct entry.
void GameOverlay::DX9DrawTextW(const std::wstring& textToDraw, int textColorHex, int topLeftX, int topLeftY, int bottomRightX, int bottomRightY, LPDIRECT3DDEVICE9 pDevice, int fontHeight, DWORD format, int weight)
{
	CComPtr<ID3DXFont> font;
	const std::string face = Settings::ReturnSettingValue("OnScreenFont");
	FontKey key = FontKey::Make(face, fontHeight, 0, weight, false);
	if (!fontCache.Get(pDevice, key, font)) {
		LOG_ERROR("Could not acquire wide-text font." << std::endl);
		return;
	}

	RECT TextRectangle{ topLeftX, topLeftY, bottomRightX, bottomRightY }; // Left, Top, Right, Bottom
	font->DrawTextW(nullptr, textToDraw.c_str(), -1, &TextRectangle, format, textColorHex);
}

void GameOverlay::DisplayMixer() {
	// Display the whole mixer if displayMixer is true
	if (Settings::ReturnSettingValue("VolumeControlEnabled") == "on" && displayMixer) {

		float offset = 0;
		for (int volumeIndex = 0; volumeIndex < mixerInternalNames.size(); ++volumeIndex) {

			float volume = 0;
			RTPCValue_type type = RTPCValue_GameObject;
			Wwise::SoundEngine::Query::GetRTPCValue(mixerInternalNames[volumeIndex].c_str(), AK_INVALID_GAME_OBJECT, &volume, &type);

			DX9DrawText(
				drawMixerTextName[volumeIndex] + std::to_string(static_cast<int>(volume)) + "%",
				whiteText,
				static_cast<int>(WindowSize.width / 96.0f),  // 20 pixels from left in 1920x1080 resolution
				static_cast<int>(WindowSize.height / 54.0f + offset), // 20 pixels from top (plus an offset to display multiple values)
				static_cast<int>(WindowSize.width / 19.2f),  // 120 pixels from left
				static_cast<int>(WindowSize.height / 16.0f), // 120 pixels from top
				pDevice);

			// Adjust the offset to display the next value
			offset += WindowSize.height / 54.0f;
		}
	}
	// Display just the current volume based on context (This will display the last volume that was adjusted for a few seconds after adjusting it)
	else if (Settings::ReturnSettingValue("VolumeControlEnabled") == "on" && displayCurrentVolume) {
		float volume = 0;
		RTPCValue_type type = RTPCValue_GameObject;
		Wwise::SoundEngine::Query::GetRTPCValue(mixerInternalNames[currentVolumeIndex].c_str(), AK_INVALID_GAME_OBJECT, &volume, &type);

		DX9DrawText(
			drawMixerTextName[currentVolumeIndex] + std::to_string(static_cast<int>(volume)) + "%",
			whiteText,
			static_cast<int>(WindowSize.width / 96.0f),  // 20 pixels from left in 1920x1080 resolution
			static_cast<int>(WindowSize.height / 54.0f), // 20 pixels from top
			static_cast<int>(WindowSize.width / 19.2f),  // 120 pixels from left
			static_cast<int>(WindowSize.height / 16.0f), // 120 pixels from top
			pDevice);
	}
}

void GameOverlay::DisplaySongTimer()
{
	if (!D3DHooks::showSongTimerOnScreen) return;

	const float songTime = SongTimer::SongTimer();
	if (songTime == 0.f) return;

	DX9DrawText(
		D3DHooks::ConvertFloatTimeToStringTime(songTime),
		whiteText,
		static_cast<int>(WindowSize.width - WindowSize.width / 16.0f), // 120 pixels left from right edge in 1920x1080 resolution
		static_cast<int>(WindowSize.height / 54.0f),                   // 20 pixels from top
		static_cast<int>(WindowSize.width - WindowSize.width / 96.0f), // 20 left from right edge
		static_cast<int>(WindowSize.height / 16.0f),                   // 120 pixels from top
		pDevice,
		{ NULL, NULL },
		DT_RIGHT | DT_NOCLIP);
}

void GameOverlay::DisplayCurrentNote()
{
	if (Settings::ReturnSettingValue("ShowCurrentNoteOnScreen") == "on" && GuitarSpeak::GetCurrentNoteName() != (std::string)"") {

		if (GameState::IsInSong()) {
			DX9DrawText(
				GuitarSpeak::GetCurrentNoteName(),
				whiteText,
				static_cast<int>(WindowSize.width / 5.5),		// 349 pixels left of the center in 1920x1080 resolution.
				static_cast<int>(WindowSize.height / 1.75),	// 617 pixels from the top
				static_cast<int>(WindowSize.width / 5.75),		// 334 pixels right of center
				static_cast<int>(WindowSize.height / 8),		// 135 pixels from the top
				pDevice);
		}
		else { // Show outside of the song at the top of the screen.
			DX9DrawText(
				"Current Note: " + GuitarSpeak::GetCurrentNoteName(),
				whiteText,
				static_cast<int>(WindowSize.width / 3.87),		// 496 pixels left of the center in 1920x1080 resolution
				static_cast<int>(WindowSize.height / 30.85),	// 35 pixels from the top
				static_cast<int>(WindowSize.width / 4),		// 480 pixel right of the center
				static_cast<int>(WindowSize.height / 8),		// 135 pixels from the top
				pDevice);
		}
	}
}

void GameOverlay::DisplayRiffRepeaterOverHundredPercentSpeed()
{
	if (Settings::ReturnSettingValue("RRSpeedAboveOneHundred") == "on" && RiffRepeater::loggedCurrentSongID &&
		(GameState::Menus::IsInModesWithAllowedFastRiffRepeater() || GameState::Menus::IsOnScoreScreens()) || RiffRepeater::currentlyEnabled_Above100) {
		realSongSpeed = RiffRepeater::GetSpeed(true); // While this should almost always be the same value, the user might enable riff repeater, which could cause this number to be wrong.

		DX9DrawText(
			"Song Speed: " + std::to_string(static_cast<int>(roundf(realSongSpeed))) + "%",
			whiteText,
			static_cast<int>(WindowSize.width / 2.0f - WindowSize.width / 38.4f), // 50 pixels left of center in 1920x1080 resolution
			static_cast<int>(WindowSize.height / 54.0f),                          // 20 pixels from top
			static_cast<int>(WindowSize.width / 2.0f + WindowSize.width / 38.4f), // 50 pixels right of center
			static_cast<int>(WindowSize.height / 16.0f),                          // 120 pixels from top
			pDevice,
			{ NULL, NULL },
			DT_CENTER | DT_NOCLIP);
	}
}

void GameOverlay::DisplayNoteByNoteStatus()
{
#if defined(_DEBUG)
	const bool isEnabled = NoteByNoteProbe::IsAutomaticEnabled();

	// J-press acknowledgement, drawn BEFORE the enabled/menu early-return so the
	// flash shows in every state a press is legal in (including N off, section
	// playing - the recommended seek-test state). Two-second flash.
	{
		double secondsAgo = 0.0;
		bool accepted = false;
		if (NoteByNoteProbe::TryGetSeekRequestFlash(secondsAgo, accepted)
			&& secondsAgo < 2.0)
		{
			DX9DrawText(
				accepted ? "NATIVE SEEK requested [J]" : "NATIVE SEEK refused (no probe)",
				accepted ? whiteText : 0xFFFF6666,
				static_cast<int>(WindowSize.width / 96.0f),
				static_cast<int>(WindowSize.height / 9.0f),
				static_cast<int>(WindowSize.width / 2.0f),
				static_cast<int>(WindowSize.height / 6.0f),
				pDevice,
				{ NULL, NULL },
				DT_LEFT | DT_NOCLIP);
		}
	}



	// (The string/fret ML companion overlay moved to DisplayMlStringFretOverlay(), called
	// outside this _DEBUG block so the ML feature draws in Release too.)

	if (!isEnabled && !GameState::Menus::IsInRiffRepeaterMenus()) return;

	DX9DrawText(
		std::string("Note by Note: ") + (isEnabled ? "ON" : "OFF") + "  [N]",
		isEnabled ? whiteText : greyText,
		static_cast<int>(WindowSize.width / 2.0f - WindowSize.width / 8.0f),
		static_cast<int>(WindowSize.height / 13.5f),
		static_cast<int>(WindowSize.width / 2.0f + WindowSize.width / 8.0f),
		static_cast<int>(WindowSize.height / 7.0f),
		pDevice,
		{ NULL, NULL },
		DT_CENTER | DT_NOCLIP);

	const auto targetText = NoteByNoteProbe::GetAutomaticTargetText();
	if (!targetText.empty())
	{
		DX9DrawText(
			targetText,
			whiteText,
			static_cast<int>(WindowSize.width / 2.0f - WindowSize.width / 5.0f),
			static_cast<int>(WindowSize.height / 27.0f),
			static_cast<int>(WindowSize.width / 2.0f + WindowSize.width / 5.0f),
			static_cast<int>(WindowSize.height / 15.0f),
			pDevice,
			{ 0, static_cast<unsigned int>(std::max(24, static_cast<int>(WindowSize.height / 32.0f))) },
			DT_CENTER | DT_NOCLIP);
	}

	if (isEnabled && !IsAudioDiagnosticsVisible())
	{
		NoteByNoteProtocol::NoteByNoteState inputState;
		if (NoteByNoteNativeScoring::TryGetStateSnapshot(inputState)
			&& inputState.structSize >= sizeof(NoteByNoteProtocol::NoteByNoteState)
			&& inputState.detectorSampleValid != 0)
		{
			// The DEAD hint only makes sense while a hold is actively waiting on
			// input; a pause menu legitimately quiets the capture and read as DEAD
			// on the first live session with this line.
			const bool looksDead = inputState.detectorLevelDb < -68.0f
				&& inputState.ownsNativeHold != 0;
			const int deadRedText = 0xFFFF6666;
			std::ostringstream inputLine;
			inputLine << "Input: " << std::fixed << std::setprecision(1)
				<< inputState.detectorLevelDb << " dB";
			if (looksDead) inputLine << "  DEAD? strum - if this stays put, restart the game";
			DX9DrawText(
				inputLine.str(),
				looksDead ? deadRedText : greyText,
				static_cast<int>(WindowSize.width / 96.0f),
				static_cast<int>(WindowSize.height / 22.0f),
				static_cast<int>(WindowSize.width / 2.0f),
				static_cast<int>(WindowSize.height / 14.0f),
				pDevice,
				{ NULL, NULL },
				DT_LEFT | DT_NOCLIP);
		}
	}

	static LONG numeralHeartbeat = 0;
	const bool numeralGatesOpen = isEnabled
		&& D3DHooks::AreFingerNumeralsEnabled()
		&& NoteByNoteHighwayRenderer::IsChordHoldActive();
	int numeralsDrawn = 0;
	int heartbeatAnchorCount = -1;
	if (isEnabled
		&& D3DHooks::AreFingerNumeralsEnabled()
		&& NoteByNoteHighwayRenderer::IsChordHoldActive())
	{
		int anchorStrings[8];
		int anchorFrets[8];
		float anchorX[8];
		float anchorY[8];
		const int anchorCount = D3DHooks::GetFreshFingerAnchors(
			anchorStrings, anchorFrets, anchorX, anchorY);
		heartbeatAnchorCount = anchorCount;
		const int numeralHeight = std::max(18, static_cast<int>(WindowSize.height / 40.0f));
		const int whiteText = 0xFFFFFFFF;
		const int shadowText = 0xC0000000;
		for (int index = 0; index < anchorCount; ++index)
		{
			int finger = 0;
			if (!NoteByNoteHighwayRenderer::TryGetChordFingerForCoordinate(
				anchorStrings[index], anchorFrets[index], finger))
			{
				continue;
			}
			const std::string digit(1, static_cast<char>('0' + finger));
			// Anchors are [0..1] screen fractions (the 3D viewport is smaller than
			// the window on high-res rigs); scale into the overlay's window space.
			const int centreX = static_cast<int>(anchorX[index] * WindowSize.width);
			const int centreY = static_cast<int>(anchorY[index] * WindowSize.height);
			const int halfBox = numeralHeight;
			// A one-pixel shadow first, so the digit reads on the bright marker art.
			DX9DrawText(
				digit,
				shadowText,
				centreX - halfBox + 1,
				centreY - halfBox + 1,
				centreX + halfBox + 1,
				centreY + halfBox + 1,
				pDevice,
				{ 0, static_cast<unsigned int>(numeralHeight) },
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
			DX9DrawText(
				digit,
				whiteText,
				centreX - halfBox,
				centreY - halfBox,
				centreX + halfBox,
				centreY + halfBox,
				pDevice,
				{ 0, static_cast<unsigned int>(numeralHeight) },
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
			++numeralsDrawn;
		}
	}
	if ((InterlockedIncrement(&numeralHeartbeat) % 600) == 1)
	{
		LOG_INFO("(NBN NUMERALS) gates=" << numeralGatesOpen
			<< " (enabled=" << isEnabled
			<< " toggle=" << D3DHooks::AreFingerNumeralsEnabled()
			<< " chordHold=" << NoteByNoteHighwayRenderer::IsChordHoldActive()
			<< ") anchorsFresh=" << heartbeatAnchorCount
			<< " drawn=" << numeralsDrawn << std::endl);
	}
#endif
	// Outside the _DEBUG block: the rest of DisplayNoteByNoteStatus is diagnostic overlay
	// that ships Debug-only, but the bend visualizer is a shipped FEATURE (#49) and must draw
	// in Release too. The meter function is Release-safe on its own (only its internal log
	// lines are _DEBUG-gated), so calling it here makes it render in every configuration.
	DisplayNoteByNoteBendMeter();
	DisplayMlStringFretOverlay();
	DisplayNoteByNoteCornerStatus();
}

void GameOverlay::DisplayNoteByNoteCornerStatus()
{
	if (!OverlayToggles::Get("nbn_corner")) return;
	const bool isEnabled = NoteByNoteProbe::IsAutomaticEnabled();
	const bool inMenus = GameState::Menus::IsInRiffRepeaterMenus();
	if (!isEnabled && !inMenus) return;

	const std::wstring text = inMenus
		? (isEnabled ? L"\u266A NOTE BY NOTE: ON" : L"\u266A NOTE BY NOTE: OFF")
		: L"\u266A NOTE BY NOTE";

	// While enabled the teal breathes between ~0xB4 and full alpha over ~1.6s, so an active
	// feature looks live without the hard flashing that would distract from the highway. The
	// off/menu state is a steady grey. Teal is the Rocksmith+ practice-HUD accent.
	int textColor;
	if (isEnabled)
	{
		const double phase = (GetTickCount64() % 1600) / 1600.0;
		const double wave = 0.5 - 0.5 * std::cos(phase * 6.2831853); // 0 -> 1 -> 0
		const int alpha = 0xB4 + static_cast<int>(wave * (0xFF - 0xB4));
		textColor = (alpha << 24) | 0x3EC9C0;
	}
	else
	{
		textColor = 0xFFB0B0B0;
	}

	// Bold and ~1.4x the old size, bottom-right. Shadow pass then colour pass; no box behind
	// it (no filled-quad primitive is wired into the overlay). DT_RIGHT with DT_NOCLIP anchors
	// to the right edge, so the wide box only needs to be roomy enough for the longest label.
	const int fontSize = std::max(24, static_cast<int>(WindowSize.height / 26.0f));
	const int right = static_cast<int>(WindowSize.width - WindowSize.width / 64.0f);
	const int bottom = static_cast<int>(WindowSize.height - WindowSize.height / 40.0f);
	const int top = bottom - fontSize - 6;
	const int left = static_cast<int>(WindowSize.width * 0.5f);
	DX9DrawTextW(text, 0xC0000000, left + 2, top + 2, right + 2, bottom + 2, pDevice,
		fontSize, DT_RIGHT | DT_NOCLIP, FW_BOLD);
	DX9DrawTextW(text, textColor, left, top, right, bottom, pDevice,
		fontSize, DT_RIGHT | DT_NOCLIP, FW_BOLD);

	if (isEnabled && !inMenus)
	{
		NoteByNoteProtocol::NoteByNoteState st;
		if (NoteByNoteNativeScoring::TryGetStateSnapshot(st)
			&& st.structSize >= sizeof(NoteByNoteProtocol::NoteByNoteState)
			&& st.compareLastValid)
		{
			const int agreeColor = 0xFF3EC96E;   // green
			const int disagreeColor = 0xFFFF5A5A; // red
			const int oneSidedColor = 0xFFFFC24D; // amber
			const int stripFont = std::max(14, static_cast<int>(fontSize * 0.6f));
			const int cellStep = stripFont;                 // one glyph-width per sample
			const uint32_t cells = st.compareHistoryCount;
			const int stripBottom = top - 4;
			const int stripTop = stripBottom - stripFont - 4;
			int cellRight = right;
			for (uint32_t i = 0; i < cells; ++i)
			{
				// history[0] is oldest; draw it leftmost so newest lands at the right edge.
				const uint8_t code = st.compareHistory[cells - 1 - i];
				const int cellColor = code == 1 ? agreeColor
					: (code == 0 ? disagreeColor : oneSidedColor);
				const int cellLeft = cellRight - cellStep;
				DX9DrawTextW(L"■", cellColor, cellLeft, stripTop, cellRight, stripBottom,
					pDevice, stripFont, DT_RIGHT | DT_NOCLIP, FW_BOLD);
				cellRight -= cellStep;
			}

			// A compact running tally so the balance is legible without counting squares.
			std::wostringstream tally;
			tally << L"agree " << st.compareAgreeCount << L" / disagree " << st.compareDisagreeCount;
			const int tallyBottom = stripTop - 2;
			const int tallyTop = tallyBottom - stripFont - 2;
			DX9DrawTextW(tally.str(), 0xFFB0B0B0, left, tallyTop, right, tallyBottom, pDevice,
				stripFont, DT_RIGHT | DT_NOCLIP, FW_NORMAL);
		}
	}
}

// Single source of truth for "the audio diagnostics rows are on screen". The NBN input
// line (DisplayNoteByNoteStatus) shares those rows and yields to this block, so both must
// agree on the gate; keep every visibility condition here.
bool GameOverlay::IsAudioDiagnosticsVisible()
{
	if (!OverlayToggles::Get("audio_diag") || !Audio::CableInput::IsOverlayEnabled()) return false;
	const Audio::CableInput::Diagnostics d = Audio::CableInput::GetDiagnostics();
	return d.installed || d.rsAsio;
}

void GameOverlay::DisplayAudioDiagnostics()
{
	if (!IsAudioDiagnosticsVisible()) return;
	const Audio::CableInput::Diagnostics d = Audio::CableInput::GetDiagnostics();

	std::wostringstream signal;
	if (!d.streamActive)
	{
		signal << L"SIGNAL  waiting for input";
	}
	else if (d.stalled)
	{
		signal << L"NO SIGNAL  the input stream has stalled";
	}
	else
	{
		const double db = d.meterPeak > 0.0f ? 20.0 * std::log10(d.meterPeak) : -90.0;
		const int bars = std::clamp(static_cast<int>((db + 60.0) / 60.0 * 12.0 + 0.5), 0, 12);
		signal << L"SIGNAL  ";
		for (int i = 0; i < 12; ++i) signal << (i < bars ? L'\u2588' : L'\u2591');
		signal << L"  " << std::fixed << std::setprecision(0) << std::max(db, -90.0) << L" dB   "
			<< d.packetsPerSecond << L" pkt/s";
		if (d.dropouts > 0) signal << L"   dropouts " << d.dropouts;
	}

	// Same row grid as the Drop Pedal overlay, with white text and a black shadow.
	const int rowPitch = static_cast<int>(WindowSize.height / 36.0f);
	const int fontSize = std::max(14, static_cast<int>(WindowSize.height / 62.0f));
	const int left = static_cast<int>(WindowSize.width / 96.0f);
	const int right = static_cast<int>(WindowSize.width * 0.8f);
	const int signalTop = static_cast<int>(WindowSize.height / 54.0f) + rowPitch;
	const int bottom = signalTop + rowPitch;
	auto draw = [&](const std::wstring& text, int top)
	{
		DX9DrawTextW(text, 0xFF000000, left + 2, top + 2, right + 2, bottom + 2, pDevice, fontSize, DT_LEFT | DT_NOCLIP, FW_BOLD);
		DX9DrawTextW(text, 0xFFFFFFFF, left, top, right, bottom, pDevice, fontSize, DT_LEFT | DT_NOCLIP, FW_BOLD);
	};
	draw(signal.str(), signalTop);
}

void GameOverlay::DisplayMlStringFretOverlay()
{
	if (!OverlayToggles::Get("ml_fret")) return;

	// MIDI -> note name (73 -> "C#5"), matching the bend meter's convention.
	static const char* const kNoteNames[12] =
		{ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	const auto midiName = [](int midi) -> std::string
	{
		if (midi < 0) return "--";
		return std::string(kNoteNames[midi % 12]) + std::to_string(midi / 12 - 1);
	};

	const int baseX = static_cast<int>(WindowSize.width / 96.0f);
	const int baseY = static_cast<int>(WindowSize.height / 5.2f);
	const int rowH = std::max(18, static_cast<int>(WindowSize.height / 26.0f));
	const int rightEdge = static_cast<int>(WindowSize.width);
	const int mlY = baseY + rowH;

	// --- Line 1: the game's OWN detection (the "original" readout). detectorQuality is the
	// value its accept gate compares against 50; a note can read the correct pitch here yet
	// sit below that gate, which is exactly when native refuses a note ML confirms. ---
	NoteByNoteProtocol::NoteByNoteState st;
	const bool haveState = NoteByNoteController::TryGetNoteByNoteState(st) && st.isInitialized;
	std::ostringstream nativeLine;
	nativeLine << "Native:";
	int nativeColor = 0xFFFFFFFF; // white
	if (haveState && st.detectorSampleValid != 0)
	{
		const int heard = st.soundingMidi >= 0.0f
			? static_cast<int>(st.soundingMidi + 0.5f)
			: st.detectorLoudestMidi;
		nativeLine << "  " << midiName(heard)
			<< "  q" << static_cast<int>(st.detectorQuality + 0.5f) << "/50";
		if (st.expectedMidi >= 0)
			nativeLine << "   exp " << midiName(st.expectedMidi);
		// Amber = a hold is waiting and the read is below the accept gate: the precise
		// "game heard it but refused it" moment ML exists to rescue.
		if (st.ownsNativeHold != 0 && st.detectorQuality < 50.0f)
			nativeColor = 0xFFFFCC66;
	}
	else
	{
		nativeLine << "  --";
		nativeColor = 0xFF999999;
	}
	DX9DrawText(nativeLine.str(), nativeColor, baseX, baseY, rightEdge, baseY + rowH,
		pDevice, { NULL, NULL }, DT_LEFT | DT_NOCLIP);

	// --- Line 2: the FretNet ML companion's read of the same audio, directly under it. ---
	MlStringFretReader::StringFret sf;
	if (!MlStringFretReader::TryGet(sf))
	{
		DX9DrawText("ML: (service offline)", 0xFF888888, baseX, mlY, rightEdge, mlY + rowH,
			pDevice, { NULL, NULL }, DT_LEFT | DT_NOCLIP);
		return;
	}
	if (haveState && st.ownsNativeHold && st.selectedChordId == -1 && !st.isBendTarget
		&& sf.shift == DropPedal::GetAppliedInputShiftSemitones())
	{
		// Resolve equivalent pitches only in this display copy; scoring keeps its evidence.
		MlFretDisplay::ResolveExpectedPosition(sf, st.selectedString,
			st.selectedFret, st.expectedMidi, 0.5f);
	}
	static const char* const kStringLabels[6] = { "E", "A", "D", "G", "B", "e" };
	std::ostringstream mlLine;
	mlLine << "ML:";
	bool anyString = false;
	for (int s = 0; s < 6; ++s)
	{
		if (sf.physFret[s] < 0) continue;
		anyString = true;
		mlLine << "  " << kStringLabels[s] << sf.physFret[s]
			<< "=" << MlStringFretReader::NoteNameForStringFret(s, sf.fret[s]);
	}
	if (!anyString) mlLine << "  (silent)";
	if (sf.shift != 0)
		mlLine << "   [shift " << (sf.shift > 0 ? "+" : "") << sf.shift << "]";
	DX9DrawText(mlLine.str(), 0xFF66CCFF, baseX, mlY, rightEdge, mlY + rowH,
		pDevice, { NULL, NULL }, DT_LEFT | DT_NOCLIP);
}

void GameOverlay::DisplayNoteByNoteBendMeter()
{
	if (!OverlayToggles::Get("bend_meter")) return;
	if (!NoteByNoteProbe::IsAutomaticEnabled()) return;

	NoteByNoteProtocol::NoteByNoteState state;
	if (!NoteByNoteNativeScoring::TryGetStateSnapshot(state))
	{
#if defined(_DEBUG)
		static LONG noStateBudget = 5;
		if (InterlockedDecrement(&noStateBudget) >= 0)
			LOG_INFO("(NBN BEND METER) no probe state available." << std::endl);
#endif
		return;
	}
	// Appended-field guard: a probe older than the bend-visualizer fields reports
	// a smaller structSize and must not have these bytes interpreted.
	if (state.structSize < sizeof(NoteByNoteProtocol::NoteByNoteState))
	{
#if defined(_DEBUG)
		static LONG structSizeBudget = 5;
		if (InterlockedDecrement(&structSizeBudget) >= 0)
			LOG_INFO("(NBN BEND METER) probe structSize " << state.structSize
				<< " predates the bend fields (host expects "
				<< sizeof(NoteByNoteProtocol::NoteByNoteState) << ")." << std::endl);
#endif
		return;
	}
#if defined(_DEBUG)
	// Diagnostics, one budget PER REASON (a shared budget drained in the opening frames
	// and explained nothing). The heartbeat proves the function runs and shows what
	// bendTargetMidi actually is over time; the drawing budget refills on every bend's
	// first frame so every bend leaves evidence in the log.
	static LONG meterHeartbeat = 0;
	static LONG drawingBudget = 10;
	static LONG lastBendTarget = -1;
	if ((InterlockedIncrement(&meterHeartbeat) % 600) == 1)
		LOG_INFO("(NBN BEND METER) alive: bendTarget=" << state.bendTargetMidi
			<< " bendBase=" << state.bendBaseMidi
			<< " sounding=" << state.soundingMidi
			<< " phase=" << static_cast<int>(state.gatePhase)
			<< " holds=" << state.ownsNativeHold << std::endl);
	const LONG thisBendTarget = state.bendTargetMidi;
	if (thisBendTarget != InterlockedExchange(&lastBendTarget, thisBendTarget)
		&& thisBendTarget >= 0)
	{
		InterlockedExchange(&drawingBudget, 3);
	}
	if (state.bendTargetMidi >= 0 && InterlockedDecrement(&drawingBudget) >= 0)
		LOG_INFO("(NBN BEND METER) drawing: base=" << state.bendBaseMidi
			<< " target=" << state.bendTargetMidi
			<< " sounding=" << state.soundingMidi
			<< " quality=" << state.soundingQuality << std::endl);
#endif
	if (state.bendTargetMidi < 0) return;

	static const char* NOTE_NAMES[12] =
		{ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	const auto noteName = [](int midi) -> std::string
	{
		if (midi < 0) return "?";
		return std::string(NOTE_NAMES[midi % 12]) + std::to_string(midi / 12 - 1);
	};

	const int base = state.bendBaseMidi >= 0 ? state.bendBaseMidi : (state.bendTargetMidi - 2);
	const int target = state.bendTargetMidi;
	const float depth = static_cast<float>(target - base);

	// Anti-flicker: only a read that is PLAUSIBLE for this bend (a little below the base to a
	// little above the target) and not too weak moves the needle; a sub-harmonic or a stray
	// tracker pitch far outside that band is a bad frame that used to fling the needle to a rail.
	// The last good value is held through a few bad frames, and a light low-pass removes jitter.
	static int meterTarget = -1;
	static float displaySounding = -1.0f;
	static int staleFrames = 0;
	static float lastRecordTime = -1.0e30f;
	if (target != meterTarget || state.selectedRecordTime != lastRecordTime)
	{
		meterTarget = target;
		lastRecordTime = state.selectedRecordTime;
		displaySounding = -1.0f;
		staleFrames = 0;
	}
	const float bandLow = static_cast<float>(base) - 1.0f;
	const float bandHigh = static_cast<float>(target) + 1.5f;
	bool haveNeedle = false;
	constexpr float BEND_PIN_RELEASE = 0.25f;
	if (state.soundingMidi >= bandLow && state.soundingMidi <= bandHigh
		&& state.soundingQuality >= 20.0f)
	{
		if (displaySounding < 0.0f)
		{
			displaySounding = state.soundingMidi < static_cast<float>(base)
				? state.soundingMidi
				: static_cast<float>(base);
			staleFrames = 0;
			haveNeedle = true;
		}
		else
		{
			if (state.soundingMidi > displaySounding)
				displaySounding = state.soundingMidi;
			else
				displaySounding += (state.soundingMidi - displaySounding) * BEND_PIN_RELEASE;
			staleFrames = 0;
			haveNeedle = true;
		}
	}
	// Signal gone/weak: ease the pin back toward base (the "back to zero" release) with the same
	// tween so the return feels consistent, instead of freezing at the high point. It keeps
	// drawing for a short window while it glides home, then clears below.
	else if (displaySounding >= 0.0f && ++staleFrames < 10)
	{
		displaySounding += (static_cast<float>(base) - displaySounding) * BEND_PIN_RELEASE;
		haveNeedle = true;
	}
	else
	{
		// Signal gone: drop the pin AND clear the held value so the next bend re-seeds at the
		// base instead of resuming from the previous bend's top.
		displaySounding = -1.0f;
		staleFrames = 0;
	}

	const float frac = depth > 0.0f
		? (displaySounding - static_cast<float>(base)) / depth
		: 0.0f;
	const bool rawOnTarget = state.soundingMidi >= bandLow && state.soundingMidi <= bandHigh
		&& state.soundingQuality >= 20.0f
		&& state.soundingMidi >= static_cast<float>(target) - 0.3f;
	const bool onTarget = haveNeedle
		&& (displaySounding >= static_cast<float>(target) - 0.3f || rawOnTarget);

	constexpr int ROWS = 12;
	constexpr int TARGET_ROW = 2;            // rows above the target are overbend headroom
	const int baseRow = ROWS - 1;
	const int travel = baseRow - TARGET_ROW; // rows from base up to target
	int needleRow = baseRow - static_cast<int>(std::lround(frac * static_cast<float>(travel)));
	if (needleRow < 0) needleRow = 0;
	if (needleRow > baseRow) needleRow = baseRow;

	const int rowHeight = std::max(16, static_cast<int>(WindowSize.height / 40.0f));
	const int left = static_cast<int>(WindowSize.width * 0.68f);
	const int right = static_cast<int>(WindowSize.width * 0.90f);
	int top = static_cast<int>(WindowSize.height * 0.28f);
	const int goldText = 0xFFFFD24C;
	const int greenText = 0xFF66FF66;

	for (int r = 0; r < ROWS; ++r)
	{
		const bool isTargetRow = (r == TARGET_ROW);
		const bool isBaseRow = (r == baseRow);
		const bool isNeedle = haveNeedle && (r == needleRow);
		std::ostringstream row;
		row << (isTargetRow || isBaseRow ? "==" : " |");
		if (isTargetRow) row << " " << noteName(target) << " target";
		else if (isBaseRow) row << " " << noteName(base) << " base";
		if (isNeedle) row << "   <==";
		const int colour = isNeedle
			? (onTarget ? greenText : whiteText)
			: (isTargetRow ? goldText : greyText);
		DX9DrawText(row.str(), colour, left, top, right, top + rowHeight, pDevice,
			{ NULL, NULL }, DT_LEFT | DT_NOCLIP);
		top += rowHeight;
	}
}

void GameOverlay::DisplayCurrentTuningForAutoTune()
{
	if (Settings::ReturnSettingValue("AutoTuneForSong") == "on" && Settings::GetKeyBind("TuningOffsetKey") != NULL && GameState::Menus::IsInTuningMenus()) {
		DX9DrawText(
			"Auto Tune For: " + Midi::GetTuningOffsetName(Midi::tuningOffset),
			whiteText,
			static_cast<int>(WindowSize.width / 5.5),		// 349 pixels left of the center in 1920x1080 resolution
			static_cast<int>(WindowSize.height / 30.85),	// 35 pixels from the top
			static_cast<int>(WindowSize.width / 5.65),		// 339 pixels right of center
			static_cast<int>(WindowSize.height / 8),		// 135 pixels from the top
			pDevice);
	}
}

void GameOverlay::DisplayLoopStartEndTimes(float loopStart, float loopEnd)
{
	DX9DrawText(
		"Loop: " + D3DHooks::ConvertFloatTimeToStringTime(loopStart) + " - " + D3DHooks::ConvertFloatTimeToStringTime(loopEnd),
		whiteText,
		static_cast<int>(WindowSize.width / 2.0f - WindowSize.width / 38.4f), // 50 pixels left of center in 1920x1080 resolution
		static_cast<int>(WindowSize.height / 21.6f),                          // 50 pixels from top
		static_cast<int>(WindowSize.width / 2.0f + WindowSize.width / 38.4f), // 50 pixels right of center
		static_cast<int>(WindowSize.height / 7.2f),                           // 150 pixels from top
		pDevice,
		{ NULL, NULL },
		DT_CENTER | DT_NOCLIP);
}

void HandleLooping() {
	if (Settings::ReturnSettingValue("AllowLooping") == "on" && (Keybindings::loopStart != NULL || Keybindings::loopEnd != NULL)) {
		// Only enable looping in learn a song modes (learn a song & non-stop play)
		if (GameState::Menus::IsInLearnASongModes()) {
			GameOverlay::DisplayLoopStartEndTimes(Keybindings::loopStart, Keybindings::loopEnd);

			// Prevent the user from creating a loop that starts at a negative timestamp.
			if ((Settings::GetModSetting("LoopingLeadUp") / 1000.f) >= Keybindings::loopStart) {
				Keybindings::roughLoopStart = 0.f;
			}
			else {
				Keybindings::roughLoopStart = Keybindings::loopStart - (Settings::GetModSetting("LoopingLeadUp") / 1000.f);
			}

			// If we are paused, reset the grey note timer.
			if (GameState::Menus::IsInLearnASongPauseModes()) {
				// Resets grey note timer to loopStart. This makes it so notes in the loop are not deactivated.
				// Deactivated notes are greyed out, and do not register with note detection.
				// As an added bonus the game also automatically adds a bit of lead time so the player has some time to prepare.
				if (SongTimer::GetGreyNoteTimer() != Keybindings::loopStart) {
					SongTimer::SetGreyNoteTimer(Keybindings::loopStart);
				}
			}

			// If not paused AND we are at the end of the loop, seek to the start of the loop.
			else if (Keybindings::loopStart != NULL && Keybindings::loopEnd != NULL && (SongTimer::SongTimer() >= Keybindings::loopEnd)) {
				Wwise::SoundEngine::SeekOnEvent(std::string("Play_" + GameState::GetSongKey()).c_str(), 0x1234, (AkTimeMs)(Keybindings::roughLoopStart * 1000), false);
			}
		}
		// Difference between learnASongModes & fastRRModes is the inclusion of RR. This means that this check is only gets the RR menus.
		else if (GameState::Menus::IsInModesWithAllowedFastRiffRepeater()) {
			// Reset loopStart and loopEnd to NULL as the user wants to do a loop with RR, or is changing some settings.
			Keybindings::loopStart = NULL;
			Keybindings::loopEnd = NULL;
		}
	}

}

static int MeasureLineHeight(ID3DXFont* font, const std::string& text, const RECT& rect, DWORD fmt) {
	RECT r = rect;
	int h = font->DrawTextA(nullptr, text.c_str(), -1, &r, fmt | DT_CALCRECT, 0);
	if (h <= 0) h = (r.bottom - r.top);
	return h;
}

static float ReadAccuracy() {
	const bool isLAS = GameState::Menus::IsInLearnASongModes();
	const bool isSA = GameState::Menus::IsInScoreAttackModes();

	uintptr_t addr = 0;
	if (isLAS) {
		addr = MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_noteData,
			Offsets::ptr_noteDataOffsets);
	}
	else if (isSA) {
		addr = MemUtil::FindDMAAddy(Offsets::baseHandle + Offsets::ptr_scoreAttackNoteData,
			Offsets::ptr_scoreAttackNoteDataOffsets);
	}
	else {
		return 0.0f;
	}

	if (!addr) return 0.0f;

	if (isLAS) {
		const LearnASongNoteData* data = reinterpret_cast<LearnASongNoteData*>(addr);

		return data->getAccuracy();
	}
	else if (isSA) {
		const ScoreAttackNoteData* data = reinterpret_cast<ScoreAttackNoteData*>(addr);
		return data->getAccuracy();
	}

	return 0.0f;
}

void GameOverlay::DisplaySongAccuracy() {
	if (Settings::ReturnSettingValue("DisplayCurrentAccuracy") == "on" &&
		GameState::IsInSong() && SongTimer::SongTimer() != 0.f) {
		auto left = static_cast<int>(WindowSize.width - WindowSize.width / 16.0f);
		auto right = static_cast<int>(WindowSize.width - WindowSize.width / 96.0f);
		auto top = static_cast<int>(WindowSize.height / 54.0f);
		auto bottom = static_cast<int>(WindowSize.height / 16.0f);
		RECT baseRect{ left, top, right, bottom };

		float accuracy = ReadAccuracy();
		std::stringstream ss;
		ss << std::fixed << std::setprecision(2) << accuracy << "%";
		std::string accuracyText = ss.str();

		if (cachedFont) {
			int lh = MeasureLineHeight(cachedFont, accuracyText, baseRect, DT_RIGHT | DT_NOCLIP);
			int gap = (std::max)(1, lh / 4);
			top += lh + 2 * gap;
			bottom += lh + 2 * gap;
		}
		else { //JIC
			auto line = static_cast<int>(WindowSize.height / 54.0f);
			top += line;
			bottom += line;
		}

		DX9DrawText(
			accuracyText,
			whiteText,
			left, top, right, bottom,
			pDevice,
			{ NULL, NULL },
			DT_RIGHT | DT_NOCLIP);
	}
}

void GameOverlay::CheckCurrentFont() {
	const std::string currentFontName = Settings::ReturnSettingValue("OnScreenFont");
	const int currentFontSize = Settings::GetModSetting("OnScreenFontSize");

	if (cachedFontName != currentFontName || cachedFontSize != currentFontSize || !cachedFont) {
		LOG_INFO("Font settings changed. Re-caching default font..." << std::endl);

		FontKey newKey = FontKey::Make(currentFontName, currentFontSize, 0, FW_NORMAL, false);
		CComPtr<ID3DXFont> newFont;

		if (fontCache.Get(pDevice, newKey, newFont)) {
			cachedFont = newFont;
			cachedFontName = currentFontName;
			cachedFontSize = currentFontSize;
		}
		else {
			LOG_ERROR("Failed to create and cache new default font!" << std::endl);
		}
	}
}

void GameOverlay::RenderOverlay(IDirect3DDevice9* device) {
	// Draw text on screen
	// NOTE: NEVER USE SET VALUES. Always do division of WindowSize width AND heigh so every resolution should have the text in around the same spot.
	if (GameState::GameLoaded) {
		WindowSize = GetWindowSize();
		pDevice = device;

		CheckCurrentFont();

		DisplayMixer();
		DisplaySongTimer();
		DisplayRiffRepeaterOverHundredPercentSpeed();
		DisplayNoteByNoteStatus();
		DisplayAudioDiagnostics();
		DisplayCurrentNote();
		DisplayCurrentTuningForAutoTune();
		static DropPedal::Overlay dropPedalOverlay;
		dropPedalOverlay.Render(cachedFont, WindowSize);
		DisplaySongAccuracy();

		HandleLooping();
	}
}
