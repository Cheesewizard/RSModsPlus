using System;
using System.Security.Principal;
using System.Threading;
using System.Windows.Forms;

namespace RSMods
{
	static class Program
	{
		// Single-instance guard for the audio bridge. Whoever holds this mutex owns the one bridge
		// window; a second launch (from the main window's button, or the game DLL auto-launching a
		// background bridge) just brings the existing window forward and exits.
		private const string AudioBridgeInstanceMutex = "Global\\RSModsPlus.AudioBridge.SingleInstance";

		[STAThread]
		static void Main(string[] arguments)
		{
			if (AppDomain.CurrentDomain.GetData("RSMods.GameDomain") is bool)
			{
				Start(arguments);
				return;
			}

			var executable = System.Reflection.Assembly.GetExecutingAssembly().Location;
			var setup = new AppDomainSetup
			{
				ApplicationBase = System.IO.Path.GetDirectoryName(executable),
				ConfigurationFile = AppDomain.CurrentDomain.SetupInformation.ConfigurationFile
			};
			var domain = AppDomain.CreateDomain("RSMods game runtime", null, setup);
			try
			{
				domain.SetData("RSMods.GameDomain", true);
				domain.ExecuteAssembly(executable, arguments);
			}
			finally
			{
				AppDomain.Unload(domain);
			}
		}

		private static void Start(string[] arguments)
		{
			try
			{
				RuntimeBootstrap.Initialize();
				Run(arguments);
			}
			catch (Exception exception)
			{
				Console.Error.WriteLine(exception);
				var logDirectory = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RSModsPlus", "Logs");
				try
				{
					System.IO.Directory.CreateDirectory(logDirectory);
					System.IO.File.WriteAllText(System.IO.Path.Combine(logDirectory, "startup-error.log"), DateTime.UtcNow.ToString("O") + Environment.NewLine + exception);
				}
				catch (Exception loggingException)
				{
					Console.Error.WriteLine(loggingException);
				}
				Environment.ExitCode = 1;
				if (!IsBackgroundAudioBridge(arguments)) MessageBox.Show(exception.ToString(), "RSMods startup error");
			}
		}

		[System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
		private static void Run(string[] arguments)
		{
			if (MlServiceHost.TryRun(arguments, out int serviceExitCode))
			{
				Environment.ExitCode = serviceExitCode;
				return;
			}

			if (MlModelVerifier.TryRun(arguments, out int modelExitCode))
			{
				Environment.ExitCode = modelExitCode;
				return;
			}

			if (NoteByNoteMenuInstaller.TryRun(arguments, out int menuInstallExitCode))
			{
				Environment.ExitCode = menuInstallExitCode;
				return;
			}

			if (NoteByNoteChartExtractor.TryRun(arguments, out int chartExitCode))
			{
				Environment.ExitCode = chartExitCode;
				return;
			}

			if (SpeakerModeCacheExtractor.TryRun(arguments, out int exitCode))
			{
				Environment.ExitCode = exitCode;
				return;
			}

			try
			{
				Application.EnableVisualStyles();
				Application.SetCompatibleTextRenderingDefault(false);

				if (arguments.Length > 0 && arguments[0] == "--audio-bridge")
				{
					RunAudioBridge(arguments);
					return;
				}

				Application.Run(new MainForm());
			}
			catch (Exception ex)
			{
				if (IsBackgroundAudioBridge(arguments)) throw;
				MessageBox.Show(ex.Message + " " + ex, "Error");
			}
		}

		private static void RunAudioBridge(string[] arguments)
		{
			Mutex audioBridgeInstance = null;
			bool isFirstInstance;
			try
			{
				audioBridgeInstance = new Mutex(true, AudioBridgeInstanceMutex, out isFirstInstance);
			}
			catch (UnauthorizedAccessException)
			{
				// The mutex exists but belongs to a process at a higher integrity level (a bridge
				// launched by an elevated Rocksmith). That still means "another instance owns it".
				isFirstInstance = false;
			}

			using (audioBridgeInstance)
			{
				if (!isFirstInstance)
				{
					// The owner may still be creating its window; give it a moment before giving up.
					for (int attempt = 0; attempt < 10; ++attempt)
					{
						if (Audio.AudioBridgeWindow.TryBringToFront(forceRestore: true) != Audio.AudioBridgeWindow.Presence.NotFound)
							break;
						Thread.Sleep(100);
					}
					return;
				}

				if ((arguments.Length != 2 && arguments.Length != 4 && arguments.Length != 5)
					|| !System.IO.Path.IsPathRooted(arguments[1]) || !System.IO.Directory.Exists(arguments[1]))
					throw new ArgumentException("Audio bridge requires an existing absolute game directory.");

				var rocksmithProcessId = 0;
				if (arguments.Length >= 4
					&& (arguments[2] != "--rocksmith-pid" || !int.TryParse(arguments[3], out rocksmithProcessId) || rocksmithProcessId <= 0))
					throw new ArgumentException("Background Audio Bridge requires a valid Rocksmith process ID.");
				var showWindow = arguments.Length == 5 && arguments[4] == "--show";
				if (arguments.Length == 5 && !showWindow)
					throw new ArgumentException("Unknown Audio Bridge launch option.");

				Application.Run(new Audio.AudioRoutingWindow(
					System.IO.Path.GetFullPath(arguments[1]),
					rocksmithProcessId,
					showWindow));
			}
		}

		private static bool IsBackgroundAudioBridge(string[] arguments)
		{
			return arguments.Length >= 3 && arguments[0] == "--audio-bridge" && arguments[2] == "--rocksmith-pid";
		}
	}
}
