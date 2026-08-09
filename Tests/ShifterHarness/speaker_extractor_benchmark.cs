using Rocksmith2014PsarcLib.Psarc;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;

namespace RSMods.Tests.ShifterHarness
{
	internal static class SpeakerExtractorBenchmark
	{
		private static int Main(string[] arguments)
		{
			if (arguments.Length != 3)
			{
				Console.Error.WriteLine("Usage: speaker_extractor_benchmark.exe <Rocksmith directory> <bank name> <output directory>");
				return 2;
			}

			try
			{
				Run(arguments[0], arguments[1], arguments[2]);
				return 0;
			}
			catch (Exception exception)
			{
				Console.Error.WriteLine(exception);
				return 1;
			}
		}

		private static void Run(string rocksmithDirectory, string bankName, string outputDirectory)
		{
			Directory.CreateDirectory(outputDirectory);
			var totalTimer = Stopwatch.StartNew();
			var source = Measure("PSARC discovery + bank/WEM resolution", () => FindSource(rocksmithDirectory, bankName));
			var wemPath = Path.Combine(outputDirectory, "source.wem");
			var oggPath = Path.Combine(outputDirectory, "source.ogg");
			var repairedOggPath = Path.Combine(outputDirectory, "source.fixed.ogg");
			var wavePath = Path.Combine(outputDirectory, "source.wav");

			Measure("PSARC WEM inflation", () =>
			{
				using (var archive = new PsarcFile(source.ArchivePath))
				using (var output = File.Create(wemPath))
				{
					var entry = archive.TOC.Entries.Single(item => item.Path == source.WemEntryPath);
					archive.InflateEntry(entry, output);
				}
			});

			var toolsDirectory = Path.Combine(rocksmithDirectory, "RSMods", "tools");
			Measure("ww2ogg", () => RunTool(
				Path.Combine(toolsDirectory, "ww2ogg.exe"),
				$"\"{wemPath}\" -o \"{oggPath}\" --pcb \"{Path.Combine(toolsDirectory, "packed_codebooks_aoTuV_603.bin")}\""));
			Measure("revorb", () => RunTool(
				Path.Combine(toolsDirectory, "revorb.exe"),
				$"\"{oggPath}\" \"{repairedOggPath}\""));
			Measure("oggdec", () => RunTool(
				Path.Combine(toolsDirectory, "oggdec.exe"),
				$"-Q -b 16 -e 0 -s 1 -o \"{wavePath}\" \"{repairedOggPath}\""));

			totalTimer.Stop();
			Console.WriteLine("{0,-38} {1,9:F2} ms", "total extract + decode", totalTimer.Elapsed.TotalMilliseconds);
			Console.WriteLine($"decoded WAV: {wavePath} ({new FileInfo(wavePath).Length:N0} bytes)");
		}

		private static SpeakerSource FindSource(string rocksmithDirectory, string bankName)
		{
			var archivePaths = Directory
				.EnumerateFiles(Path.Combine(rocksmithDirectory, "dlc"), "*_p.psarc", SearchOption.AllDirectories)
				.ToList();
			var songsArchive = Path.Combine(rocksmithDirectory, "songs.psarc");
			if (File.Exists(songsArchive))
			{
				archivePaths.Add(songsArchive);
			}

			foreach (var archivePath in archivePaths)
			{
				using (var archive = new PsarcFile(archivePath))
				{
					var bankEntry = archive.TOC.Entries.FirstOrDefault(entry =>
						RSMods.PsarcEntryPath.TryGetFileName(entry.Path, out string entryFileName)
						&& string.Equals(
							entryFileName,
							bankName,
							StringComparison.OrdinalIgnoreCase));
					if (bankEntry == null)
					{
						continue;
					}

					byte[] bankBytes;
					using (var bankStream = new MemoryStream())
					{
						archive.InflateEntry(bankEntry, bankStream);
						bankBytes = bankStream.ToArray();
					}

					var matchingWems = archive.TOC.Entries.Where(entry =>
					{
						if (!RSMods.PsarcEntryPath.TryGetWemMediaId(entry.Path, out uint mediaId))
						{
							return false;
						}

						return Contains(bankBytes, BitConverter.GetBytes(mediaId));
					}).ToList();
					if (matchingWems.Count != 1)
					{
						throw new InvalidDataException($"Expected one WEM for {bankName}; found {matchingWems.Count}.");
					}

					return new SpeakerSource(archivePath, matchingWems[0].Path);
				}
			}

			throw new FileNotFoundException($"Could not find bank {bankName}.");
		}

		private static bool Contains(byte[] source, byte[] value)
		{
			for (var sourceIndex = 0; sourceIndex <= source.Length - value.Length; sourceIndex++)
			{
				var matches = true;
				for (var valueIndex = 0; valueIndex < value.Length; valueIndex++)
				{
					if (source[sourceIndex + valueIndex] == value[valueIndex])
					{
						continue;
					}

					matches = false;
					break;
				}

				if (matches)
				{
					return true;
				}
			}

			return false;
		}

		private static void RunTool(string executablePath, string arguments)
		{
			var startInfo = new ProcessStartInfo
			{
				FileName = executablePath,
				Arguments = arguments,
				CreateNoWindow = true,
				UseShellExecute = false,
				RedirectStandardError = true,
				RedirectStandardOutput = true
			};

			using (var process = Process.Start(startInfo))
			{
				if (process == null)
				{
					throw new InvalidOperationException($"Could not start {executablePath}.");
				}

				var standardOutput = process.StandardOutput.ReadToEnd();
				var standardError = process.StandardError.ReadToEnd();
				process.WaitForExit();
				if (process.ExitCode != 0)
				{
					throw new InvalidOperationException($"{Path.GetFileName(executablePath)} failed: {standardOutput} {standardError}");
				}
			}
		}

		private static T Measure<T>(string name, Func<T> operation)
		{
			var timer = Stopwatch.StartNew();
			var result = operation();
			timer.Stop();
			Console.WriteLine($"{name,-38} {timer.Elapsed.TotalMilliseconds,9:F2} ms");
			return result;
		}

		private static void Measure(string name, Action operation)
		{
			Measure(name, () =>
			{
				operation();
				return true;
			});
		}

		private sealed class SpeakerSource
		{
			public string ArchivePath { get; }
			public string WemEntryPath { get; }

			public SpeakerSource(string archivePath, string wemEntryPath)
			{
				ArchivePath = archivePath;
				WemEntryPath = wemEntryPath;
			}
		}
	}
}
