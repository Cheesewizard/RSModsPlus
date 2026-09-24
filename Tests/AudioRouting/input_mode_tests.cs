using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using RSMods.Audio;

namespace RSMods.Tests
{
	internal static class InputModeTests
	{
		private static int Main(string[] arguments)
		{
			try
			{
				string directory = arguments[0];
				Directory.CreateDirectory(directory);
				string loader = Path.Combine(directory, "avrt.dll");
				string driver = Path.Combine(directory, "RS_ASIO.dll");
				string settings = Path.Combine(directory, "RS_ASIO.ini");
				File.WriteAllText(loader, "loader fixture");
				File.WriteAllText(driver, "driver fixture");
				File.WriteAllText(settings,
					"[Config]\r\nEnableWasapiInputs=0\r\nEnableAsio=1\r\n\r\n"
					+ "[Asio.Output]\r\nDriver=Rocksmith Audio Bridge ASIO\r\n\r\n"
					+ "[Asio.Input.0]\r\nDriver=M-Audio M-Track Solo and Duo ASIO\r\nChannel=0\r\n\r\n"
					+ "[Asio.Input.1]\r\nDriver=\r\n\r\n[Asio.Input.Mic]\r\nDriver=\r\n");

				string originalSettings = File.ReadAllText(settings);
				var mode = new AudioInputModeConfiguration(directory, () => false);
				var endedProcess = Process.GetCurrentProcess();
				endedProcess.Dispose();
				var processCheck = typeof(AudioInputModeConfiguration).GetMethod("IsProcessInGameDirectory", BindingFlags.Instance | BindingFlags.NonPublic);
				if ((bool)processCheck.Invoke(mode, new object[] { endedProcess }))
					throw new Exception("An exited Rocksmith process was reported as running.");
				if (mode.ReadMode() != AudioInputMode.Asio) throw new Exception("Configured ASIO input was not detected.");
				mode.SetAsioEnabled(false);
				if (mode.ReadMode() != AudioInputMode.Cable) throw new Exception("Cable mode was not selected.");
				if (File.Exists(loader) || File.Exists(driver)
					|| !File.Exists(loader + ".disabled") || !File.Exists(driver + ".disabled"))
					throw new Exception("Cable mode did not disable both RS_ASIO files.");
				// Cable mode is decided by the RS_ASIO files alone (a renamed RS_ASIO.dll never reads its ini),
				// so the user's RS_ASIO.ini must come through byte-identical for the switch back to ASIO.
				if (File.ReadAllText(settings) != originalSettings)
					throw new Exception("Cable mode rewrote RS_ASIO.ini instead of leaving it to the ASIO switch-back.");

				mode.SetAsioEnabled(true);
				if (mode.ReadMode() != AudioInputMode.Asio) throw new Exception("ASIO input was not restored.");
				if (!File.Exists(loader) || !File.Exists(driver)
					|| File.Exists(loader + ".disabled") || File.Exists(driver + ".disabled"))
					throw new Exception("ASIO mode did not restore both RS_ASIO files.");
				if (File.ReadAllText(settings) != originalSettings)
					throw new Exception("ASIO mode did not leave RS_ASIO.ini exactly as the user had it.");

				bool rejected = false;
				try { new AudioInputModeConfiguration(directory, () => true).SetAsioEnabled(false); }
				catch (InvalidOperationException) { rejected = true; }
				if (!rejected || mode.ReadMode() != AudioInputMode.Asio) throw new Exception("Running-game protection failed.");

				Console.WriteLine("PASS: Cable disables both RS_ASIO files; ASIO restores them; RS_ASIO.ini is never rewritten by the mode switch");
				return 0;
			}
			catch (Exception error)
			{
				Console.Error.WriteLine(error);
				return 1;
			}
		}
	}
}
