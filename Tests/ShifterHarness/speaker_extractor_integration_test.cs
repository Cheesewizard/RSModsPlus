using System;

namespace RSMods
{
	internal static class SpeakerExtractorIntegrationTest
	{
		private static int Main(string[] arguments)
		{
			if (!SpeakerModeCacheExtractor.TryRun(arguments, out int exitCode))
			{
				Console.Error.WriteLine("Speaker Mode extractor rejected its command line.");
				return 2;
			}

			return exitCode;
		}
	}
}
