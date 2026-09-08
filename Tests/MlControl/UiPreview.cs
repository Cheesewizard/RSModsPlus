using System;
using System.ComponentModel;
using System.Drawing;
using System.Windows.Forms;

namespace RSMods
{
	public partial class MainForm : Form
	{
		protected override bool ShowWithoutActivation => true;

		private readonly Container components = new Container();
		private readonly TabPage tab_RSModsPlus = new TabPage("RSModsPlus") { BackColor = Color.Azure };
		private readonly GroupBox groupBox_RSModsPlus_AudioStatus = new GroupBox { Text = "Audio status" };
		private readonly TextBox textBox_RSModsPlus_AudioStatus = new TextBox { Location = new Point(16, 54), Multiline = true, ReadOnly = true };

		[STAThread]
		private static void Main(string[] arguments)
		{
			Application.EnableVisualStyles();
			using (var form = new MainForm())
			{
				form.ClientSize = new Size(1158, 539);
				form.ShowInTaskbar = false;
				form.StartPosition = FormStartPosition.Manual;
				form.Location = new Point(-20000, -20000);
				var tabs = new TabControl { Dock = DockStyle.Fill };
				tabs.TabPages.Add(form.tab_RSModsPlus);
				form.Controls.Add(tabs);
				form.tab_RSModsPlus.Controls.Add(form.groupBox_RSModsPlus_AudioStatus);
				form.groupBox_RSModsPlus_AudioStatus.Controls.Add(form.textBox_RSModsPlus_AudioStatus);
				form.InitializeMlServiceControls();
				form.Show();
				form.Refresh();
				using (var bitmap = new Bitmap(form.Width, form.Height))
				{
					form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
					bitmap.Save(arguments[0]);
				}
				form.components.Dispose();
			}
		}
	}
}
