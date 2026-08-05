#include "../../stdafx.h"
#include "../../Audio/AsioHook.hpp"
#include "../../Audio/DelayLinePitchShifter.hpp"
#include "DropPedal.hpp"
#include "DropPedalHooks.hpp"
#include "DropPedalInput.hpp"
#include "DropPedalState.hpp"
#include "../../GameState.hpp"
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
	int pendingChartTuningSemitones = 0;
	bool hasPendingChartTuning = false;
	bool isPendingChartTuningUniform = false;
	unsigned long long speakerTargetReadAfterTick = 0;
	unsigned long long synchronizedSpeakerModeTick = 0;

	// The tuner UI's tuning text is authoritative while the pre-song tuner is up.
	// Once it is gone (a guitar already in tune passes it in under a second), the
	// loaded arrangement tuning stands in so a quick tuner cannot starve the sync.
	// Failed reads are detectable either way: GetTuningAtTuner's failure value (69
	// per string) falls outside the chart range, and TryGetCurrentTuning reports
	// an unresolved pointer explicitly.
	bool TryReadChartTuning(int (&stringTunings)[6])
	{
		if (GameState::Menus::IsInPreSongTuner())
		{
			const auto tuning = SongTuning::GetTuningAtTuner();
			stringTunings[0] = static_cast<int>(static_cast<signed char>(tuning.lowE));
			stringTunings[1] = static_cast<int>(static_cast<signed char>(tuning.strA));
			stringTunings[2] = static_cast<int>(static_cast<signed char>(tuning.strD));
			stringTunings[3] = static_cast<int>(static_cast<signed char>(tuning.strG));
			stringTunings[4] = static_cast<int>(static_cast<signed char>(tuning.strB));
			stringTunings[5] = static_cast<int>(static_cast<signed char>(tuning.highE));
			return true;
		}

		std::array<byte, 6> arrangementTuning{};
		if (!SongTuning::TryGetCurrentTuning(arrangementTuning)) return false;

		for (int i = 0; i < 6; i++)
		{
			stringTunings[i] = static_cast<int>(static_cast<signed char>(arrangementTuning[i]));
		}
		return true;
	}

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
		hasPendingChartTuning = false;
		DropPedalState::SetSpeakerTargetSynchronized(false);
		speakerTargetReadAfterTick = 0;
	}

	if (!hasSynchronizedSpeakerTarget)
	{
		// The tuner populates its tuning data while the screen builds, so no single
		// reading is trusted; the value is accepted once two consecutive reads agree.
		// Reads start immediately -- a fixed initial delay loses the race against a
		// guitar that is already in tune and passes the tuner in under a second.
		const auto now = GetTickCount64();
		if (now < speakerTargetReadAfterTick) return false;
		speakerTargetReadAfterTick = now + 250;

		int stringTunings[6];
		if (!TryReadChartTuning(stringTunings)) return false;

		if (stringTunings[0] < MIN_CHART_TUNING_SEMITONES
			|| stringTunings[0] > MAX_CHART_TUNING_SEMITONES)
		{
			hasPendingChartTuning = false;
			return false;
		}

		bool isUniform = true;
		for (const int stringTuning : stringTunings)
		{
			isUniform = isUniform && stringTuning == stringTunings[0];
		}

		if (!hasPendingChartTuning
			|| pendingChartTuningSemitones != stringTunings[0]
			|| isPendingChartTuningUniform != isUniform)
		{
			hasPendingChartTuning = true;
			pendingChartTuningSemitones = stringTunings[0];
			isPendingChartTuningUniform = isUniform;
			return false;
		}

		if (!isUniform)
		{
			LOG_ERROR("Speaker Mode requires a uniform chart tuning; selected song "
				<< songKey << " is not uniform" << std::endl);
			DisableSpeakerMode();
			return false;
		}

		synchronizedSpeakerTargetSemitones = stringTunings[0];
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
	hasPendingChartTuning = false;
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
