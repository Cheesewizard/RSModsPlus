using System;
using System.Diagnostics;
using System.IO;

namespace RSMods.Audio
{
	internal enum AudioInputMode
	{
		Cable,
		Asio,
		Unavailable
	}

	internal sealed class AudioInputModeConfiguration
	{
		private readonly string gameDirectory;
		private readonly Func<bool> isGameRunning;
		private static readonly string[] fileNames = { "avrt.dll", "RS_ASIO.dll" };

		public AudioInputModeConfiguration(string gameDirectory, Func<bool> isGameRunning = null)
		{
			if (string.IsNullOrWhiteSpace(gameDirectory)) throw new ArgumentException("Rocksmith folder is required.", nameof(gameDirectory));
			this.gameDirectory = Path.GetFullPath(gameDirectory);
			this.isGameRunning = isGameRunning ?? IsGameRunning;
		}

		public AudioInputMode ReadMode()
		{
			int enabled = 0;
			int disabled = 0;
			foreach (string name in fileNames)
			{
				if (File.Exists(Path.Combine(gameDirectory, name))) enabled++;
				if (File.Exists(Path.Combine(gameDirectory, name + ".disabled"))) disabled++;
			}
			if (enabled == 2 && disabled == 0) return AudioInputMode.Asio;
			if (enabled == 0 && disabled == 2) return AudioInputMode.Cable;
			if (enabled == 0 && disabled == 0) return AudioInputMode.Unavailable;
			throw new InvalidOperationException("ASIO files are incomplete or conflicting. Expected both avrt.dll and RS_ASIO.dll, or both with .disabled appended. No files were changed.");
		}

		public bool IsGameRunning()
		{
			foreach (var process in Process.GetProcessesByName("Rocksmith2014"))
			{
				using (process)
				{
					if (string.Equals(Path.GetDirectoryName(process.MainModule.FileName), gameDirectory.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) return true;
				}
			}
			return false;
		}

		public void SetAsioEnabled(bool enabled)
		{
			using (var modeLock = new FileStream(Path.Combine(gameDirectory, ".rsmods-audio-mode.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
			{
				if (isGameRunning()) throw new InvalidOperationException("Close Rocksmith before changing ASIO / Cable mode.");
				var current = ReadMode();
				if (current == AudioInputMode.Unavailable) throw new InvalidOperationException("Install RS_ASIO before enabling ASIO mode.");
				if ((current == AudioInputMode.Asio) == enabled) return;
				string sourceSuffix = enabled ? ".disabled" : "";
				string destinationSuffix = enabled ? "" : ".disabled";
				string firstSource = Path.Combine(gameDirectory, fileNames[0] + sourceSuffix);
				string firstDestination = Path.Combine(gameDirectory, fileNames[0] + destinationSuffix);
				File.Move(firstSource, firstDestination);
				try
				{
					File.Move(Path.Combine(gameDirectory, fileNames[1] + sourceSuffix), Path.Combine(gameDirectory, fileNames[1] + destinationSuffix));
				}
				catch (Exception error)
				{
					try { File.Move(firstDestination, firstSource); }
					catch (Exception rollbackError) { throw new AggregateException("ASIO mode change and restoration failed. Check both DLL names before launching Rocksmith.", error, rollbackError); }
					throw;
				}
			}
		}
	}
}
