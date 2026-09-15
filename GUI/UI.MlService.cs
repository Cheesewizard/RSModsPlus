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
		private RSMods.Audio.StudioButton mlRestartButton;
		private DateTime mlRestartRequestedUntil;
		private int mlGameProcessId;

		private void InitializeMlServiceControls(FlowLayoutPanel page)
		{
			var group = new Panel
			{
				BackColor = RSMods.Audio.StudioTheme.Surface, ForeColor = RSMods.Audio.StudioTheme.Muted,
				Margin = new Padding(0, 8, 0, 0),
				Size = new Size(810, 108),
				TabIndex = 1
			};
			group.Controls.Add(new Label { Text = "ML NOTE DETECTION", Font = RSMods.Audio.StudioTheme.SectionFont, Location = new Point(16, 12), AutoSize = true });
			mlConnectionLabel = new Label { Location = new Point(16, 38), Size = new Size(595, 24), Text = "Checking ML connection…", AutoEllipsis = true };
			mlRestartButton = new RSMods.Audio.StudioButton("Restart ML service") { Location = new Point(622, 32), Size = new Size(170, 32), Enabled = false };
			mlRestartButton.Click += RestartMlService;
			group.Controls.Add(mlConnectionLabel);
			group.Controls.Add(mlRestartButton);
			group.Controls.Add(new Label { Location = new Point(16, 74), Size = new Size(778, 24), Font = RSMods.Audio.StudioTheme.Small, Text = "Status updates automatically. Restart reloads ML note detection while Rocksmith stays open." });
			page.Controls.Add(group);
			var timer = new Timer(components) { Interval = 1000 };
			timer.Tick += RefreshMlConnection;
			timer.Start();
			RefreshMlConnection(null, EventArgs.Empty);
		}

		private void RefreshMlConnection(object sender, EventArgs arguments)
		{
			try
			{
				var games = Process.GetProcessesByName("Rocksmith2014");
				try
				{
					mlGameProcessId = games.Length == 1 ? games[0].Id : 0;
					if (mlGameProcessId == 0)
					{
						ApplyMlConnectionState(games.Length == 0 ? "Disconnected — Rocksmith is not running." : "Multiple Rocksmith processes found — close the extra game before restarting ML.", false, false);
						return;
					}
					var connection = MlServiceConnection.Read(mlGameProcessId);
					if (DateTime.UtcNow < mlRestartRequestedUntil)
					{
						ApplyMlConnectionState("Restart requested — waiting for the game…", false, false);
						return;
					}
					ApplyMlConnectionState(connection.Message, connection.IsConnected, connection.CanRestart);
				}
				finally
				{
					foreach (var game in games) game.Dispose();
				}
			}
			catch (Exception exception)
			{
				ApplyMlConnectionState("Cannot read ML status: " + exception.Message, false, false);
				Trace.TraceError(exception.ToString());
			}
		}

		private void ApplyMlConnectionState(string message, bool isConnected, bool canRestart)
		{
			var colour = isConnected ? RSMods.Audio.StudioTheme.Positive : RSMods.Audio.StudioTheme.Muted;
			if (mlConnectionLabel.Text != message)
			{
				mlConnectionLabel.Text = message;
			}
			if (mlConnectionLabel.ForeColor != colour)
			{
				mlConnectionLabel.ForeColor = colour;
			}
			if (mlRestartButton.Enabled != canRestart)
			{
				mlRestartButton.Enabled = canRestart;
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
