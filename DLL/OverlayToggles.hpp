#pragma once

#include <string>

namespace OverlayToggles
{
	bool Get(const std::string& name);            // unknown name -> true (fail-visible)
	bool Set(const std::string& name, bool on);   // false if the name is not registered
	void ApplyIniDefaults();                       // read RSMods .ini "Overlay_<name>" at startup
	std::string List();                            // "name=on\nname2=off\n..." for the bridge
}
