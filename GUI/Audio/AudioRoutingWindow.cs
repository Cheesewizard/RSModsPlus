using System;
using System.Diagnostics;
using System.Drawing;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal sealed class AudioRoutingWindow : Form
	{
		private const int TOGGLE_RECORDING_MESSAGE = 0x8421;

		private readonly AudioRoutingPanel panel;
		private readonly Process rocksmithProcess;
		private readonly Timer rocksmithLifetimeTimer;
		private bool isClosingWithRocksmith;

		public AudioRoutingWindow(string gameDirectory) : this(gameDirectory, 0)
		{
		}

		internal AudioRoutingWindow(string gameDirectory, int rocksmithProcessId)
		{
			Text = AudioBridgeWindow.Title;
			Icon = Properties.Resources.AudioBridgeIcon;
			DoubleBuffered = true;
			StartPosition = FormStartPosition.CenterScreen;
			ClientSize = new Size(1060, 880);
			MinimumSize = new Size(940, 620);
			BackColor = StudioTheme.Background;
			ForeColor = StudioTheme.Ink;
			panel = new AudioRoutingPanel(gameDirectory) { Dock = DockStyle.Fill };
			if (rocksmithProcessId > 0)
			{
				rocksmithProcess = Process.GetProcessById(rocksmithProcessId);
				WindowState = FormWindowState.Minimized;
				rocksmithLifetimeTimer = new Timer { Interval = 500 };
				rocksmithLifetimeTimer.Tick += CheckRocksmithLifetime;
				rocksmithLifetimeTimer.Start();
			}
			Controls.Add(panel);
			FormClosing += WarnWhileBusy;
			FormClosed += ReleaseRocksmithLifetime;
		}

		protected override void WndProc(ref Message message)
		{
			if (message.Msg == TOGGLE_RECORDING_MESSAGE)
			{
				panel.ToggleRecordingFromHotkey();
				return;
			}
			base.WndProc(ref message);
		}

		private void WarnWhileBusy(object sender, FormClosingEventArgs args)
		{
			if (isClosingWithRocksmith)
				return;
			if (rocksmithProcess != null && !rocksmithProcess.HasExited)
			{
				args.Cancel = true;
				WindowState = FormWindowState.Minimized;
				return;
			}
			if (!panel.IsBusy)
				return;
			args.Cancel = true;
			MessageBox.Show(this, "An audio operation is still running. Finish the take or cancel the buffer test before closing.",
				"Audio bridge", MessageBoxButtons.OK, MessageBoxIcon.Warning);
		}

		private void CheckRocksmithLifetime(object sender, EventArgs args)
		{
			if (!rocksmithProcess.HasExited)
				return;
			isClosingWithRocksmith = true;
			Close();
		}

		private void ReleaseRocksmithLifetime(object sender, FormClosedEventArgs args)
		{
			rocksmithLifetimeTimer?.Stop();
			rocksmithLifetimeTimer?.Dispose();
			rocksmithProcess?.Dispose();
		}
	}
}
