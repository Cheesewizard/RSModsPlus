using System;
using System.IO;
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
				File.WriteAllText(loader, "loader fixture");
				File.WriteAllText(driver, "driver fixture");
				var mode = new AudioInputModeConfiguration(directory, () => false);
				mode.SetAsioEnabled(false);
				if (mode.ReadMode() != AudioInputMode.Cable || File.Exists(loader) || File.Exists(driver)) throw new Exception("Cable mode did not disable both files.");
				mode.SetAsioEnabled(true);
				if (mode.ReadMode() != AudioInputMode.Asio || File.ReadAllText(loader) != "loader fixture" || File.ReadAllText(driver) != "driver fixture") throw new Exception("ASIO restore changed file contents.");
				bool rejected = false;
				try { new AudioInputModeConfiguration(directory, () => true).SetAsioEnabled(false); }
				catch (InvalidOperationException) { rejected = true; }
				if (!rejected || mode.ReadMode() != AudioInputMode.Asio) throw new Exception("Running-game protection failed.");
				using (var locked = new FileStream(driver, FileMode.Open, FileAccess.Read, FileShare.Read))
				{
					rejected = false;
					try { mode.SetAsioEnabled(false); }
					catch (IOException) { rejected = true; }
					if (!rejected || mode.ReadMode() != AudioInputMode.Asio) throw new Exception("Partial rename was not rolled back.");
				}
				File.WriteAllText(driver + ".disabled", "conflicting fixture");
				rejected = false;
				try { mode.SetAsioEnabled(false); }
				catch (InvalidOperationException) { rejected = true; }
				if (!rejected || !File.Exists(loader)) throw new Exception("Conflicting files were changed.");
				Console.WriteLine("PASS: ASIO/Cable rename, exact restore, running-game refusal, partial failure rollback and collision refusal");
				return 0;
			}
			catch (Exception error) { Console.Error.WriteLine(error); return 1; }
		}
	}
}
