using Newtonsoft.Json.Linq;
using RocksmithToolkitLib.DLCPackage;
using RSMods.Util;
using SevenZip;
using System;
using System.Diagnostics;
using System.IO;

namespace RSMods
{
	internal static class NoteByNoteMenuInstaller
	{
		private const string INSTALL_ARGUMENT = "--install-note-by-note-menu";
		private const string CACHE_DIRECTORY_NAME = "cache_psarc_RS2014_Pc";
		private const string CACHE_ARCHIVE_NAME = "cache7.7z";
		private const string MANIFEST_INTERNAL_PATH = "manifests\\ui_menu_pillar_learnasong.database.json";
		private const string MANIFEST_FILE_NAME = "ui_menu_pillar_learnasong.database.json";

		private const string DUMP_ARGUMENT = "--dump-note-by-note-menu";

		public static bool TryRun(string[] arguments, out int exitCode)
		{
			exitCode = 0;
			if (arguments.Length == 0
				|| (!string.Equals(arguments[0], INSTALL_ARGUMENT, StringComparison.OrdinalIgnoreCase)
					&& !string.Equals(arguments[0], DUMP_ARGUMENT, StringComparison.OrdinalIgnoreCase)))
			{
				return false;
			}

			try
			{
				if (string.Equals(arguments[0], DUMP_ARGUMENT, StringComparison.OrdinalIgnoreCase))
				{
					// Diagnostic (2026-09-02, issue #62): write the Learn a Song menu manifest out
					// of a cache.psarc so the shipped slider definitions can be compared with the
					// injected NoteByNote entry. Usage: --dump-note-by-note-menu <cache.psarc> <out.json>
					if (arguments.Length != 3)
					{
						throw new ArgumentException($"Usage: {DUMP_ARGUMENT} <cache.psarc> <output.json>");
					}

					DumpManifest(arguments[1], arguments[2]);
					return true;
				}

				if (arguments.Length != 2)
				{
					throw new ArgumentException($"Usage: {INSTALL_ARGUMENT} <Rocksmith directory>");
				}

				Install(arguments[1]);
			}
			catch (Exception exception)
			{
				exitCode = 1;
				File.WriteAllText(
					Path.Combine(System.Windows.Forms.Application.StartupPath, "NoteByNoteMenuInstall.log"),
					exception.ToString());
			}

			return true;
		}

		private static void Install(string rocksmithDirectory)
		{
			if (Process.GetProcessesByName("Rocksmith2014").Length != 0)
			{
				throw new InvalidOperationException("Rocksmith must be closed before installing the Note by Note menu.");
			}

			var gameDirectory = Path.GetFullPath(rocksmithDirectory);
			var cachePath = Path.Combine(gameDirectory, "cache.psarc");
			if (!File.Exists(cachePath))
			{
				throw new FileNotFoundException("Rocksmith cache.psarc was not found.", cachePath);
			}

			var backupPath = Path.Combine(gameDirectory, "cache.pre-note-by-note-menu.psarc");
			if (!File.Exists(backupPath))
			{
				File.Copy(cachePath, backupPath);
			}

			var installDirectory = Path.Combine(
				Path.GetTempPath(),
				$"RSModsPlus_NoteByNoteMenu_{Guid.NewGuid():N}");
			Directory.CreateDirectory(installDirectory);

			try
			{
				Packer.Unpack(cachePath, installDirectory);
				var unpackedCacheDirectory = Path.Combine(installDirectory, CACHE_DIRECTORY_NAME);
				var cacheArchivePath = Path.Combine(unpackedCacheDirectory, CACHE_ARCHIVE_NAME);
				if (!File.Exists(cacheArchivePath))
				{
					throw new InvalidDataException("The unpacked cache does not contain cache7.7z.");
				}

				if (!ZipUtilities.ExtractSingleFile(
					installDirectory,
					cacheArchivePath,
					MANIFEST_INTERNAL_PATH))
				{
					throw new InvalidDataException("The Learn a Song menu manifest could not be extracted.");
				}

				var manifestPath = Path.Combine(installDirectory, MANIFEST_FILE_NAME);
				AddNoteByNoteToggle(manifestPath);
				if (!ZipUtilities.InjectFile(
					manifestPath,
					cacheArchivePath,
					MANIFEST_INTERNAL_PATH,
					OutArchiveFormat.SevenZip,
					CompressionMode.Append))
				{
					throw new InvalidDataException("The Note by Note menu manifest could not be injected.");
				}

				var pendingCachePath = Path.Combine(installDirectory, "cache.note-by-note.pending.psarc");
				Packer.Pack(unpackedCacheDirectory, pendingCachePath);
				if (!File.Exists(pendingCachePath) || new FileInfo(pendingCachePath).Length < 1024 * 1024)
				{
					throw new InvalidDataException("The rebuilt Rocksmith cache is missing or incomplete.");
				}

				File.Copy(pendingCachePath, cachePath, true);
				File.WriteAllText(
					Path.Combine(System.Windows.Forms.Application.StartupPath, "NoteByNoteMenuInstall.log"),
					$"Installed Note by Note into Riff Repeater Advanced Settings.{Environment.NewLine}"
					+ $"Backup: {backupPath}{Environment.NewLine}"
					+ $"Installed cache size: {new FileInfo(cachePath).Length}{Environment.NewLine}");
			}
			finally
			{
				Directory.Delete(installDirectory, true);
			}
		}

		private static void DumpManifest(string cachePath, string outputPath)
		{
			cachePath = Path.GetFullPath(cachePath);
			if (!File.Exists(cachePath))
			{
				throw new FileNotFoundException("cache.psarc was not found.", cachePath);
			}

			var workDirectory = Path.Combine(
				Path.GetTempPath(),
				$"RSModsPlus_NoteByNoteMenuDump_{Guid.NewGuid():N}");
			Directory.CreateDirectory(workDirectory);
			try
			{
				Packer.Unpack(cachePath, workDirectory);
				// The unpack folder is named after the archive file, so a backup such as
				// cache.pre-note-by-note-menu.psarc lands somewhere other than CACHE_DIRECTORY_NAME.
				var archives = Directory.GetFiles(workDirectory, CACHE_ARCHIVE_NAME, SearchOption.AllDirectories);
				if (archives.Length == 0)
				{
					throw new InvalidDataException("The unpacked cache does not contain cache7.7z.");
				}
				var cacheArchivePath = archives[0];
				// An output DIRECTORY receives the whole cache7.7z (every UI manifest), so the
				// shipped slider definitions in the other pillar manifests can be compared too.
				if (Directory.Exists(outputPath))
				{
					File.Copy(cacheArchivePath, Path.Combine(Path.GetFullPath(outputPath), CACHE_ARCHIVE_NAME), true);
					return;
				}
				if (!ZipUtilities.ExtractSingleFile(workDirectory, cacheArchivePath, MANIFEST_INTERNAL_PATH))
				{
					throw new InvalidDataException("The Learn a Song menu manifest could not be extracted.");
				}
				File.Copy(Path.Combine(workDirectory, MANIFEST_FILE_NAME), Path.GetFullPath(outputPath), true);
			}
			finally
			{
				Directory.Delete(workDirectory, true);
			}
		}

		private static void AddNoteByNoteToggle(string manifestPath)
		{
			var root = JObject.Parse(File.ReadAllText(manifestPath));
			var buttons = root["Static"]?["UI"]?["Menus"]?["Entries"]?
				["RiffRepeater_AdvancedSettings"]?["View"]?["Definition"]?["Buttons"] as JObject;
			if (buttons == null)
			{
				throw new InvalidDataException("RiffRepeater_AdvancedSettings.Buttons was not found.");
			}

			// Place NOTE BY NOTE as the LAST item with a contiguous SortOrder. The native
			// Riff Repeater menu builds its keyboard-navigation index from a contiguous
			// 0..N-1 SortOrder run (confirmed against the shipped pillar manifests, whose
			// menus all use 0-based contiguous SortOrders). The original -1 sat OUTSIDE
			// that run, so the cursor and the visible list desynced: the highlight started
			// on NOTE BY NOTE, the down arrow jumped up into the Riff Repeater items,
			// cycled to the end of them, then finally reached Difficulty Repeats (a classic
			// contiguity break). max(existing SortOrder)+1 appends it cleanly at the end and
			// keeps the run contiguous. The existing NoteByNote entry (if any) is skipped so
			// re-installs stay idempotent instead of walking the index forward each run.
			int noteByNoteSortOrder = 0;
			foreach (var property in buttons.Properties())
			{
				if (string.Equals(property.Name, "NoteByNote", StringComparison.Ordinal))
				{
					continue;
				}

				if (property.Value is JObject existingButton
					&& existingButton["SortOrder"] is JValue sortValue
					&& sortValue.Type == JTokenType.Integer)
				{
					int existingSortOrder = sortValue.Value<int>();
					if (existingSortOrder + 1 > noteByNoteSortOrder)
					{
						noteByNoteSortOrder = existingSortOrder + 1;
					}
				}
			}

			// The eight shipped Advanced Settings rows are not in this manifest at all: the
			// LAS_RiffRepeater controller creates them in code (2026-09-02, confirmed by dumping
			// the shipped cache: this Buttons object is empty). With no JSON siblings the
			// computation above yields 0, which drew NOTE BY NOTE at the TOP while the game's
			// navigation index appended it after the eight native rows, so the highlight and
			// the visible list disagreed. Sort it after the native rows so both orders match.
			// Tested 2026-09-02: SortOrder 8 loads fine but does NOT move the row; the game draws
			// the JSON rows before its code-built ones regardless, and the cursor starts on the
			// JSON row, so the visible and navigation orders already agree with it first. Kept at
			// the contiguous-run value (0 with no JSON siblings); the position is not ours to set.
			const int NATIVE_ADVANCED_SETTINGS_ROWS = 0;
			if (noteByNoteSortOrder < NATIVE_ADVANCED_SETTINGS_ROWS)
			{
				noteByNoteSortOrder = NATIVE_ADVANCED_SETTINGS_ROWS;
			}

			buttons["NoteByNote"] = new JObject
			{
				["ID"] = "NoteByNote",
				["Label"] = "NOTE BY NOTE",
				["State"] = "up",
				["SortOrder"] = noteByNoteSortOrder,
				["Component"] = "RSSlider",
				["Notched"] = true,
				["XScale"] = 100,
				["YScale"] = 80,
				["InitialValue"] = 0,
				["States"] = new JObject
				{
					["Default"] = true
				},
				["AcceptedValues"] = new JObject
				{
					["0"] = "$[23447]Off",
					["1"] = "$[23446]On"
				}
			};

			File.WriteAllText(manifestPath, root.ToString());
		}
	}
}
