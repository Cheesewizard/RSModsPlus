using System;
using System.Drawing;
using System.Windows.Forms;
using RSMods.Audio;
using RSMods.Util;

namespace RSMods
{
	public partial class MainForm
	{
		private void AddAudioRoutingButton()
		{
			var button = new Button
			{
				Text = "Open audio bridge...",
				AutoSize = true,
				Location = new Point(650, 115)
			};
			button.Click += OpenAudioRouting;
			groupBox_RSModsPlus_CableInput.Controls.Add(button);
		}

		private void OpenAudioRouting(object sender, EventArgs args)
		{
			using (var window = new AudioRoutingWindow(GenUtil.GetRSDirectory()))
			{
				window.ShowDialog(this);
			}
		}
	}
}
