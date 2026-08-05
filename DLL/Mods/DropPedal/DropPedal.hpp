#pragma once

#include "DropPedalPlayer.hpp"

// Owns the session pitch route shared by the Drop Pedal input engines and
// Speaker Mode's Wwise music-output shifter.
//
// The player adds a MultiPitch pedal to their tone in the Tone Designer. The game
// delivers that pedal's pitch through SetParam on the plugin's param object, in
// cents, re-sending it on every tone load and tone switch. Overriding that value
// is the whole mod: no tone files are modified, and a tone without a pitch pedal
// is left untouched.
//
// A pitch shifter moves the whole signal, so this covers uniform tunings such as
// Eb or D standard. It cannot produce drop tunings, where only one string differs.
//
namespace DropPedal
{
	enum class PitchMode
	{
		Off,
		DropPedal,
		SpeakerMode
	};

	void LoadSettings();
	bool IsConfiguredEnabled();
	bool ShouldInstallInputHooks();
	bool RequiresInputShifter();
	void ReportInputShifterUnavailable();
	void InstallInputHooks();
	void UpdateInputShifterPitch(Player player);

	void InstallHooks();
	void Poll();

	// Tracks the active arrangement's authored tuning reference through tuner and song.
	void HandleArrangementTuning(bool isGameplay);
	void ResetSongState();

	bool IsEnabled();
	bool IsSpeakerModeEnabled();
	void DisableSpeakerMode();
	bool TrySynchronizeSpeakerTarget(const std::string& songKey);
	PitchMode GetPitchMode();
	std::string GetPitchModeName();
	int GetTargetSemitones(Player player);
	int GetShiftSemitones();
	std::string GetTuningName(Player player);
	std::string GetPitchRouteName();
	std::string GetPhysicalTuningName();
	std::string GetTargetTuningName();
	bool TryGetAuthoredTrueTuning(float& trueTuning);

	// Selects which engine realises the pitch. With the ASIO input shifter active, the
	// game-side MultiPitch driving is suppressed: the input itself is retuned, so audio
	// and detection hear the same shifted notes while Rocksmith keeps the arrangement's
	// authored tuning reference. The hotkeys and overlay stay live either way.
	void SetInputShifterActive(bool active);
	bool IsInputShifterActive();
	bool IsPlayerShiftAvailable(Player player);
	// Whether the player's loaded tone contains a pitch shifter; Cable only.
	bool HasLivePedalTone(Player player);
	bool ConsumeInputShifterTransitionFailure();

	// Tick of the last engine decision or change, for the on-screen engine notice.
	// Zero until hooks are installed.
	unsigned long long GetEngineNoticeTick();

	// The tuning the guitar is physically in, and which way the shift is going, so the
	// overlay can name and colour the state without duplicating the arithmetic.
	int GetBaseTuningSemitones(Player player);
	std::string GetBaseTuningName(Player player);
	int GetShiftDirection(Player player);
}
