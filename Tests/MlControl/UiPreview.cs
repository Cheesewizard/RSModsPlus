using System;
using System.ComponentModel;
using System.Drawing;
using System.Windows.Forms;
using RSMods.Audio;

namespace RSMods
{
	public partial class MainForm : Form
	{
		protected override bool ShowWithoutActivation => true;

		private readonly Container components = new Container();

		[STAThread]
		private static void Main(string[] arguments)
		{
			Application.EnableVisualStyles();
			using (var form = new MainForm())
			{
				form.ClientSize = new Size(870, 160);
				form.ShowInTaskbar = false;
				form.StartPosition = FormStartPosition.Manual;
				form.Location = new Point(-20000, -20000);
				var page = new FlowLayoutPanel { Dock = DockStyle.Fill, BackColor = StudioTheme.Background, Font = StudioTheme.Body };
				form.Controls.Add(page);
				form.InitializeMlServiceControls(page);
				form.components.Dispose();
				StudioTheme.EnableDoubleBuffering(form);
				form.Show();
				form.VerifyStableState("ML service running — initializing or waiting for game audio.", false, true);
				form.VerifyStableState("Connected — receiving fresh ML results.", true, true);
				form.VerifyStableState("Restart requested — waiting for the game…", false, false);
				form.VerifyStableState("Cannot read ML status: test error", false, false);
				form.VerifyStableState("Disconnected — Rocksmith is not running.", false, false);
				form.VerifyStableState("ML service running — initializing or waiting for game audio.", false, true);
				if (arguments.Length > 0)
				{
					using (var bitmap = new Bitmap(form.Width, form.Height))
					{
						form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
						bitmap.Save(arguments[0]);
					}
				}
				Console.WriteLine("PASS: six ML state transitions; 600 unchanged updates produced no control changes or invalidations.");
			}
		}

		private void VerifyStableState(string message, bool isConnected, bool canRestart)
		{
			ApplyMlConnectionState(message, isConnected, canRestart);
			if (mlConnectionLabel.Text != message || mlConnectionLabel.ForeColor != (isConnected ? StudioTheme.Positive : StudioTheme.Muted) || mlRestartButton.Enabled != canRestart)
			{
				throw new InvalidOperationException("ML state was not applied correctly.");
			}
			Update();
			var changes = 0;
			EventHandler changed = (sender, args) => changes++;
			InvalidateEventHandler invalidated = (sender, args) => changes++;
			mlRestartButton.EnabledChanged += changed;
			mlConnectionLabel.ForeColorChanged += changed;
			mlConnectionLabel.TextChanged += changed;
			mlRestartButton.Invalidated += invalidated;
			mlConnectionLabel.Invalidated += invalidated;
			try
			{
				for (var index = 0; index < 100; index++)
				{
					ApplyMlConnectionState(message, isConnected, canRestart);
					Update();
				}
				if (changes != 0) throw new InvalidOperationException("Unchanged ML state caused " + changes + " control changes/invalidations.");
			}
			finally
			{
				mlRestartButton.EnabledChanged -= changed;
				mlConnectionLabel.ForeColorChanged -= changed;
				mlConnectionLabel.TextChanged -= changed;
				mlRestartButton.Invalidated -= invalidated;
				mlConnectionLabel.Invalidated -= invalidated;
			}
		}
	}
}
