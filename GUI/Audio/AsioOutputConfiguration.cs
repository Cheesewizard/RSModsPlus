using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace RSMods.Audio
{
	internal sealed class AsioOutputConfiguration
	{
		private readonly string path;
		private readonly string backupPath;
		private byte[] previousContent;
		private byte[] appliedContent;

		public AsioOutputConfiguration(string gameDirectory)
		{
			path = Path.Combine(gameDirectory, "RS_ASIO.ini");
			backupPath = Path.Combine(gameDirectory, "RS_ASIO.before-audio-routing.ini");
		}

		public void Apply(bool enabled, bool wasEnabled)
		{
			if (!File.Exists(path)) throw new IOException("The ASIO settings file is missing.");
			if (!enabled && !wasEnabled) return;
			string currentDriver = Read(path, "Asio.Output", "Driver");
			string currentWindowsOutput = Read(path, "Config", "EnableWasapiOutputs");
			if (wasEnabled && (currentDriver.Length != 0 || currentWindowsOutput != "1"))
			{
				throw new InvalidOperationException("ASIO output settings were changed outside this tool. They have been left untouched.");
			}
			if (enabled && wasEnabled) return;
			string driver;
			string windowsOutput;
			if (enabled)
			{
				File.Copy(path, backupPath, true);
				driver = "";
				windowsOutput = "1";
			}
			else
			{
				if (!File.Exists(backupPath)) throw new IOException("The saved ASIO output configuration is missing. No settings were changed.");
				driver = Read(backupPath, "Asio.Output", "Driver");
				windowsOutput = Read(backupPath, "Config", "EnableWasapiOutputs");
			}
			previousContent = File.ReadAllBytes(path);
			string temporaryPath = path + ".routing.tmp";
			File.WriteAllBytes(temporaryPath, previousContent);
			Write(temporaryPath, "Config", "EnableWasapiOutputs", windowsOutput);
			Write(temporaryPath, "Asio.Output", "Driver", driver);
			appliedContent = File.ReadAllBytes(temporaryPath);
			File.Replace(temporaryPath, path, null);
		}

		public void UndoFailedSave()
		{
			if (previousContent == null || appliedContent == null) return;
			byte[] current = File.ReadAllBytes(path);
			if (current.Length != appliedContent.Length) throw new IOException("ASIO settings changed during save; automatic restoration was not applied.");
			for (int index = 0; index < current.Length; ++index)
			{
				if (current[index] != appliedContent[index]) throw new IOException("ASIO settings changed during save; automatic restoration was not applied.");
			}
			string temporaryPath = path + ".routing.tmp";
			File.WriteAllBytes(temporaryPath, previousContent);
			File.Replace(temporaryPath, path, null);
		}

		private static string Read(string file, string section, string key)
		{
			var value = new StringBuilder(2048);
			GetPrivateProfileString(section, key, "", value, value.Capacity, file);
			return value.ToString();
		}

		private static void Write(string file, string section, string key, string value)
		{
			if (!WritePrivateProfileString(section, key, value, file)) throw new IOException("Could not update ASIO output settings.");
		}

		[DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetPrivateProfileStringW")]
		private static extern uint GetPrivateProfileString(string section, string key, string defaultValue, StringBuilder value, int capacity, string path);

		[DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "WritePrivateProfileStringW", SetLastError = true)]
		[return: MarshalAs(UnmanagedType.Bool)]
		private static extern bool WritePrivateProfileString(string section, string key, string value, string path);
	}
}
