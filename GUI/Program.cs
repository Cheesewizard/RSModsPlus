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
