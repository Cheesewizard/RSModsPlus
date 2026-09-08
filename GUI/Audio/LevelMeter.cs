using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal sealed class LevelMeter : Control
	{
		private const double FloorDecibels = -60.0;
		private const int ReadoutWidth = 74;
		private static readonly double[] Ticks = { -48, -36, -24, -12, -6 };
		private double level;
		private double hold;
		private DateTime holdTaken = DateTime.MinValue;
		private DateTime clipped = DateTime.MinValue;

		public LevelMeter()
		{
			Height = 26;
			Margin = new Padding(0, 6, 0, 6);
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
			BackColor = Color.Transparent;
		}

		public bool IsClipping
		{
			get { return (DateTime.UtcNow - clipped).TotalSeconds < 1.5; }
		}

		public void SetLevel(int peak)
		{
			double target = Fraction(peak);
			level = target > level ? target : level * 0.72 + target * 0.28;
			if (peak >= 1000)
				clipped = DateTime.UtcNow;
			if (level >= hold || (DateTime.UtcNow - holdTaken).TotalSeconds > 1.2)
			{
				hold = level;
				holdTaken = DateTime.UtcNow;
			}
			Invalidate();
		}

		public void Reset()
		{
			level = 0;
			hold = 0;
			clipped = DateTime.MinValue;
			Invalidate();
		}

		public static string Describe(int peak)
		{
			if (peak <= 0)
				return "silent";
			return (20.0 * Math.Log10(Math.Min(1000, peak) / 1000.0)).ToString("0.0") + " dB";
		}

		internal static double Fraction(int peak)
		{
			if (peak <= 0)
				return 0;
			double decibels = 20.0 * Math.Log10(Math.Min(1000, peak) / 1000.0);
			return Math.Max(0, Math.Min(1, (decibels - FloorDecibels) / -FloorDecibels));
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
			int trackWidth = Math.Max(40, Width - ReadoutWidth);
			var track = new Rectangle(0, Height / 2 - 8, trackWidth, 16);
			using (var path = StudioTheme.RoundedRectangle(track, 4))
			{
				using (var brush = new SolidBrush(StudioTheme.Field))
					e.Graphics.FillPath(brush, path);
				e.Graphics.SetClip(path);
				DrawFill(e.Graphics, track);
				DrawTicks(e.Graphics, track);
				e.Graphics.ResetClip();
				using (var pen = new Pen(StudioTheme.Line))
					e.Graphics.DrawPath(pen, path);
			}
			var readout = new Rectangle(track.Right + 10, 0, ReadoutWidth - 10, Height);
			bool clipping = IsClipping;
			TextRenderer.DrawText(e.Graphics, clipping ? "CLIP" : Readout(), clipping ? StudioTheme.Strong : StudioTheme.Small, readout,
				clipping ? StudioTheme.Record : level <= 0 ? StudioTheme.Faint : StudioTheme.Muted,
				TextFormatFlags.Right | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
		}

		private string Readout()
		{
			if (level <= 0)
				return "silent";
			return (FloorDecibels + level * -FloorDecibels).ToString("0.0") + " dB";
		}

		private void DrawFill(Graphics graphics, Rectangle track)
		{
			if (level <= 0)
				return;
			var gradient = new Rectangle(track.X, track.Y, track.Width, track.Height);
			using (var brush = new LinearGradientBrush(gradient, StudioTheme.Positive, StudioTheme.Record, LinearGradientMode.Horizontal))
			{
				brush.InterpolationColors = new ColorBlend
				{
					Colors = new[] { StudioTheme.Positive, StudioTheme.Positive, StudioTheme.Warning, StudioTheme.Record },
					Positions = new[] { 0f, 0.72f, 0.88f, 1f }
				};
				graphics.FillRectangle(brush, track.X, track.Y, (float)(track.Width * level), track.Height);
			}
			if (hold > 0.01)
			{
				float x = track.X + (float)(track.Width * hold) - 2;
				using (var brush = new SolidBrush(Color.FromArgb(210, 255, 255, 255)))
					graphics.FillRectangle(brush, x, track.Y + 2, 2, track.Height - 4);
			}
		}

		private void DrawTicks(Graphics graphics, Rectangle track)
		{
			using (var pen = new Pen(Color.FromArgb(48, 0, 0, 0)))
			{
				foreach (double decibels in Ticks)
				{
					float x = track.X + (float)(track.Width * (decibels - FloorDecibels) / -FloorDecibels);
					graphics.DrawLine(pen, x, track.Y, x, track.Bottom);
				}
			}
		}
	}
}
