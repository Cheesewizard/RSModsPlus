#pragma once

#include "DropPedalPlayer.hpp"

#include <string>
#include "DropPedal.hpp"

namespace DropPedalState
{
	void Configure(const std::string& enabledSetting, const std::string& engineSetting);
	bool IsConfiguredEnabled();
	bool IsAsioEngine();
	bool IsCableEngine();
	bool IsEnabled();
	bool IsSpeakerModeEnabled();
	DropPedal::PitchMode GetPitchMode();
	bool TryCyclePitchMode(DropPedal::PitchMode& nextMode);
	bool DisableSpeakerMode();
	void SetGameplayInProgress(bool isGameplay);
	bool AreSpeakerControlsLocked();
	void SetSpeakerTargetSynchronized(bool isSynchronized);
	bool IsSpeakerTargetSynchronized();
	bool AdjustTarget(DropPedal::Player player, int semitoneDelta);
	bool SetTargetSemitones(DropPedal::Player player, int semitones);
	bool AdjustBaseTuning(DropPedal::Player player, int semitoneDelta);
	int GetTargetSemitones(DropPedal::Player player);
	int GetBaseTuningSemitones(DropPedal::Player player);
	float GetTargetCents(DropPedal::Player player);
	std::string GetTuningName(DropPedal::Player player);
	std::string GetBaseTuningName(DropPedal::Player player);
	std::string GetAbsoluteTuningName(int semitonesFromE);
	int GetShiftDirection(DropPedal::Player player);
	unsigned long long GetModeNoticeTick();
}
