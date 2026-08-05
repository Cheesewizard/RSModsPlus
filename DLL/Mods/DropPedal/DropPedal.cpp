#include "../../stdafx.h"
#include "../../Audio/AsioHook.hpp"
#include "../../Audio/DelayLinePitchShifter.hpp"
#include "DropPedal.hpp"
#include "DropPedalHooks.hpp"
#include "DropPedalInput.hpp"
#include "DropPedalState.hpp"
#include "../../SongTuning.hpp"

namespace
{
	constexpr int MIN_CHART_TUNING_SEMITONES = -24;
	constexpr int MAX_CHART_TUNING_SEMITONES = 24;
	constexpr int SEMITONES_PER_OCTAVE = 12;

	std::string speakerTargetSongKey;
	bool hasSynchronizedSpeakerTarget = false;
	int synchronizedSpeakerTargetSemitones = 0;
	int synchronizedSpeakerOctaveAdjustment = 0;
	unsigned long long speakerTargetReadAfterTick = 0;
	unsigned long long synchronizedSpeakerModeTick = 0;

	int GetClosestOctaveAdjustment(int selectedShiftSemitones, int chartShiftSemitones)
	{
		const int difference = selectedShiftSemitones - chartShiftSemitones;
		const int roundingAdjustment = difference >= 0
			? SEMITONES_PER_OCTAVE / 2
			: -(SEMITONES_PER_OCTAVE / 2);
		return ((difference + roundingAdjustment) / SEMITONES_PER_OCTAVE)
			* SEMITONES_PER_OCTAVE;
	}

	Audio::DelayLinePitchShifter playerOneInputPitchShifter{ 0 };
	Audio::DelayLinePitchShifter playerTwoInputPitchShifter{ 0 };

	Audio::DelayLinePitchShifter& GetInputPitchShifter(DropPedal::Player player)
	{
		return player == DropPedal::Player::One
			? playerOneInputPitchShifter
			: playerTwoInputPitchShifter;
	}
}

void DropPedal::LoadSettings()
{
	DropPedalState::Configure(
		Settings::ReturnSettingValue("EnableDropPedal"),
		Settings::ReturnSettingValue("DropPedalEngine"));
}

bool DropPedal::IsConfiguredEnabled()
{
	return DropPedalState::IsConfiguredEnabled();
}

bool DropPedal::ShouldInstallInputHooks()
{
	return DropPedalState::IsConfiguredEnabled() && !DropPedalState::IsCableEngine();
}

bool DropPedal::RequiresInputShifter()
{
	return DropPedalState::IsConfiguredEnabled() && DropPedalState::IsAsioEngine();
}

void DropPedal::ReportInputShifterUnavailable()
{
	DropPedalHooks::ReportInputShifterUnavailable();
}

void DropPedal::InstallInputHooks()
{
	if (!ShouldInstallInputHooks()) return;

	Audio::AsioHook::Install();
	Audio::AsioHook::SetProcessor(GetPlayerIndex(Player::One), &playerOneInputPitchShifter);
	Audio::AsioHook::SetProcessor(GetPlayerIndex(Player::Two), &playerTwoInputPitchShifter);
	UpdateInputShifterPitch(Player::One);
	UpdateInputShifterPitch(Player::Two);
}

void DropPedal::UpdateInputShifterPitch(Player player)
{
	if (!ShouldInstallInputHooks()) return;

	const int targetSemitones = IsEnabled() ? GetTargetSemitones(player) : 0;
	GetInputPitchShifter(player).SetSemitones(targetSemitones);
}

bool DropPedal::IsEnabled()
{
	return DropPedalState::IsConfiguredEnabled() && DropPedalState::IsEnabled();
}

bool DropPedal::IsSpeakerModeEnabled()
{
	return DropPedalState::IsConfiguredEnabled() && DropPedalState::IsSpeakerModeEnabled();
}

void DropPedal::DisableSpeakerMode()
{
	if (!DropPedalState::DisableSpeakerMode()) return;

	DropPedalHooks::PushPitchToLiveShifters();
}

bool DropPedal::TrySynchronizeSpeakerTarget(const std::string& songKey)
{
	if (songKey.empty() || !IsSpeakerModeEnabled()) return false;

	bool didSynchronize = false;
	const auto modeTick = DropPedalState::GetModeNoticeTick();
	if (speakerTargetSongKey != songKey || synchronizedSpeakerModeTick != modeTick)
	{
		speakerTargetSongKey = songKey;
		synchronizedSpeakerModeTick = modeTick;
		hasSynchronizedSpeakerTarget = false;
		DropPedalState::SetSpeakerTargetSynchronized(false);
		speakerTargetReadAfterTick = GetTickCount64() + 1500;
	}

	if (!hasSynchronizedSpeakerTarget && GetTickCount64() < speakerTargetReadAfterTick)
	{
		return false;
	}

	if (!hasSynchronizedSpeakerTarget)
	{
		speakerTargetReadAfterTick = GetTickCount64() + 500;
		const auto tuning = SongTuning::GetTuningAtTuner();
		const int stringTunings[] =
		{
			static_cast<int>(static_cast<signed char>(tuning.lowE)),
			static_cast<int>(static_cast<signed char>(tuning.strA)),
			static_cast<int>(static_cast<signed char>(tuning.strD)),
			static_cast<int>(static_cast<signed char>(tuning.strG)),
			static_cast<int>(static_cast<signed char>(tuning.strB)),
			static_cast<int>(static_cast<signed char>(tuning.highE))
		};

		synchronizedSpeakerTargetSemitones = stringTunings[0];
		if (synchronizedSpeakerTargetSemitones < MIN_CHART_TUNING_SEMITONES
			|| synchronizedSpeakerTargetSemitones > MAX_CHART_TUNING_SEMITONES)
		{
			return false;
		}

		for (const int stringTuning : stringTunings)
		{
			if (stringTuning == synchronizedSpeakerTargetSemitones) continue;

			LOG_ERROR("Speaker Mode requires a uniform chart tuning; selected song "
				<< songKey << " is not uniform" << std::endl);
			DisableSpeakerMode();
			return false;
		}

		DropPedalState::SetSpeakerTargetSynchronized(true);
		const int chartShiftSemitones = synchronizedSpeakerTargetSemitones
			- DropPedalState::GetBaseTuningSemitones(Player::One);
		synchronizedSpeakerOctaveAdjustment = GetClosestOctaveAdjustment(
			DropPedalState::GetTargetSemitones(Player::One),
			chartShiftSemitones);
		hasSynchronizedSpeakerTarget = true;
		didSynchronize = true;
	}

	const int shiftSemitones = synchronizedSpeakerTargetSemitones
		- DropPedalState::GetBaseTuningSemitones(Player::One)
		+ synchronizedSpeakerOctaveAdjustment;
	if (!DropPedalState::SetTargetSemitones(Player::One, shiftSemitones))
	{
		LOG_ERROR("Speaker Mode chart tuning is outside the supported pitch range for "
			<< songKey << std::endl);
		DisableSpeakerMode();
		return false;
	}
	if (didSynchronize)
	{
		LOG_INFO("Speaker Mode chart target synchronized to " << GetTargetTuningName()
			<< " (chart " << synchronizedSpeakerTargetSemitones
			<< ", octave adjustment " << synchronizedSpeakerOctaveAdjustment
			<< ") for " << songKey << std::endl);
	}
	return true;
}

DropPedal::PitchMode DropPedal::GetPitchMode()
{
	return DropPedalState::GetPitchMode();
}

std::string DropPedal::GetPitchModeName()
{
	switch (GetPitchMode())
	{
		case PitchMode::DropPedal:
			return "Drop Pedal";
		case PitchMode::SpeakerMode:
			return "Speaker Mode";
		case PitchMode::Off:
		default:
			return "Off";
	}
}

void DropPedal::HandleArrangementTuning(bool isGameplay)
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	DropPedalState::SetGameplayInProgress(isGameplay);
	DropPedalHooks::HandleArrangementTuning();
}

void DropPedal::ResetSongState()
{
	DropPedalState::SetGameplayInProgress(false);
	DropPedalState::SetSpeakerTargetSynchronized(false);
	speakerTargetSongKey.clear();
	hasSynchronizedSpeakerTarget = false;
	synchronizedSpeakerTargetSemitones = 0;
	synchronizedSpeakerOctaveAdjustment = 0;
	speakerTargetReadAfterTick = 0;
	synchronizedSpeakerModeTick = 0;
	if (!DropPedalState::IsConfiguredEnabled()) return;

	DropPedalHooks::ResetSongState();
}

int DropPedal::GetTargetSemitones(Player player)
{
	return DropPedalState::GetTargetSemitones(player);
}

// Speaker Mode shifts the one song mix; its route rides on Player One's state.
int DropPedal::GetShiftSemitones()
{
	return DropPedalState::GetTargetSemitones(Player::One);
}

std::string DropPedal::GetTuningName(Player player)
{
	return DropPedalState::GetTuningName(player);
}

std::string DropPedal::GetPitchRouteName()
{
	const auto physicalTuning = GetPhysicalTuningName();
	const auto targetTuning = GetTargetTuningName();
	const int shiftSemitones = GetShiftSemitones();
	if (shiftSemitones == 0) return physicalTuning;

	std::ostringstream route;
	if (GetPitchMode() == PitchMode::SpeakerMode)
	{
		route << targetTuning << " -> " << physicalTuning;
		const int speakerShiftSemitones = -shiftSemitones;
		route << " (" << (speakerShiftSemitones > 0 ? "+" : "")
			<< speakerShiftSemitones << ")";
		return route.str();
	}

	route << physicalTuning << " -> " << targetTuning;
	route << " (" << (shiftSemitones > 0 ? "+" : "") << shiftSemitones << ")";
	return route.str();
}

std::string DropPedal::GetPhysicalTuningName()
{
	return DropPedalState::GetAbsoluteTuningName(
		DropPedalState::GetBaseTuningSemitones(Player::One));
}

std::string DropPedal::GetTargetTuningName()
{
	return DropPedalState::GetAbsoluteTuningName(
		DropPedalState::GetBaseTuningSemitones(Player::One)
		+ DropPedalState::GetTargetSemitones(Player::One));
}

bool DropPedal::TryGetAuthoredTrueTuning(float& trueTuning)
{
	if (!DropPedalState::IsConfiguredEnabled()) return false;

	return DropPedalHooks::TryGetAuthoredTrueTuning(trueTuning);
}

std::string DropPedal::GetBaseTuningName(Player player)
{
	return DropPedalState::GetBaseTuningName(player);
}

int DropPedal::GetBaseTuningSemitones(Player player)
{
	return DropPedalState::GetBaseTuningSemitones(player);
}

int DropPedal::GetShiftDirection(Player player)
{
	const int direction = DropPedalState::GetShiftDirection(player);
	return IsSpeakerModeEnabled() && player == Player::One ? -direction : direction;
}

void DropPedal::InstallHooks()
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	DropPedalHooks::Install();
}

void DropPedal::SetInputShifterActive(bool active)
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	DropPedalHooks::SetInputShifterActive(active);
}

bool DropPedal::IsInputShifterActive()
{
	return DropPedalState::IsConfiguredEnabled() && DropPedalHooks::IsInputShifterActive();
}

bool DropPedal::IsPlayerShiftAvailable(Player player)
{
	if (player == Player::One) return true;

	if (!IsInputShifterActive())
	{
		return DropPedalState::IsConfiguredEnabled()
			&& DropPedalHooks::IsCableAttributionActive();
	}

	const size_t routeIndex = GetPlayerIndex(player);
	return Audio::AsioHook::IsInputConfigured(routeIndex)
		&& Audio::AsioHook::IsInputReady(routeIndex);
}

bool DropPedal::HasLivePedalTone(Player player)
{
	return DropPedalState::IsConfiguredEnabled()
		&& DropPedalHooks::HasLivePlayerPedalTone(player);
}

bool DropPedal::ConsumeInputShifterTransitionFailure()
{
	return DropPedalState::IsConfiguredEnabled()
		&& DropPedalHooks::ConsumeInputShifterTransitionFailure();
}

unsigned long long DropPedal::GetEngineNoticeTick()
{
	return DropPedalState::IsConfiguredEnabled()
		? DropPedalHooks::GetEngineNoticeTick()
		: 0;
}

void DropPedal::Poll()
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	DropPedalHooks::Poll();

	if (!DropPedalHooks::IsInputShifterActive())
	{
		DropPedalInput::PollPendingPitchPush();
		DropPedalHooks::LogPendingOverrides();
	}
}
