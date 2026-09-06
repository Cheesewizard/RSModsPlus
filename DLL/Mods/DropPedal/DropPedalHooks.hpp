#pragma once

#include "DropPedalPlayer.hpp"

namespace DropPedalHooks
{
	void Install();
	void Poll();
	// Decoder-thread-safe request; the game-loop poll restores tuning state.
	void QueuePitchRestore();
	void SetInputShifterActive(bool active);
	bool IsInputShifterActive();
	bool TryGetAuthoredTrueTuning(float& trueTuning);
	void ReportInputShifterUnavailable();
	unsigned long long GetInputNoticeTick();
	void HandleArrangementTuning();
	void ResetSongState();
}
