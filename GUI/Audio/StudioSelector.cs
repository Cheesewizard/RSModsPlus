using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>Drop-down list that paints its own frame, so it does not carry the light Windows border into the dark studio.</summary>
	internal sealed class StudioSelector : ComboBox
	{
		private const int WmPaint = 0x000F;
		private const int ArrowWidth = 26;

		public StudioSelector()
		{
			StudioTheme.StyleSelector(this);
		}

		protected override void WndProc(ref Message message)
		{
			base.WndProc(ref message);
			if (message.Msg != WmPaint || !IsHandleCreated)
				return;
			using (var canvas = Graphics.FromHwnd(Handle))
			{
				canvas.SmoothingMode = SmoothingMode.AntiAlias;
				var arrow = new Rectangle(Width - ArrowWidth, 1, ArrowWidth - 2, Height - 2);
				using (var brush = new SolidBrush(StudioTheme.Field))
					canvas.FillRectangle(brush, arrow);
				using (var pen = new Pen(Enabled ? StudioTheme.Muted : StudioTheme.Faint, 1.8f) { StartCap = LineCap.Round, EndCap = LineCap.Round })
				{
					float centre = arrow.X + arrow.Width / 2f;
					float middle = Height / 2f;
					canvas.DrawLines(pen, new[]
					{
						new PointF(centre - 4, middle - 2), new PointF(centre, middle + 2), new PointF(centre + 4, middle - 2)
					});
				}
				canvas.SmoothingMode = SmoothingMode.None;
				using (var pen = new Pen(Focused ? StudioTheme.Blend(StudioTheme.Line, StudioTheme.Accent, 0.7) : StudioTheme.Line, 2))
					canvas.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
			}
		}
	}
}
