namespace RSMods
{
	internal static class ProductInfo
	{
		public const string DISPLAY_NAME = "RSModsPlus 4";

		// Title of the audio bridge window. This is a cross-process contract: the game DLL finds the
		// running bridge by this exact title (DLL/Keybindings.cpp AUDIO_BRIDGE_WINDOW_TITLE) to auto-launch
		// it and to deliver the recording hotkey, and the GUI uses it to focus an existing instance.
		// Change both copies together.
		public const string AUDIO_BRIDGE_WINDOW_TITLE = DISPLAY_NAME + " · Audio bridge";
	}
}
