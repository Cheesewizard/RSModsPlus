using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// Studio check box. The system CheckBox draws a light Windows box that ignores the dark theme;
	/// this one paints a rounded box in the theme colours with an accent fill when checked. Same
	/// members the panel used on CheckBox (Text, Checked, CheckedChanged, Enabled).
	/// </summary>
	internal sealed class StudioCheck : Control
	{
		private const int BoxSize = 18;
		private const int Gap = 9;

		private bool isChecked;
		private bool hovered;

		public event EventHandler CheckedChanged;

		public StudioCheck(string text = "")
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.Selectable | ControlStyles.StandardClick, true);
			Font = StudioTheme.Body;
			ForeColor = StudioTheme.Ink;
			Cursor = Cursors.Hand;
			TabStop = true;
			AccessibleRole = AccessibleRole.CheckButton;
			Margin = new Padding(0, 6, 0, 4);
			Height = 26;
			Text = text;
		}

		public bool Checked
		{
			get => isChecked;
			set
			{
				if (isChecked == value) return;
				isChecked = value;
				Invalidate();
				CheckedChanged?.Invoke(this, EventArgs.Empty);
			}
		}

		public override string Text
		{
			get => base.Text;
			set
			{
				base.Text = value;
				Width = BoxSize + Gap + TextRenderer.MeasureText(value ?? "", Font).Width + 6;
				Invalidate();
			}
		}

		protected override void OnFontChanged(EventArgs e)
		{
			base.OnFontChanged(e);
			Text = Text;
		}

		protected override void OnClick(EventArgs e)
		{
			if (Enabled) Checked = !Checked;
			base.OnClick(e);
		}

		protected override void OnKeyDown(KeyEventArgs e)
		{
			if (Enabled && e.KeyCode == Keys.Space)
			{
				Checked = !Checked;
				e.Handled = e.SuppressKeyPress = true;
			}
			base.OnKeyDown(e);
		}

		protected override void OnMouseDown(MouseEventArgs e) { Focus(); base.OnMouseDown(e); }

		protected override void OnMouseEnter(EventArgs e) { hovered = true; Invalidate(); base.OnMouseEnter(e); }

		protected override void OnMouseLeave(EventArgs e) { hovered = false; Invalidate(); base.OnMouseLeave(e); }

		protected override void OnEnabledChanged(EventArgs e) { Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); base.OnEnabledChanged(e); }

		protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }

		protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

		protected override void OnPaint(PaintEventArgs e)
		{
			var canvas = e.Graphics;
			canvas.SmoothingMode = SmoothingMode.AntiAlias;
			var box = new Rectangle(1, Height / 2 - BoxSize / 2, BoxSize - 1, BoxSize - 1);
			Color fill = isChecked && Enabled ? StudioTheme.Accent : StudioTheme.Field;
			if (hovered && Enabled) fill = StudioTheme.Shift(fill, 14);
			if (!Enabled) fill = StudioTheme.Blend(fill, StudioTheme.Surface, 0.5);
			using (var path = StudioTheme.RoundedRectangle(box, 4))
			{
				using (var brush = new SolidBrush(fill))
					canvas.FillPath(brush, path);
				Color edge = Focused && Enabled ? StudioTheme.Accent : isChecked && Enabled ? StudioTheme.Shift(StudioTheme.Accent, 30) : StudioTheme.Line;
				using (var pen = new Pen(edge))
					canvas.DrawPath(pen, path);
			}
			if (isChecked)
			{
				using (var pen = new Pen(Enabled ? Color.White : StudioTheme.Faint, 2f) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
					canvas.DrawLines(pen, new[]
					{
						new PointF(box.X + 4.5f, box.Y + box.Height / 2f + 0.5f),
						new PointF(box.X + box.Width / 2f - 0.5f, box.Bottom - 4.5f),
						new PointF(box.Right - 4f, box.Y + 4.5f)
					});
			}
			var text = new Rectangle(box.Right + Gap, 0, Width - box.Right - Gap, Height);
			TextRenderer.DrawText(canvas, Text, Font, text, Enabled ? ForeColor : StudioTheme.Faint,
				TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.EndEllipsis);
		}
	}
}
