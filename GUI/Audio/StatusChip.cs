using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal enum ChipTone
	{
		Idle,
		Good,
		Warn,
		Bad,
		Info
	}

	internal sealed class StatusChip : Control
	{
		private ChipTone tone = ChipTone.Idle;

		public StatusChip(string text = "")
		{
			Font = StudioTheme.Small;
			Height = 24;
			Margin = new Padding(0, 3, 8, 3);
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
			BackColor = Color.Transparent;
			Text = text;
		}

		public ChipTone Tone
		{
			get { return tone; }
			set { if (tone != value) { tone = value; Invalidate(); } }
		}

		public void Set(string text, ChipTone chipTone)
		{
			Tone = chipTone;
			Text = text;
		}

		public override string Text
		{
			get { return base.Text; }
			set
			{
				if (base.Text == value)
					return;
				base.Text = value;
				Width = TextRenderer.MeasureText(value ?? "", Font).Width + 34;
				Invalidate();
			}
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
			Color accent = ToneColor();
			var bounds = new Rectangle(0, 0, Width - 1, Height - 1);
			using (var path = StudioTheme.RoundedRectangle(bounds, Height / 2))
			{
				using (var brush = new SolidBrush(StudioTheme.Blend(StudioTheme.Surface, accent, 0.16)))
					e.Graphics.FillPath(brush, path);
				using (var pen = new Pen(StudioTheme.Blend(StudioTheme.Line, accent, 0.35)))
					e.Graphics.DrawPath(pen, path);
			}
			using (var brush = new SolidBrush(accent))
				e.Graphics.FillEllipse(brush, new Rectangle(10, Height / 2 - 3, 7, 7));
			TextRenderer.DrawText(e.Graphics, Text, Font, new Rectangle(22, 0, Width - 28, Height),
				tone == ChipTone.Idle ? StudioTheme.Muted : StudioTheme.Ink,
				TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
		}

		private Color ToneColor()
		{
			switch (tone)
			{
				case ChipTone.Good: return StudioTheme.Positive;
				case ChipTone.Warn: return StudioTheme.Warning;
				case ChipTone.Bad: return StudioTheme.Record;
				case ChipTone.Info: return StudioTheme.Accent;
				default: return StudioTheme.Faint;
			}
		}
	}
}
