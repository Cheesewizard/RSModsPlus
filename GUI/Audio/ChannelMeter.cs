using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>Slim vertical output meter that sits beside the master fader.</summary>
	internal sealed class ChannelMeter : Control
	{
		private const int BarWidth = 10;
		private double level;
		private double hold;
		private int paintedTop = -1;
		private int paintedHold = -1;
		private DateTime holdTaken = DateTime.MinValue;

		public ChannelMeter()
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
			Width = 18;
		}

		public void SetLevel(int peak)
		{
			double target = LevelMeter.Fraction(peak);
			level = target > level ? target : level * 0.72 + target * 0.28;
			if (level >= hold || (DateTime.UtcNow - holdTaken).TotalSeconds > 1.2)
			{
				hold = level;
				holdTaken = DateTime.UtcNow;
			}
			InvalidateWhenMoved();
		}

		public void Reset()
		{
			level = 0;
			hold = 0;
			InvalidateWhenMoved();
		}

		/// <summary>Repaints only when the bar or its peak hold lands on a different pixel row.</summary>
		private void InvalidateWhenMoved()
		{
			int top = (int)Math.Round(level * Math.Max(0, Height));
			int peak = (int)Math.Round(hold * Math.Max(0, Height));
			if (top == paintedTop && peak == paintedHold)
				return;
			paintedTop = top;
			paintedHold = peak;
			Invalidate();
		}

		protected override void OnPaint(PaintEventArgs args)
		{
			base.OnPaint(args);
			var canvas = args.Graphics;
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			int inset = Math.Max(10, DeviceDpi / 10);
			var bar = new Rectangle(Width / 2 - BarWidth / 2, inset, BarWidth, Math.Max(1, Height - inset * 2));
			using (var path = StudioTheme.RoundedRectangle(bar, BarWidth / 2))
			{
				using (var brush = new SolidBrush(StudioTheme.Well))
					canvas.FillPath(brush, path);
				canvas.SetClip(path);
				if (level > 0.001)
				{
					int top = bar.Bottom - (int)Math.Round(bar.Height * level);
					using (var brush = new LinearGradientBrush(new Rectangle(bar.X, bar.Y, bar.Width, bar.Height + 1),
						StudioTheme.Record, StudioTheme.Positive, LinearGradientMode.Vertical))
					{
						brush.InterpolationColors = new ColorBlend
						{
							Colors = new[] { StudioTheme.Record, StudioTheme.Warning, StudioTheme.Positive, StudioTheme.Positive },
							Positions = new[] { 0f, 0.12f, 0.28f, 1f }
						};
						canvas.FillRectangle(brush, bar.X, top, bar.Width, bar.Bottom - top);
					}
				}
				if (hold > 0.01)
				{
					float y = bar.Bottom - (float)(bar.Height * hold);
					using (var brush = new SolidBrush(Color.FromArgb(205, 255, 255, 255)))
						canvas.FillRectangle(brush, bar.X, y - 1, bar.Width, 2);
				}
				canvas.ResetClip();
				using (var pen = new Pen(StudioTheme.Line))
					canvas.DrawPath(pen, path);
			}
		}
	}
}
