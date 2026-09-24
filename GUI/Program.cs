using System;
using System.Security.Principal;
using System.Threading;
using System.Windows.Forms;

namespace RSMods
{
	static class Program
	{
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
				if (!IsBackgroundAudioBridge(arguments) && !(arguments.Length > 0 && arguments[0] == "--video-take"))
					MessageBox.Show(exception.ToString(), "RSMods startup error");
			}
		}

		[System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
		private static void Run(string[] arguments)
		{
			// Hidden video take for the in-game overlay and record hotkey (no window, exits when the take ends).
			if (Audio.VideoTakeHost.TryRun(arguments, out int videoExitCode))
			{
				Environment.ExitCode = videoExitCode;
				return;
			}

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

				// The desktop Audio Bridge window was retired on 2026-09-23 (the in-game overlay replaced it). A launch
				// from an older game DLL still passes --audio-bridge; it now exits without opening anything.
				if (arguments.Length > 0 && arguments[0] == "--audio-bridge")
					return;

				Application.Run(new MainForm());
			}
			catch (Exception ex)
			{
				if (IsBackgroundAudioBridge(arguments)) throw;
				MessageBox.Show(ex.Message + " " + ex, "Error");
			}
		}

		private static bool IsBackgroundAudioBridge(string[] arguments)
		{
			return arguments.Length >= 3 && arguments[0] == "--audio-bridge" && arguments[2] == "--rocksmith-pid";
		}
	}
}
