using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace RSMods.Audio
{
	/// <summary>
	/// Sets up the "Rocksmith Audio Bridge" proxy ASIO driver from the GUI, so the user never edits a
	/// registry or an RS_ASIO.ini by hand. The proxy is registered machine-wide (HKLM, one elevation prompt)
	/// because ASIO hosts enumerate drivers only from HKLM; it is then "linked" to the user's real ASIO
	/// device: RS_ASIO is pointed at the proxy for output AND for every input that used the same physical
	/// driver, and the proxy is told which real driver to forward to. One shared host matters: when the
	/// interface is absent at launch the proxy still boots (virtually), so RS_ASIO keeps a capture device and
	/// the host can bind the real input the moment the interface arrives. Pointing only the output at the
	/// proxy leaves RS_ASIO asking the raw driver for input, which fails once at launch and never re-scans.
	/// Unlink restores the user's original RS_ASIO.ini values exactly.
	/// </summary>
	internal static class AsioProxySetup
	{
		public const string ProxyName = "Rocksmith Audio Bridge";
		private const string TargetKey = @"Software\RSMods\AsioProxy";
		// Must match CLSID_RocksmithAudioBridge in DLL/AsioProxy/AsioProxyDriver.cpp.
		private const string Clsid = "{7B2E5C10-9F3A-4D6B-A1C8-2E4F6A8B0D31}";
		private const string OutputBackupKey = "OriginalOutputDriver.RSMods";
		private const string InputBackupKey = "OriginalDriver.AudioBridge";
		private const string InputModeBackupKey = "OriginalDriver.RSMods";
		private static readonly string[] inputSections = { "Asio.Input.0", "Asio.Input.1", "Asio.Input.Mic" };

		/// <summary>Every ASIO driver Windows knows about, except our own proxy. This is the list the
		/// user picks from instead of reading an RS_ASIO log for the exact name.</summary>
		public static List<string> ListRealDrivers()
		{
			var names = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
			foreach (var hive in new[] { RegistryHive.LocalMachine, RegistryHive.CurrentUser })
			{
				try
				{
					using (var baseKey = RegistryKey.OpenBaseKey(hive, RegistryView.Registry32))
					using (var asio = baseKey.OpenSubKey(@"Software\ASIO"))
					{
						if (asio == null) continue;
						foreach (string name in asio.GetSubKeyNames())
							if (!string.Equals(name, ProxyName, StringComparison.OrdinalIgnoreCase))
								names.Add(name);
					}
				}
				catch { /* hive unreadable; skip */ }
			}
			return new List<string>(names);
		}

		/// <summary>True once the proxy is discoverable by ASIO hosts, i.e. its name is under HKLM\Software\ASIO.</summary>
		public static bool IsProxyRegistered()
		{
			try
			{
				using (var key = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32)
					.OpenSubKey(@"Software\ASIO\" + ProxyName))
					return key != null;
			}
			catch { return false; }
		}

		public static bool IsLinked(string gameDirectory)
		{
			if (string.IsNullOrWhiteSpace(gameDirectory)) return false;
			string ini = Path.Combine(Path.GetFullPath(gameDirectory), "RS_ASIO.ini");
			return File.Exists(ini) && string.Equals(ReadDriver(ini), ProxyName, StringComparison.OrdinalIgnoreCase);
		}

		/// <summary>Registers the proxy machine-wide by calling its own DllRegisterServer through an elevated,
		/// 32-bit regsvr32 (one UAC prompt). It must be HKLM: ASIO hosts, RS_ASIO included, enumerate drivers
		/// only from HKLM\Software\ASIO, so a per-user entry is invisible to them. It must be the 32-bit
		/// regsvr32 (SysWOW64) because the proxy is a 32-bit DLL. Throws OperationCanceledException if the user
		/// declines the elevation prompt.</summary>
		public static void Register(string proxyDllPath)
		{
			if (!File.Exists(proxyDllPath)) throw new FileNotFoundException("The audio bridge driver file is missing from the game folder. Reinstall RSModsPlus to restore it.", proxyDllPath);
			RunRegsvr32(proxyDllPath, unregister: false);
		}

		public static void Unregister(string proxyDllPath) => RunRegsvr32(proxyDllPath, unregister: true);

		private static void RunRegsvr32(string proxyDllPath, bool unregister)
		{
			// From this 64-bit process, C:\Windows\SysWOW64\regsvr32.exe is the 32-bit one that can load a
			// 32-bit DLL. /s keeps regsvr32 silent so the result is its exit code, not a message box.
			string regsvr = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "SysWOW64", "regsvr32.exe");
			string args = (unregister ? "/u /s \"" : "/s \"") + proxyDllPath + "\"";
			var info = new ProcessStartInfo(regsvr, args)
			{
				UseShellExecute = true,      // required for the "runas" (elevation) verb
				Verb = "runas",
				WindowStyle = ProcessWindowStyle.Hidden,
			};
			try
			{
				using (var process = Process.Start(info))
				{
					process.WaitForExit();
					if (process.ExitCode != 0)
						throw new InvalidOperationException("The driver registration step failed (regsvr32 exit code " + process.ExitCode + ").");
				}
			}
			catch (Win32Exception error) when (error.NativeErrorCode == 1223) // ERROR_CANCELLED
			{
				throw new OperationCanceledException("Registering the audio bridge driver needs a one-time Windows admin approval, and the prompt was declined.");
			}
		}

		/// <summary>Point RS_ASIO at one proxy host for output and any input using the same physical driver.
		/// The original values are preserved so Unlink can restore them verbatim.</summary>
		public static void Link(string gameDirectory, string realDriverName)
		{
			SelectTarget(realDriverName);
			SetPreferRealOutput(true);
			LinkSettings(Path.Combine(gameDirectory, "RS_ASIO.ini"), realDriverName);
		}

		internal static void LinkSettings(string ini, string realDriverName)
		{
			string current = ReadDriver(ini);
			if (!string.Equals(current, ProxyName, StringComparison.OrdinalIgnoreCase))
				WriteSetting(ini, OutputBackupKey, current);   // stash the real value once
			else if (string.IsNullOrWhiteSpace(ReadSetting(ini, OutputBackupKey)))
				WriteSetting(ini, OutputBackupKey, realDriverName);   // hand-edited to the proxy: record the real driver so Unlink and the proxy's fallback work
			WriteDriver(ini, ProxyName);
			foreach (string section in inputSections)
			{
				string inputDriver = ReadSection(ini, section, "Driver");
				if (!string.Equals(inputDriver, realDriverName, StringComparison.OrdinalIgnoreCase)) continue;
				WriteSection(ini, section, InputBackupKey, inputDriver);
				WriteSection(ini, section, "Driver", ProxyName);
			}
		}

		/// <summary>Re-link inputs for an ini whose output already names the proxy. Runs when the bridge
		/// window opens, so an ini that was pointed at the proxy by hand (output only) gains the shared input
		/// host without another Apply. The real driver comes from the stashed output value, else from the
		/// proxy's registry target.</summary>
		internal static void SynchronizeLinkedInputs(string gameDirectory)
		{
			string ini = Path.Combine(Path.GetFullPath(gameDirectory), "RS_ASIO.ini");
			if (!File.Exists(ini) || !string.Equals(ReadDriver(ini), ProxyName, StringComparison.OrdinalIgnoreCase)) return;
			string realDriverName = ReadSetting(ini, OutputBackupKey);
			if (string.IsNullOrWhiteSpace(realDriverName)) realDriverName = ReadTarget();
			if (string.IsNullOrWhiteSpace(realDriverName)) return;
			LinkSettings(ini, realDriverName);
		}

		public static string ReadTarget()
		{
			try
			{
				using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32).OpenSubKey(TargetKey))
					return key?.GetValue("Target") as string ?? "";
			}
			catch { return ""; }
		}

		public static void SelectTarget(string realDriverName)
		{
			using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32)
				.CreateSubKey(TargetKey))
				key.SetValue("Target", realDriverName ?? "", RegistryValueKind.String);
		}

		public static void SetPreferRealOutput(bool preferReal)
		{
			using (var key = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32)
				.CreateSubKey(TargetKey))
				key.SetValue("PreferReal", preferReal ? 1 : 0, RegistryValueKind.DWord);
		}

		public static void Unlink(string gameDirectory)
		{
			string ini = Path.Combine(gameDirectory, "RS_ASIO.ini");
			if (!File.Exists(ini)) return;
			string original = ReadSetting(ini, OutputBackupKey);
			if (!string.IsNullOrEmpty(original)) WriteDriver(ini, original);
			foreach (string section in inputSections)
			{
				string inputDriver = ReadSection(ini, section, InputBackupKey);
				if (string.IsNullOrEmpty(inputDriver)) continue;
				if (string.Equals(ReadSection(ini, section, "Driver"), ProxyName, StringComparison.OrdinalIgnoreCase))
					WriteSection(ini, section, "Driver", inputDriver);
				if (string.Equals(ReadSection(ini, section, InputModeBackupKey), ProxyName, StringComparison.OrdinalIgnoreCase))
					WriteSection(ini, section, InputModeBackupKey, inputDriver);
				WriteSection(ini, section, InputBackupKey, null);
			}
		}

		// RS_ASIO reads its .ini itself; we only ever write valid INI lines it can read.
		private static string ReadDriver(string ini) => ReadSection(ini, "Asio.Output", "Driver");
		private static void WriteDriver(string ini, string value)
		{
			if (!WritePrivateProfileString("Asio.Output", "Driver", value, ini))
				throw new IOException("Could not update the ASIO output driver.");
		}
		private static string ReadSetting(string ini, string key) => ReadSection(ini, "Asio.Output", key);
		private static void WriteSetting(string ini, string key, string value) => WriteSection(ini, "Asio.Output", key, value);
		private static void WriteSection(string ini, string section, string key, string value)
		{
			if (!WritePrivateProfileString(section, key, value, ini))
				throw new IOException("Could not update the ASIO settings.");
		}

		private static string ReadSection(string ini, string section, string key)
		{
			var sb = new StringBuilder(512);
			GetPrivateProfileString(section, key, "", sb, sb.Capacity, ini);
			return sb.ToString();
		}

		[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
		private static extern uint GetPrivateProfileString(string section, string key, string def, StringBuilder value, int size, string path);
		[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
		private static extern bool WritePrivateProfileString(string section, string key, string value, string path);
	}
}
