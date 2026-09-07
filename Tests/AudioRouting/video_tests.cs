using System;
using System.IO;
using RSMods.Audio;

namespace RSMods.Tests
{
	internal static class VideoTests
	{
		private static int Main(string[] arguments)
		{
			try
			{
				if (arguments.Length != 3) throw new ArgumentException("Pass the captured MP4, the WAV take and the output MP4.");
				string result = WindowCaptureRecorder.Combine(Path.GetFullPath(arguments[0]), Path.GetFullPath(arguments[1]), Path.GetFullPath(arguments[2]), 2000000);
				if (!File.Exists(result) || new FileInfo(result).Length == 0) throw new Exception("MP4 was not created.");
				Console.WriteLine("PASS: production muxer created " + result);
				return 0;
			}
			catch (Exception error) { Console.Error.WriteLine(error); return 1; }
		}
	}
}
