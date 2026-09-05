#include "stdafx.h"
#include "D3DOverlay.hpp"
#include "Mods/DropPedal/DropPedalOverlay.hpp"
#include "Mods/DropPedal/DropPedal.hpp"
#include "Audio/CableInput.hpp"

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
// having been created through D3DXCreateFontA, so a glyph like U+266A renders correctly.
void GameOverlay::DX9DrawTextW(const std::wstring& textToDraw, int textColorHex, int topLeftX, int topLeftY, int bottomRightX, int bottomRightY, LPDIRECT3DDEVICE9 pDevice, int fontHeight, DWORD format, int weight)
{
	CComPtr<ID3DXFont> font;
	const std::string face = Settings::ReturnSettingValue("OnScreenFont");
	FontKey key = FontKey::Make(face, fontHeight, 0, weight, false);
	if (!fontCache.Get(pDevice, key, font)) {
		LOG_ERROR("Could not acquire wide-text font." << std::endl);
		return;
	}

	RECT TextRectangle{ topLeftX, topLeftY, bottomRightX, bottomRightY };
	font->DrawTextW(nullptr, textToDraw.c_str(), -1, &TextRectangle, format, textColorHex);
}

// Audio diagnostics overlay: latency and signal figures for the guitar input path. Two
// rows, top-left, on the same grid as the Drop Pedal overlay:
//   IN 10.0 ms measured (buf 10.0)   OUT 3.0 ms   TOTAL 13.0 ms   modern raw 48 kHz / exclusive out
//   SIGNAL  [########----]  -18 dB   100 pkt/s
// Input latency is measured from the device timestamp to the game's hand-off, with the
// buffer arithmetic in brackets; output latency is the game's own figure from
// audiodump.txt, so nothing here is estimated. Colour: green while a signal is present,
// grey idle, amber after dropouts, red when the stream has stalled.
void GameOverlay::DisplayAudioDiagnostics()
{
	if (!Audio::CableInput::IsOverlayEnabled()) return;
	const Audio::CableInput::Diagnostics d = Audio::CableInput::GetDiagnostics();
	if (!d.installed && !d.rsAsio) return;

	auto widen = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };

	// Line 1: the numbers a player cares about, big and bold. IN is one served chunk (the
	// driver period), OUT is the game's own figure from audiodump.txt, TOTAL is their sum.
	// (The Drop Pedal input pitch shifter, whose live latency would add here, is not part of
	// this build, so no pedal-latency term is shown.)
	const uint32_t pedalFrames = 0;
	const double pedalMs = pedalFrames > 0 ? 1000.0 * pedalFrames / 48000.0 : 0.0;

	// IN prefers the MEASURED figure (device timestamp to game hand-off, newest sample) and
	// shows the buffer arithmetic in brackets; a polled stock stream can be far fresher than
	// its 22 ms buffer suggests, and only the measurement settles that.
	std::wostringstream latency;
	latency << std::fixed << std::setprecision(1);
	const double bufferIn = d.rsAsio ? 0.0 : (d.inputLatencyMs > 0.0 ? d.inputLatencyMs : d.inputLatencyGameMs);
	const double in = d.measuredValid ? d.measuredInputMs : bufferIn;
	if (d.rsAsio) latency << L"IN RS_ASIO";
	else if (d.measuredValid)
	{
		latency << L"IN " << d.measuredInputMs << L" ms measured";
		if (bufferIn > 0.0) latency << L" (buf " << bufferIn << L")";
	}
	else if (in > 0.0) latency << L"IN " << in << L" ms buf";
	else latency << L"IN --";
	if (pedalMs > 0.0) latency << L"   PEDAL +" << pedalMs << L" ms";
	if (d.outputKnown)
	{
		latency << L"   OUT " << d.outputLatencyMs << L" ms";
		if (in > 0.0) latency << L"   TOTAL " << (in + pedalMs + d.outputLatencyMs) << L" ms";
	}
	if (!d.rsAsio && !d.inputPath.empty())
	{
		latency << L"   " << widen(d.inputPath);
		if (!d.deviceFormat.empty()) latency << L" " << widen(d.deviceFormat);
	}
	if (d.outputKnown) latency << (d.outputExclusive ? L" / exclusive out" : L" / shared out");

	// Line 2: a block meter with the level, or the fault state in red.
	std::wostringstream signal;
	int signalColor = 0xFFC8C8C8;
	if (d.rsAsio)
	{
		signal << L"SIGNAL  handled by RS_ASIO";
	}
	else if (!d.streamActive)
	{
		signal << L"SIGNAL  waiting for the cable";
	}
	else if (d.stalled)
	{
		signal << L"NO SIGNAL  the input stream has stalled";
		signalColor = 0xFFFF5A5A;
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
		if (db > -40.0) signalColor = 0xFF7EE07E;
		if (d.dropouts > 0) signalColor = 0xFFFFB050;
	}

	// Same row grid as the Drop Pedal overlay (row pitch height/36 from height/54), rows
	// 1 and 2, bold, with a solid two-pixel shadow so it reads over the bright menu wall.
	const int rowPitch = static_cast<int>(WindowSize.height / 36.0f);
	const int fontSize = std::max(14, static_cast<int>(WindowSize.height / 62.0f));
	const int left = static_cast<int>(WindowSize.width / 96.0f);
	const int right = static_cast<int>(WindowSize.width * 0.8f);
	const int line1Top = static_cast<int>(WindowSize.height / 54.0f) + rowPitch;
	const int line2Top = line1Top + rowPitch;
	const int bottom = line2Top + rowPitch;
	auto draw = [&](const std::wstring& text, int top, int color)
	{
		DX9DrawTextW(text, 0xFF000000, left + 2, top + 2, right + 2, bottom + 2, pDevice, fontSize, DT_LEFT | DT_NOCLIP, FW_BOLD);
		DX9DrawTextW(text, color, left, top, right, bottom, pDevice, fontSize, DT_LEFT | DT_NOCLIP, FW_BOLD);
	};
	draw(latency.str(), line1Top, 0xFF3EC9C0);
	draw(signal.str(), line2Top, signalColor);
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
		DisplayCurrentNote();
		DisplayCurrentTuningForAutoTune();
		static DropPedal::Overlay dropPedalOverlay;
		dropPedalOverlay.Render(cachedFont, WindowSize);
		DisplaySongAccuracy();
		DisplayAudioDiagnostics();

		HandleLooping();
	}
}
