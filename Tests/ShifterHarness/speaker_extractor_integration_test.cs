using System;

namespace RSMods
{
	internal static class SpeakerExtractorIntegrationTest
	{
		private const string PATH_TEST_ARGUMENT = "--test-psarc-entry-paths";

		private static int Main(string[] arguments)
		{
			if (arguments.Length == 1 && string.Equals(arguments[0], PATH_TEST_ARGUMENT, StringComparison.Ordinal))
			{
				return RunPathTests();
			}

			if (!SpeakerModeCacheExtractor.TryRun(arguments, out int exitCode))
			{
				Console.Error.WriteLine("Speaker Mode extractor rejected its command line.");
				return 2;
			}

			return exitCode;
		}

		private static int RunPathTests()
		{
			try
			{
				AssertFileName("songs/bin/generic/Song_Savior.bnk", "Song_Savior.bnk");
				AssertFileName("songs\\bin\\generic\\Song_Savior.bnk", "Song_Savior.bnk");
				AssertFileName("manifests/songs/invalid\0entry", "invalid\0entry");
				AssertNotFileName(null);

				AssertMediaId("audio/windows/123456789.wem", 123456789);
				AssertMediaId("audio\\windows\\42.WEM", 42);
				AssertNotMediaId(null);
				AssertNotMediaId("manifests/songs/invalid\0entry");
				AssertNotMediaId("audio/windows/not-a-number.wem");

				Console.WriteLine("Speaker extractor PSARC entry path tests passed.");
				return 0;
			}
			catch (Exception exception)
			{
				Console.Error.WriteLine(exception);
				return 1;
			}
		}

		private static void AssertFileName(string entryPath, string expectedFileName)
		{
			if (!PsarcEntryPath.TryGetFileName(entryPath, out string actualFileName))
			{
				throw new InvalidOperationException($"Expected a file name for '{entryPath}'.");
			}

			AssertEqual(expectedFileName, actualFileName);
		}

		private static void AssertNotFileName(string entryPath)
		{
			if (PsarcEntryPath.TryGetFileName(entryPath, out string fileName))
			{
				throw new InvalidOperationException(
					$"Expected no file name for '{entryPath}', got '{fileName}'.");
			}
		}

		private static void AssertMediaId(string entryPath, uint expectedMediaId)
		{
			if (!PsarcEntryPath.TryGetWemMediaId(entryPath, out uint actualMediaId))
			{
				throw new InvalidOperationException($"Expected a WEM media ID for '{entryPath}'.");
			}

			if (actualMediaId != expectedMediaId)
			{
				throw new InvalidOperationException(
					$"Expected media ID {expectedMediaId} for '{entryPath}', got {actualMediaId}.");
			}
		}

		private static void AssertNotMediaId(string entryPath)
		{
			if (PsarcEntryPath.TryGetWemMediaId(entryPath, out uint mediaId))
			{
				throw new InvalidOperationException(
					$"Expected no WEM media ID for '{entryPath}', got {mediaId}.");
			}
		}

		private static void AssertEqual(string expected, string actual)
		{
			if (!string.Equals(expected, actual, StringComparison.Ordinal))
			{
				throw new InvalidOperationException($"Expected '{expected}', got '{actual}'.");
			}
		}
	}
}
