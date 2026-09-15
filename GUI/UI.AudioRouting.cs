using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Windows.Forms;
using RSMods.Audio;
using RSMods.Util;

namespace RSMods
{
	public partial class MainForm
	{
		// The audio bridge process owns a set of RSMods.ini values (ReadSettings.BridgeOwnedIdentifiers)
		// and writes them one line at a time, because the shared WriteINI rewrites the whole file from
		// this window's in-memory table. Re-read every bridge-owned value on activation so the next full
		// save from here does not overwrite what the bridge wrote.
		internal static void SyncModernCableSettingFromDisk()
		{
			foreach (string identifier in ReadSettings.BridgeOwnedIdentifiers)
			{
				string value = ReadSettings.ProcessSettings(identifier);
				if (!string.IsNullOrWhiteSpace(value))
					SyncBridgeOwnedSetting(identifier, value);
			}
		}

		private static void SyncBridgeOwnedSetting(string identifier, string value)
		{
			foreach (var section in WriteSettings.saveSettingsOrDefaults.Keys)
			{
				if (WriteSettings.saveSettingsOrDefaults[section].ContainsKey(identifier))
				{
					WriteSettings.saveSettingsOrDefaults[section][identifier] = value;
					return;
				}
			}
		}

		// The bridge is opened from the RSModsPlus navigation ("Audio bridge" opens it directly). What this
		// window still owes the bridge is the settings sync: re-read the bridge-owned values whenever the
		// window is activated, so a later full save from here cannot revert what the bridge wrote.
		private void AddAudioRoutingButton()
		{
			Activated += (sender, args) => SyncModernCableSettingFromDisk();
		}

		private void OpenAudioRouting(object sender, EventArgs args)
		{
			try
			{
				switch (AudioBridgeWindow.TryBringToFront())
				{
					case AudioBridgeWindow.Presence.Shown:
						return;
					case AudioBridgeWindow.Presence.FoundButNotShown:
						MessageBox.Show(this,
							"The audio bridge is already open but cannot be brought to the front from here. "
							+ "It is probably running as administrator alongside Rocksmith; switch to it from the taskbar.",
							"Audio bridge", MessageBoxButtons.OK, MessageBoxIcon.Information);
						return;
				}

				var startInfo = new ProcessStartInfo
				{
					FileName = Application.ExecutablePath,
					Arguments = "--audio-bridge \"" + Path.Combine(GenUtil.GetRSDirectory(), ".") + "\"",
					UseShellExecute = false
				};
				using (var process = Process.Start(startInfo))
				{
					if (process == null) throw new InvalidOperationException("Audio bridge did not start.");
				}
			}
			catch (Exception exception)
			{
				Console.Error.WriteLine(exception);
				MessageBox.Show(this, exception.Message, "Cannot open audio bridge", MessageBoxButtons.OK, MessageBoxIcon.Error);
			}
		}
	}
}
