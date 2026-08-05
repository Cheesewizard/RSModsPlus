#include "../../stdafx.h"
#include "DropPedalOverlay.hpp"

#include "../../GameState.hpp"
#include "../../Resolution.h"
#include "DropPedal.hpp"

namespace
{
	constexpr unsigned int WHITE_TEXT = 0xFFFFFFFF;
	constexpr unsigned int DROP_PEDAL_DOWN_TEXT = 0xFF6BE06B;
	constexpr unsigned int DROP_PEDAL_UP_TEXT = 0xFFFFC24D;
	constexpr unsigned int DROP_PEDAL_DISABLED_TEXT = 0xFFC8C8C8;
	constexpr unsigned long long ENGINE_SHOW_MILLISECONDS = 6000;
	constexpr unsigned long long ENGINE_FADE_MILLISECONDS = 1500;

}

void DropPedal::Overlay::Render(ID3DXFont* font, const Resolution& windowSize)
{
	if (!DropPedal::IsConfiguredEnabled() || !font) return;

	// Speaker Mode shifts the one shared song mix, so Player 2 has nothing to
	// control and only Player One's row renders.
	const bool showsPlayerTwoRow = GameState::IsMultiplayer()
		&& DropPedal::GetPitchMode() != PitchMode::SpeakerMode;
	RenderTuning(font, windowSize, Player::One, 0);
	if (showsPlayerTwoRow)
	{
		RenderTuning(font, windowSize, Player::Two, 1);
	}

	RenderEngine(font, windowSize, showsPlayerTwoRow);
}

void DropPedal::Overlay::RenderTuning(
	ID3DXFont* font,
	const Resolution& windowSize,
	Player player,
	int row)
{
	UpdateTuningCache(font, player);
	const size_t playerIndex = GetPlayerIndex(player);
	const int top = static_cast<int>(windowSize.height / 54.0f)
		+ row * static_cast<int>(windowSize.height / 36.0f);

	DrawShadowedText(
		font,
		windowSize,
		tuningLines[playerIndex],
		tuningTextColors[playerIndex],
		static_cast<int>(windowSize.width / 96.0f),
		top,
		static_cast<int>(windowSize.width / 3.0f),
		top + static_cast<int>(windowSize.height / 27.0f));
}

void DropPedal::Overlay::RenderEngine(
	ID3DXFont* font,
	const Resolution& windowSize,
	bool isMultiplayer)
{
	if (GameState::currentMenu != "MainMenu") return;

	const unsigned long long noticeTick = DropPedal::GetEngineNoticeTick();
	if (noticeTick == 0) return;

	// The engine is decided before the overlay font is ready, so the notice timer
	// begins on the first frame that can render it. A later engine transition resets it.
	if (noticeTick != lastSeenNoticeTick)
	{
		lastSeenNoticeTick = noticeTick;
		engineDisplayStartTick = 0;
	}

	if (engineDisplayStartTick == 0)
	{
		engineDisplayStartTick = GetTickCount64();
	}

	const unsigned long long elapsed = GetTickCount64() - engineDisplayStartTick;
	if (elapsed >= ENGINE_SHOW_MILLISECONDS + ENGINE_FADE_MILLISECONDS) return;

	UpdateEngineCache(font);

	const float fade = elapsed < ENGINE_SHOW_MILLISECONDS
		? 1.0f
		: 1.0f - (float)(elapsed - ENGINE_SHOW_MILLISECONDS) / ENGINE_FADE_MILLISECONDS;

	const int left = static_cast<int>(windowSize.width / 96.0f);
	const int top = static_cast<int>(windowSize.height / (isMultiplayer ? 10.8f : 18.0f));

	DrawShadowedText(
		font,
		windowSize,
		engineLine,
		D3DCOLOR_ARGB(static_cast<int>(255 * fade), 255, 255, 255),
		left,
		top,
		left + static_cast<int>(windowSize.width / 3.0f),
		top + static_cast<int>(windowSize.height / 27.0f));
}

void DropPedal::Overlay::UpdateTuningCache(ID3DXFont* font, Player player)
{
	const size_t playerIndex = GetPlayerIndex(player);
	const auto pitchMode = DropPedal::GetPitchMode();
	const bool isCableOwned = !DropPedal::IsInputShifterActive();
	const int targetSemitones = DropPedal::GetTargetSemitones(player);
	const int baseTuningSemitones = DropPedal::GetBaseTuningSemitones(player);

	// Each row shows its own player's configured state, like hardware. The one
	// exception is Cable's tone dependency: the game reloads tones every song,
	// so the row flags a tone the pedal cannot act on.
	const bool isMissingPedalTone = isCableOwned
		&& pitchMode == PitchMode::DropPedal
		&& !DropPedal::HasLivePedalTone(player);

	if (hasCachedTuningState[playerIndex]
		&& pitchMode == cachedPitchModes[playerIndex]
		&& isMissingPedalTone == cachedMissingPedalTone[playerIndex]
		&& targetSemitones == cachedTargetSemitones[playerIndex]
		&& baseTuningSemitones == cachedBaseTuningSemitones[playerIndex]
		&& font == cachedTuningFonts[playerIndex])
	{
		return;
	}

	// Speaker Mode moves the one song mix along Player One's route; it renders as
	// a single global row, so only Player One's cache ever sees that mode.
	std::string& tuningLine = tuningLines[playerIndex];
	unsigned int& tuningTextColor = tuningTextColors[playerIndex];
	if (pitchMode == PitchMode::Off)
	{
		tuningLine = "Pitch: Off";
	}
	else if (pitchMode == PitchMode::SpeakerMode)
	{
		tuningLine = "Speaker: " + DropPedal::GetPitchRouteName();
	}
	else if (isMissingPedalTone)
	{
		tuningLine = "Drop: No pedal in tone";
	}
	else
	{
		tuningLine = "Drop: " + DropPedal::GetTuningName(player);
	}
	tuningTextColor = DROP_PEDAL_DISABLED_TEXT;

	if (pitchMode != PitchMode::Off && !isMissingPedalTone)
	{
		const int direction = DropPedal::GetShiftDirection(player);
		tuningTextColor = direction < 0
			? DROP_PEDAL_DOWN_TEXT
			: (direction > 0 ? DROP_PEDAL_UP_TEXT : WHITE_TEXT);
	}

	cachedPitchModes[playerIndex] = pitchMode;
	cachedMissingPedalTone[playerIndex] = isMissingPedalTone;
	cachedTargetSemitones[playerIndex] = targetSemitones;
	cachedBaseTuningSemitones[playerIndex] = baseTuningSemitones;
	cachedTuningFonts[playerIndex] = font;
	hasCachedTuningState[playerIndex] = true;
	font->PreloadTextA(tuningLine.c_str(), static_cast<int>(tuningLine.length()));
}

void DropPedal::Overlay::UpdateEngineCache(ID3DXFont* font)
{
	const bool isInputShifterActive = DropPedal::IsInputShifterActive();
	if (hasCachedEngineState
		&& isInputShifterActive == cachedInputShifterActive
		&& font == cachedEngineFont)
	{
		return;
	}

	engineLine = isInputShifterActive ? "Engine: ASIO Drop Pedal" : "Engine: Cable Drop Pedal";
	cachedInputShifterActive = isInputShifterActive;
	cachedEngineFont = font;
	hasCachedEngineState = true;
	font->PreloadTextA(engineLine.c_str(), static_cast<int>(engineLine.length()));
}

void DropPedal::Overlay::DrawShadowedText(
	ID3DXFont* font,
	const Resolution& windowSize,
	const std::string& text,
	unsigned int textColor,
	int topLeftX,
	int topLeftY,
	int bottomRightX,
	int bottomRightY) const
{
	RECT textRectangle{ topLeftX, topLeftY, bottomRightX, bottomRightY };
	RECT shadowRectangle = textRectangle;
	int shadowOffset = static_cast<int>(windowSize.height / 540);
	if (shadowOffset < 1) shadowOffset = 1;
	OffsetRect(&shadowRectangle, shadowOffset, shadowOffset);

	font->DrawTextA(
		nullptr,
		text.c_str(),
		-1,
		&shadowRectangle,
		DT_LEFT | DT_NOCLIP,
		D3DCOLOR_ARGB(230, 0, 0, 0));
	font->DrawTextA(nullptr, text.c_str(), -1, &textRectangle, DT_LEFT | DT_NOCLIP, textColor);
}
