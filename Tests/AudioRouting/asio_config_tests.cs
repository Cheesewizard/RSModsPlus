using System;
using System.IO;
using System.Reflection;
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
				// Contract: the GUI never writes RS_ASIO.ini. Pointing RS_ASIO at the bridge is the user's job.
				// The old Link/Unlink/LinkSettings/SynchronizeLinkedInputs machinery rewrote the user's ini on the
				// bridge power toggle and stranded them on the raw driver, which then failed to boot when the
				// interface was absent. Guard against any of it returning.
				var type = typeof(AsioProxySetup);
				const BindingFlags all = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance;
				foreach (string forbidden in new[] { "Link", "LinkSettings", "Unlink", "SynchronizeLinkedInputs" })
					Require(type.GetMethod(forbidden, all) == null,
						"AsioProxySetup." + forbidden + " must not exist: the GUI must never write RS_ASIO.ini.");

				// It must never write RS_ASIO.ini either directly: no WritePrivateProfileString P/Invoke here.
				foreach (var method in type.GetMethods(all))
					Require(method.Name != "WritePrivateProfileString",
						"AsioProxySetup must not P/Invoke WritePrivateProfileString: RS_ASIO.ini is the user's to edit.");

				// Prove it in practice: an ini on the raw driver is left byte-for-byte identical after the setup
				// entry points the GUI still owns (which touch only HKLM registration and HKCU Target).
				string directory = args[0];
				Directory.CreateDirectory(directory);
				string path = Path.Combine(directory, "RS_ASIO.ini");
				string original = "[Config]\r\nEnableWasapiOutputs=0\r\nEnableAsio=1\r\n[Asio.Output]\r\nDriver=Fixture ASIO\r\nBaseChannel=2\r\n[Asio.Input.0]\r\nDriver=Fixture ASIO\r\nChannel=1\r\n";
				File.WriteAllText(path, original);
				Require(!AsioProxySetup.IsLinked(directory), "A raw-driver ini must not report as linked to the bridge.");
				Require(File.ReadAllText(path) == original, "Reading link state must not modify RS_ASIO.ini.");

				// An ini the user pointed at the bridge reads back as linked, and stays untouched.
				string linked = "[Config]\r\nEnableWasapiOutputs=0\r\nEnableAsio=1\r\n[Asio.Output]\r\nDriver=Rocksmith Audio Bridge ASIO\r\nBaseChannel=2\r\n[Asio.Input.0]\r\nDriver=Rocksmith Audio Bridge ASIO\r\nChannel=1\r\n";
				File.WriteAllText(path, linked);
				Require(AsioProxySetup.IsLinked(directory), "An ini pointed at the bridge must report as linked.");
				Require(File.ReadAllText(path) == linked, "Reading link state must not modify a linked RS_ASIO.ini.");

				Console.WriteLine("PASS: the GUI never writes RS_ASIO.ini; link state is read-only");
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
