using System.Drawing;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal sealed class AudioRoutingWindow : Form
	{
		private readonly AudioRoutingPanel panel;

		public AudioRoutingWindow(string gameDirectory)
		{
			Text = "RSModsPlus · Audio bridge";
			StartPosition = FormStartPosition.CenterParent;
			ClientSize = new Size(1060, 880);
			MinimumSize = new Size(940, 620);
			BackColor = StudioTheme.Background;
			ForeColor = StudioTheme.Ink;
			panel = new AudioRoutingPanel(gameDirectory) { Dock = DockStyle.Fill };
			Controls.Add(panel);
			FormClosing += WarnWhileBusy;
		}

		private void WarnWhileBusy(object sender, FormClosingEventArgs args)
		{
			if (!panel.IsBusy)
				return;
			args.Cancel = true;
			MessageBox.Show(this, "An audio operation is still running. Finish the take or cancel the buffer test before closing.",
				"Audio bridge", MessageBoxButtons.OK, MessageBoxIcon.Warning);
		}
	}
}
