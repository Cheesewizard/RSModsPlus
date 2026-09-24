namespace RSMods
{
	internal static class ProductInfo
	{
		// Product name (2026-09-23 rebrand from "RSModsPlus", which Rocksmith+ players confused with RS+).
		// Pipes, registry keys, INI names and folders keep "RSModsPlus" for upgrade compatibility. The runtime
		// files were renamed before the first public release (only the developer had an install):
		// rsmodsplus.dll -> RocksmithAudioBridge.dll, and the ASIO driver -> RocksmithAudioBridgeAsio.dll.
		public const string PRODUCT_NAME = "Rocksmith Audio Bridge";
		// Keep VERSION in step with DLL/ProductVersion.hpp VERSION.
		public const string VERSION = "v4";
		public const string DISPLAY_NAME = PRODUCT_NAME + " " + VERSION;

		// Title of the desktop audio bridge window. This is a cross-process contract: the game DLL finds the
		// running bridge by this exact title (DLL/Keybindings.cpp AUDIO_BRIDGE_WINDOW_TITLE) to auto-launch
		// it and to deliver the recording hotkey, and the GUI uses it to focus an existing instance.
		// Change both copies together. The suffix is "Desktop mixer" rather than "Audio bridge" now that the
		// product itself is called Rocksmith Audio Bridge.
		public const string AUDIO_BRIDGE_WINDOW_TITLE = DISPLAY_NAME + " · Desktop mixer";
	}
}
