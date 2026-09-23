#pragma once

#include <functional>
#include <string>
#include <vector>
#include <map>
#include <Windows.h>
#include "Mods/VolumeControl.hpp"
#include "Mods/Loft.hpp"
#include "Mods/Enumeration.hpp"
#include "Mods/VoiceOverControl.hpp"
#include "Twitch.hpp"
#include "CC/ControlServer.hpp"
#include "Menu.hpp"

struct ModCommand {
    std::function<bool()> condition = [] { return true; };
    std::function<void()> action;
    std::string logMessage;
};

namespace Keybindings {
    bool EnsureAudioBridgeRunning(bool showWindow = false);
	bool IsAudioBridgeFocusTransferPending();
	void NotifyGameFocused();
    // Toggle a recording take on the desktop bridge (audio, plus MP4 video when the bridge's Format is set
    // to video). Posts the same message the in-game recording hotkey does; the in-game overlay uses this so
    // its Record button captures video without the user tabbing out. No-op if the bridge is not running.
    void ToggleAudioBridgeRecording();
    // Overlay path: toggle recording and override the desktop bridge's format. Wet and dry WAVs are always paired.
    void ToggleAudioBridgeRecording(bool video);
    // Switch focus to the desktop audio bridge window, or launch it visibly if it is not open.
    // The in-game overlay's "Open the desktop bridge" button calls this so the user can reach the full window.
    void BringAudioBridgeToFront();

    void DispatchCommand(WPARAM keyPressed, const std::map<std::string, ModCommand, std::less<>>& commands);

    void HandleKeyUp(WPARAM keyPressed);
    void HandleKeyDown(WPARAM keyPressed);

    void InitializeCommands();
    void UpdateSettingsOnGUIChange(LPARAM lParam);

    // Looping functionality.
    inline float loopStart = NULL; // The start of the loop, as specified by the user.
    inline float roughLoopStart = NULL; // Just like loopStart, except we account for the lead-in time.
    inline float loopEnd = NULL; // The end of the loop, as specified by the user.
}
