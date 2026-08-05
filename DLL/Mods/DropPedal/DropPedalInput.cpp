#include "../../stdafx.h"
#include "DropPedal.hpp"
#include "DropPedalHooks.hpp"
#include "DropPedalInput.hpp"
#include "DropPedalState.hpp"

namespace
{
	constexpr ULONGLONG PITCH_PUSH_DELAY_MILLISECONDS = 150;
	std::atomic<ULONGLONG> pushDeadlineTick{ 0 };

	DropPedal::Player GetCommandPlayer()
	{
		return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
			? DropPedal::Player::Two
			: DropPedal::Player::One;
	}

	const char* GetPlayerName(DropPedal::Player player)
	{
		return player == DropPedal::Player::One ? "Player 1" : "Player 2";
	}

	bool RejectUnavailablePlayerTwo(DropPedal::Player player)
	{
		if (player != DropPedal::Player::Two) return false;
		if (DropPedal::IsPlayerShiftAvailable(DropPedal::Player::Two)) return false;

		if (!DropPedal::IsInputShifterActive())
		{
			LOG_ERROR("Drop pedal Player 2 is not addressable on this game version's Cable "
				"engine; Player 2 follows Player 1." << std::endl);
			return true;
		}

		LOG_ERROR("Drop pedal Player 2 controls require a configured [Asio.Input.1]." << std::endl);
		return true;
	}
}

void DropPedalInput::AdjustTarget(int semitoneDelta)
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	const DropPedal::Player player = GetCommandPlayer();
	if (RejectUnavailablePlayerTwo(player)) return;

	if (!DropPedalState::AdjustTarget(player, semitoneDelta)) return;

	DropPedal::UpdateInputShifterPitch(player);

	LOG_INFO("Drop pedal " << GetPlayerName(player) << " target now "
		<< DropPedalState::GetTuningName(player) << std::endl);

	// Under ASIO the processor retune above is the whole change for Player 2;
	// under Cable every player's shift needs the debounced game-side push.
	if (player == DropPedal::Player::Two && DropPedal::IsInputShifterActive()) return;

	pushDeadlineTick.store(
		GetTickCount64() + PITCH_PUSH_DELAY_MILLISECONDS,
		std::memory_order_release);
}

void DropPedalInput::ToggleEnabled()
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	DropPedal::PitchMode nextMode;
	if (!DropPedalState::TryCyclePitchMode(nextMode))
	{
		LOG_INFO("Speaker Mode cannot be selected or disabled during gameplay" << std::endl);
		return;
	}

	DropPedal::UpdateInputShifterPitch(DropPedal::Player::One);
	DropPedal::UpdateInputShifterPitch(DropPedal::Player::Two);

	// A mode transition takes effect immediately. Entering Drop Pedal applies its
	// interval; entering Speaker Mode or Off restores the tone's authored pitch.
	if (!DropPedalHooks::IsInputShifterActive())
	{
		DropPedalHooks::PushPitchToLiveShifters();
	}

	LOG_INFO("Pitch mode changed to " << DropPedal::GetPitchModeName()
		<< ", route " << DropPedal::GetPitchRouteName() << std::endl);
}

void DropPedalInput::AdjustBaseTuning(int semitoneDelta)
{
	if (!DropPedalState::IsConfiguredEnabled()) return;

	const DropPedal::Player player = GetCommandPlayer();
	if (RejectUnavailablePlayerTwo(player)) return;

	if (!DropPedalState::AdjustBaseTuning(player, semitoneDelta)) return;

	LOG_INFO("Drop pedal " << GetPlayerName(player) << " base tuning now "
		<< DropPedalState::GetBaseTuningName(player) << std::endl);
}

void DropPedalInput::PollPendingPitchPush()
{
	ULONGLONG deadline = pushDeadlineTick.load(std::memory_order_acquire);
	if (deadline == 0 || GetTickCount64() < deadline)
	{
		return;
	}

	if (!pushDeadlineTick.compare_exchange_strong(
		deadline,
		0,
		std::memory_order_acq_rel,
		std::memory_order_acquire)) return;

	DropPedalHooks::PushPitchToLiveShifters();
}
