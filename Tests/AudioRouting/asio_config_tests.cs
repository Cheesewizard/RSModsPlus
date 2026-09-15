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
				File.WriteAllText(path, original);
				AsioProxySetup.LinkSettings(path, "Fixture ASIO");
				string linked = File.ReadAllText(path);
				Require(linked.Contains("[Asio.Output]\r\nDriver=Rocksmith Audio Bridge"), "Proxy link did not replace the ASIO output driver");
				Require(linked.Contains("[Asio.Input.0]\r\nDriver=Rocksmith Audio Bridge"), "Proxy link did not share one driver host with ASIO input");
				Require(linked.Contains("OriginalDriver.AudioBridge=Fixture ASIO"), "Proxy link did not preserve the ASIO input driver");
				AsioProxySetup.Unlink(directory);
				string unlinked = File.ReadAllText(path);
				Require(unlinked.Contains("[Asio.Input.0]\r\nDriver=Fixture ASIO"), "Proxy unlink did not restore the ASIO input driver");
				Require(!unlinked.Contains("OriginalDriver.AudioBridge"), "Proxy unlink retained its temporary input setting");

				File.WriteAllText(path,
					"[Config]\r\nEnableWasapiOutputs=0\r\nEnableAsio=1\r\n[Asio.Output]\r\n"
					+ "Driver=Rocksmith Audio Bridge\r\nOriginalOutputDriver.RSMods=Fixture ASIO\r\nBaseChannel=2\r\n"
					+ "[Asio.Input.0]\r\nDriver=Fixture ASIO\r\nOriginalDriver.RSMods=Fixture ASIO\r\nChannel=1\r\n");
				AsioProxySetup.SynchronizeLinkedInputs(directory);
				string synchronized = File.ReadAllText(path);
				Require(synchronized.Contains("[Asio.Input.0]\r\nDriver=Rocksmith Audio Bridge"), "Existing proxy link did not synchronize its matching ASIO input");
				Require(synchronized.Contains("OriginalDriver.AudioBridge=Fixture ASIO"), "Existing proxy link did not preserve the synchronized input driver");

				// Output pointed at the proxy by hand (no stash, input still on the raw driver): linking must record
				// the real driver for Unlink and the proxy's boot fallback, and share the input host.
				File.WriteAllText(path,
					"[Config]\r\nEnableWasapiOutputs=0\r\nEnableAsio=1\r\n[Asio.Output]\r\n"
					+ "Driver=Rocksmith Audio Bridge\r\nBaseChannel=2\r\n[Asio.Input.0]\r\nDriver=Fixture ASIO\r\nChannel=1\r\n");
				AsioProxySetup.LinkSettings(path, "Fixture ASIO");
				string handEdited = File.ReadAllText(path);
				Require(handEdited.Contains("OriginalOutputDriver.RSMods=Fixture ASIO"), "Hand-edited proxy output did not record the real driver");
				Require(handEdited.Contains("[Asio.Input.0]\r\nDriver=Rocksmith Audio Bridge"), "Hand-edited proxy output did not share the input host");
				AsioProxySetup.Unlink(directory);
				Require(File.ReadAllText(path).Contains("[Asio.Output]\r\nDriver=Fixture ASIO"), "Unlink after a hand-edited link did not restore the output driver");
				Console.WriteLine("PASS: proxy link shares the input host, preserves and restores the real driver, and repairs an output-only link");
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
