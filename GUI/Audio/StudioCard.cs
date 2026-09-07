using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal sealed class StudioCard : Panel
	{
		private readonly TableLayoutPanel body;

		public StudioCard(string title = null, bool stretch = false)
		{
			BackColor = StudioTheme.Background;
			Padding = new Padding(18, 14, 18, 16);
			Margin = new Padding(0, 0, 0, 12);
			AutoSize = !stretch;
			AutoSizeMode = AutoSizeMode.GrowAndShrink;
			SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
			body = new TableLayoutPanel
			{
				Dock = DockStyle.Fill,
				ColumnCount = 1,
				AutoSize = !stretch,
				AutoSizeMode = AutoSizeMode.GrowAndShrink,
				BackColor = StudioTheme.Surface
			};
			body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			Controls.Add(body);
			if (!string.IsNullOrEmpty(title))
			{
				var heading = StudioTheme.Section(title);
				heading.Margin = new Padding(0, 0, 0, 6);
				Add(heading);
			}
		}

		public void Add(Control control, bool fill = false)
		{
			body.RowStyles.Add(fill ? new RowStyle(SizeType.Percent, 100) : new RowStyle(SizeType.AutoSize));
			if (fill)
				control.Dock = DockStyle.Fill;
			body.Controls.Add(control, 0, body.RowCount++);
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			base.OnPaint(e);
			e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
			using (var path = StudioTheme.RoundedRectangle(new Rectangle(0, 0, Width - 1, Height - 1), 10))
			{
				using (var brush = new SolidBrush(StudioTheme.Surface))
					e.Graphics.FillPath(brush, path);
				using (var pen = new Pen(StudioTheme.Line))
					e.Graphics.DrawPath(pen, path);
			}
		}
	}
}
