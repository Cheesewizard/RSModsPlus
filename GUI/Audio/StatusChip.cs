using System.Drawing;
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

	/// <summary>Square status tag: a framed field with a colour bar on its left edge saying the tone.</summary>
	internal sealed class StatusChip : Control
	{
		private ChipTone tone = ChipTone.Idle;

		public StatusChip(string text = "")
		{
			Font = StudioTheme.Small;
			Height = 24;
			Margin = new Padding(0, 3, 8, 3);
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
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
				Width = TextRenderer.MeasureText(value ?? "", Font).Width + 26;
				Invalidate();
			}
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			Color accent = ToneColor();
			using (var brush = new SolidBrush(StudioTheme.Blend(StudioTheme.Field, accent, tone == ChipTone.Idle ? 0.0 : 0.12)))
				e.Graphics.FillRectangle(brush, 0, 0, Width, Height);
			using (var pen = new Pen(StudioTheme.Blend(StudioTheme.Line, accent, tone == ChipTone.Idle ? 0.0 : 0.4)))
				e.Graphics.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
			using (var brush = new SolidBrush(tone == ChipTone.Idle ? StudioTheme.Line : accent))
				e.Graphics.FillRectangle(brush, 0, 0, 3, Height);
			TextRenderer.DrawText(e.Graphics, Text, Font, new Rectangle(12, 0, Width - 16, Height),
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
