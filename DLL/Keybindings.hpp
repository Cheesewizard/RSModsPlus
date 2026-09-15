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
    void EnsureAudioBridgeRunning();

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
