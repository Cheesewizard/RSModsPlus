using System;
using System.IO;
using RSMods.Audio;

namespace RSMods.Tests
{
	internal static class AsioConfigurationTests
	{
		private static void Require(bool condition, string message)
		{
			if (!condition) throw new InvalidOperationException(message);
		}

		private static int Main(string[] args)
		{
			try
			{
				string directory = args[0];
				Directory.CreateDirectory(directory);
				string path = Path.Combine(directory, "RS_ASIO.ini");
				string original = "[Config]\r\nEnableWasapiOutputs=0\r\nEnableAsio=1\r\n[Asio.Output]\r\nDriver=Fixture ASIO\r\nBaseChannel=2\r\n[Asio.Input.0]\r\nDriver=Fixture ASIO\r\nChannel=1\r\n";
				File.WriteAllText(path, original);
				var settings = new AsioOutputConfiguration(directory);
				settings.Apply(true, false);
				string changed = File.ReadAllText(path);
				Require(changed.Contains("EnableWasapiOutputs=1"), "Windows output was not enabled");
				Require(changed.Contains("[Asio.Output]\r\nDriver=\r\nBaseChannel=2"), "Output driver was not cleared or its channel changed");
				Require(changed.Contains("[Asio.Input.0]\r\nDriver=Fixture ASIO\r\nChannel=1"), "ASIO guitar input was changed");
				settings.UndoFailedSave();
				Require(File.ReadAllText(path) == original, "Failed-save restoration was not exact");
				settings = new AsioOutputConfiguration(directory);
				settings.Apply(true, false);
				settings.Apply(false, true);
				Require(File.ReadAllText(path) == original, "Disabling did not restore output settings");
				settings.Apply(true, false);
				File.AppendAllText(path, "; user edit\r\n");
				bool rejected = false;
				try { settings.UndoFailedSave(); }
				catch (IOException) { rejected = true; }
				Require(rejected && File.ReadAllText(path).Contains("; user edit"), "Concurrent user edit was overwritten");
				Console.WriteLine("PASS: ASIO output switch, input preservation, output restoration, failed-save rollback, and concurrent edit protection");
				return 0;
			}
			catch (Exception exception)
			{
				Console.Error.WriteLine(exception);
				return 1;
			}
		}
	}
}
