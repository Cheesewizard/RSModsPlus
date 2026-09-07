using System;
using System.Windows.Forms;
using System.Security.Principal;

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
				ApplicationBase = System.IO.Directory.GetParent(System.IO.Path.GetDirectoryName(executable)).FullName,
				PrivateBinPath = System.IO.Path.GetFileName(System.IO.Path.GetDirectoryName(executable)),
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
				if (arguments.Length == 0) MessageBox.Show(exception.ToString(), "RSMods startup error");
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
				Application.Run(new MainForm());
			}
			catch (Exception ex)
			{
				MessageBox.Show(ex.Message + " " + ex, "Error");
			}
		}
	}
}
