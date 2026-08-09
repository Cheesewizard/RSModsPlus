#pragma once

#include "DropPedalPlayer.hpp"

#include <array>
#include <string>
#include "DropPedal.hpp"

struct ID3DXFont;
struct Resolution;

namespace DropPedal
{
	class Overlay final
	{
	public:
		static void LoadSettings();

		void Render(ID3DXFont* font, const Resolution& windowSize);

	private:
		std::array<bool, PLAYER_COUNT> hasCachedTuningState{};
		std::array<PitchMode, PLAYER_COUNT> cachedPitchModes{};
		std::array<bool, PLAYER_COUNT> cachedMissingPedalTone{};
		std::array<int, PLAYER_COUNT> cachedTargetSemitones{};
		std::array<int, PLAYER_COUNT> cachedBaseTuningSemitones{};
		std::array<ID3DXFont*, PLAYER_COUNT> cachedTuningFonts{};
		std::array<unsigned long long, PLAYER_COUNT> cachedTuningColorRevisions{};
		std::array<std::string, PLAYER_COUNT> tuningLines;
		std::array<unsigned int, PLAYER_COUNT> tuningTextColors{};

		bool hasCachedEngineState = false;
		bool cachedInputShifterActive = false;
		ID3DXFont* cachedEngineFont = nullptr;
		std::string engineLine;
		unsigned long long lastSeenNoticeTick = 0;
		unsigned long long engineDisplayStartTick = 0;

		void RenderTuning(ID3DXFont* font, const Resolution& windowSize, Player player, int row);
		void RenderEngine(ID3DXFont* font, const Resolution& windowSize, bool isMultiplayer);
		void UpdateTuningCache(ID3DXFont* font, Player player);
		void UpdateEngineCache(ID3DXFont* font);
		void DrawShadowedText(
			ID3DXFont* font,
			const Resolution& windowSize,
			const std::string& text,
			unsigned int textColor,
			int topLeftX,
			int topLeftY,
			int bottomRightX,
			int bottomRightY) const;
	};
}
