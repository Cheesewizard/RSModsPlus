using System;
using System.IO;
using System.Threading;
using RSMods.Audio;

namespace RSMods.Tests
{
	internal static class ControlClientTests
	{
		private static int Main(string[] arguments)
		{
			try
			{
				var client = new AudioControlClient(int.Parse(arguments[0]));
				var status = client.SendAsync(1).GetAwaiter().GetResult();
				if (status.EndpointId != "second") throw new Exception("Native endpoint identity was decoded incorrectly.");
				status = client.SendAsync(7, "81").GetAwaiter().GetResult();
				status = client.SendAsync(8, "29").GetAwaiter().GetResult();
				if (status.MixerError < 0 || status.Volumes[0] != 81 || status.Volumes[1] != 29)
					throw new Exception("Native mixer readback or channel isolation failed.");
				for (int channel = 0; channel < 7; channel++)
				{
					var before = (float[])status.Volumes.Clone();
					foreach (int volume in new[] { 0, 100, 20 + channel })
					{
						status = client.SendAsync((uint)(7 + channel), volume.ToString()).GetAwaiter().GetResult();
						for (int other = 0; other < 7; other++)
						{
							if (status.Volumes[other] != (other == channel ? volume : before[other])) throw new Exception("Seven-channel mixer isolation failed.");
						}
					}
				}
				status = client.SendAsync(7, "81").GetAwaiter().GetResult();
				for (int take = 0; take < 2; take++)
				{
					status = client.SendAsync(2, arguments[1]).GetAwaiter().GetResult();
					if (!status.IsRecording) throw new Exception("Record was not acknowledged.");
					status = client.SendAsync(8, "40").GetAwaiter().GetResult();
					if (!status.IsRecording || status.Volumes[1] != 40 || status.Volumes[0] != 81)
						throw new Exception("Mixer adjustment interrupted the take or changed the other channel.");
					Thread.Sleep(80);
					status = client.SendAsync(3).GetAwaiter().GetResult();
					if (status.IsRecording || status.Frames == 0 || status.RecordingStarted == 0 || !File.Exists(status.FilePath)) throw new Exception("The native take did not finalize.");
				}
				Console.WriteLine("PASS: production C# client recorded repeated takes through the native pipe");
				return 0;
			}
			catch (Exception error) { Console.Error.WriteLine(error); return 1; }
		}
	}
}
