using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal sealed class SegmentedControl : Control
	{
		public event EventHandler SelectedIndexChanged;
		private readonly string[] segments;
		private readonly int[] widths;
		private int selectedIndex;
		private int hoveredIndex = -1;

		public SegmentedControl(params string[] segments)
		{
			if (segments == null || segments.Length < 2)
				throw new ArgumentException("A segmented control needs at least two segments.", nameof(segments));
			this.segments = segments;
			widths = new int[segments.Length];
			Font = StudioTheme.Body;
			Height = 34;
			Margin = new Padding(0, 4, 0, 4);
			Cursor = Cursors.Hand;
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
			BackColor = Color.Transparent;
			Measure();
		}

		public int SelectedIndex
		{
			get { return selectedIndex; }
			set
			{
				int clamped = Math.Max(0, Math.Min(segments.Length - 1, value));
				if (clamped == selectedIndex)
					return;
				selectedIndex = clamped;
				Invalidate();
				SelectedIndexChanged?.Invoke(this, EventArgs.Empty);
			}
		}

		private void Measure()
		{
			int total = 8;
			for (int index = 0; index < segments.Length; index++)
			{
				widths[index] = TextRenderer.MeasureText(segments[index], StudioTheme.Strong).Width + 30;
				total += widths[index];
			}
			Width = total;
		}

		protected override void OnMouseMove(MouseEventArgs e)
		{
			int index = IndexAt(e.X);
			if (index != hoveredIndex)
			{
				hoveredIndex = index;
				Invalidate();
			}
			base.OnMouseMove(e);
		}

		protected override void OnMouseLeave(EventArgs e)
		{
			hoveredIndex = -1;
			Invalidate();
			base.OnMouseLeave(e);
		}

		protected override void OnMouseDown(MouseEventArgs e)
		{
			if (Enabled)
			{
				int index = IndexAt(e.X);
				if (index >= 0)
					SelectedIndex = index;
			}
			base.OnMouseDown(e);
		}

		protected override void OnEnabledChanged(EventArgs e)
		{
			Cursor = Enabled ? Cursors.Hand : Cursors.Default;
			Invalidate();
			base.OnEnabledChanged(e);
		}

		private int IndexAt(int x)
		{
			int offset = 4;
			for (int index = 0; index < segments.Length; index++)
			{
				if (x >= offset && x < offset + widths[index])
					return index;
				offset += widths[index];
			}
			return -1;
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
			using (var path = StudioTheme.RoundedRectangle(new Rectangle(0, 0, Width - 1, Height - 1), 7))
			{
				using (var brush = new SolidBrush(StudioTheme.Field))
					e.Graphics.FillPath(brush, path);
				using (var pen = new Pen(StudioTheme.Line))
					e.Graphics.DrawPath(pen, path);
			}
			int offset = 4;
			for (int index = 0; index < segments.Length; index++)
			{
				var bounds = new Rectangle(offset, 4, widths[index], Height - 9);
				bool active = index == selectedIndex;
				if (active)
					using (var path = StudioTheme.RoundedRectangle(bounds, 5))
					using (var brush = new SolidBrush(Enabled ? StudioTheme.Blend(StudioTheme.Neutral, StudioTheme.Accent, 0.35) : StudioTheme.Neutral))
						e.Graphics.FillPath(brush, path);
				else if (Enabled && index == hoveredIndex)
					using (var path = StudioTheme.RoundedRectangle(bounds, 5))
					using (var brush = new SolidBrush(StudioTheme.Shift(StudioTheme.Field, 10)))
						e.Graphics.FillPath(brush, path);
				Color ink = !Enabled ? StudioTheme.Faint : active ? Color.White : StudioTheme.Muted;
				TextRenderer.DrawText(e.Graphics, segments[index], active ? StudioTheme.Strong : StudioTheme.Body, bounds, ink,
					TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
				offset += widths[index];
			}
		}
	}
}
