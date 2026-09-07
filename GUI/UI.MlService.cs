using System;
using System.Diagnostics;
using System.Drawing;
using System.Windows.Forms;
using RSModsPlus.MachineLearning;

namespace RSMods
{
	public partial class MainForm
	{
		private Label mlConnectionLabel;
		private Button mlRestartButton;
		private DateTime mlRestartRequestedUntil;
		private int mlGameProcessId;

		private void InitializeMlServiceControls()
		{
			var group = new GroupBox
			{
				Text = "ML note detection",
				Location = new Point(16, 180),
				Size = new Size(1110, 90),
				TabIndex = 1
			};
			mlConnectionLabel = new Label { Location = new Point(16, 26), Size = new Size(885, 24), Text = "Checking ML connection…", AutoEllipsis = true };
			mlRestartButton = new Button { Text = "Restart ML service", Location = new Point(920, 22), Size = new Size(170, 28), Enabled = false };
			mlRestartButton.Click += RestartMlService;
			group.Controls.Add(mlConnectionLabel);
			group.Controls.Add(mlRestartButton);
			group.Controls.Add(new Label { Location = new Point(16, 56), Size = new Size(1078, 20), Text = "Status updates automatically. Restart reloads ML note detection while Rocksmith stays open." });
			tab_RSModsPlus.Controls.Add(group);
			groupBox_RSModsPlus_AudioStatus.Location = new Point(16, 284);
			groupBox_RSModsPlus_AudioStatus.Size = new Size(1110, 212);
			groupBox_RSModsPlus_AudioStatus.TabIndex = 2;
			textBox_RSModsPlus_AudioStatus.Size = new Size(1078, 142);
			var timer = new Timer(components) { Interval = 1000 };
			timer.Tick += RefreshMlConnection;
			timer.Start();
			RefreshMlConnection(null, EventArgs.Empty);
		}

		private void RefreshMlConnection(object sender, EventArgs arguments)
		{
			mlRestartButton.Enabled = false;
			mlConnectionLabel.ForeColor = SystemColors.ControlText;
			try
			{
				var games = Process.GetProcessesByName("Rocksmith2014");
				try
				{
					mlGameProcessId = games.Length == 1 ? games[0].Id : 0;
					if (mlGameProcessId == 0)
					{
						mlConnectionLabel.Text = games.Length == 0 ? "Disconnected — Rocksmith is not running." : "Multiple Rocksmith processes found — close the extra game before restarting ML.";
						return;
					}
					var connection = MlServiceConnection.Read(mlGameProcessId);
					if (DateTime.UtcNow < mlRestartRequestedUntil)
					{
						mlConnectionLabel.Text = "Restart requested — waiting for the game…";
						return;
					}
					mlConnectionLabel.Text = connection.Message;
					mlConnectionLabel.ForeColor = connection.IsConnected ? Color.DarkGreen : SystemColors.ControlText;
					mlRestartButton.Enabled = connection.CanRestart;
				}
				finally
				{
					foreach (var game in games) game.Dispose();
				}
			}
			catch (Exception exception)
			{
				mlConnectionLabel.Text = "Cannot read ML status: " + exception.Message;
				Trace.TraceError(exception.ToString());
			}
		}

		private void RestartMlService(object sender, EventArgs arguments)
		{
			try
			{
				MlServiceConnection.RequestRestart(mlGameProcessId);
				mlRestartRequestedUntil = DateTime.UtcNow.AddSeconds(2);
				RefreshMlConnection(null, EventArgs.Empty);
			}
			catch (Exception exception)
			{
				Trace.TraceError(exception.ToString());
				MessageBox.Show(this, exception.Message, "ML restart failed", MessageBoxButtons.OK, MessageBoxIcon.Error);
			}
		}
	}
}
