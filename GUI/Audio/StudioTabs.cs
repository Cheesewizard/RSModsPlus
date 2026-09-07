using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal sealed class StudioTabs : TabControl
	{
		private int hoveredIndex = -1;

		public StudioTabs()
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
			Padding = new Point(24, 12);
			Font = StudioTheme.Strong;
		}

		protected override void OnMouseMove(MouseEventArgs args)
		{
			base.OnMouseMove(args);
			int hovered = -1;
			for (int index = 0; index < TabCount; index++)
			{
				if (GetTabRect(index).Contains(args.Location))
				{
					hovered = index;
					break;
				}
			}
			if (hovered == hoveredIndex)
				return;
			hoveredIndex = hovered;
			Invalidate();
		}

		protected override void OnMouseLeave(EventArgs args)
		{
			base.OnMouseLeave(args);
			if (hoveredIndex < 0)
				return;
			hoveredIndex = -1;
			Invalidate();
		}

		protected override void OnPaint(PaintEventArgs args)
		{
			var canvas = args.Graphics;
			canvas.Clear(StudioTheme.Background);
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			for (int index = 0; index < TabCount; index++)
			{
				var tab = GetTabRect(index);
				var pill = new Rectangle(tab.X + 2, tab.Y + 3, tab.Width - 4, tab.Height - 6);
				bool selected = index == SelectedIndex;
				using (var path = StudioTheme.RoundedRectangle(pill, pill.Height / 2))
				{
					if (selected)
					{
						using (var brush = new LinearGradientBrush(new Rectangle(pill.X, pill.Y, pill.Width, pill.Height + 1),
							StudioTheme.Blend(StudioTheme.Neutral, StudioTheme.Accent, 0.45), StudioTheme.Blend(StudioTheme.Neutral, StudioTheme.Accent, 0.18), LinearGradientMode.Vertical))
							canvas.FillPath(brush, path);
					}
					else if (index == hoveredIndex)
					{
						using (var brush = new SolidBrush(StudioTheme.Surface))
							canvas.FillPath(brush, path);
					}
				}
				TextRenderer.DrawText(canvas, TabPages[index].Text, selected ? StudioTheme.Strong : StudioTheme.Body, pill,
					selected ? Color.White : index == hoveredIndex ? StudioTheme.Ink : StudioTheme.Muted,
					TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
			}
		}
	}
}
