using System;
using System.Drawing;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// Left navigation column: one flat list of sections. Replaces the tab strip and its nested sub-tabs,
	/// which read as two menus stacked on each other. The selected row is a raised block with an accent bar
	/// on its left edge; a hairline down the right edge separates the column from the page.
	/// </summary>
	internal sealed class StudioNav : Control
	{
		private const int RowHeight = 40;
		private readonly string[] items;
		private int selectedIndex;
		private int hoveredIndex = -1;

		public event EventHandler SelectedIndexChanged;

		public StudioNav(params string[] items)
		{
			if (items == null || items.Length == 0)
				throw new ArgumentException("The navigation needs at least one section.", nameof(items));
			this.items = items;
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
			Font = StudioTheme.Strong;
			Width = 164;
			Cursor = Cursors.Hand;
			TabStop = true;
			AccessibleRole = AccessibleRole.List;
		}

		public int SelectedIndex
		{
			get => selectedIndex;
			set
			{
				int clamped = Math.Max(0, Math.Min(items.Length - 1, value));
				if (clamped == selectedIndex) return;
				selectedIndex = clamped;
				Invalidate();
				SelectedIndexChanged?.Invoke(this, EventArgs.Empty);
			}
		}

		private int IndexAt(int y)
		{
			int index = y / RowHeight;
			return index >= 0 && index < items.Length ? index : -1;
		}

		protected override void OnMouseMove(MouseEventArgs e)
		{
			int index = IndexAt(e.Y);
			if (index != hoveredIndex) { hoveredIndex = index; Invalidate(); }
			base.OnMouseMove(e);
		}

		protected override void OnMouseLeave(EventArgs e) { hoveredIndex = -1; Invalidate(); base.OnMouseLeave(e); }

		protected override void OnMouseDown(MouseEventArgs e)
		{
			Focus();
			if (Enabled && e.Button == MouseButtons.Left)
			{
				int index = IndexAt(e.Y);
				if (index >= 0) SelectedIndex = index;
			}
			base.OnMouseDown(e);
		}

		protected override bool IsInputKey(Keys keyData)
		{
			return keyData == Keys.Up || keyData == Keys.Down || base.IsInputKey(keyData);
		}

		protected override void OnKeyDown(KeyEventArgs e)
		{
			if (Enabled && e.KeyCode == Keys.Up) { SelectedIndex = selectedIndex - 1; e.Handled = true; }
			else if (Enabled && e.KeyCode == Keys.Down) { SelectedIndex = selectedIndex + 1; e.Handled = true; }
			base.OnKeyDown(e);
		}

		protected override void OnEnabledChanged(EventArgs e) { Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); base.OnEnabledChanged(e); }

		protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }

		protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

		protected override void OnPaint(PaintEventArgs e)
		{
			var canvas = e.Graphics;
			for (int index = 0; index < items.Length; index++)
			{
				var row = new Rectangle(0, index * RowHeight, Width - 1, RowHeight);
				bool selected = index == selectedIndex;
				if (selected)
				{
					using (var brush = new SolidBrush(StudioTheme.Surface))
						canvas.FillRectangle(brush, row);
					using (var pen = new Pen(StudioTheme.Line))
						canvas.DrawRectangle(pen, row.X, row.Y, row.Width, row.Height);
					using (var brush = new SolidBrush(Enabled ? StudioTheme.Accent : StudioTheme.Faint))
						canvas.FillRectangle(brush, 0, row.Y, 4, row.Height + 1);
				}
				else if (index == hoveredIndex && Enabled)
				{
					using (var brush = new SolidBrush(StudioTheme.Shift(StudioTheme.Background, 8)))
						canvas.FillRectangle(brush, row);
				}
				Color ink = !Enabled ? StudioTheme.Faint : selected || index == hoveredIndex ? StudioTheme.Ink : StudioTheme.Muted;
				TextRenderer.DrawText(canvas, items[index], selected ? StudioTheme.Strong : StudioTheme.Body,
					new Rectangle(18, row.Y, row.Width - 22, row.Height), ink,
					TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.EndEllipsis);
			}
			using (var pen = new Pen(StudioTheme.Line))
				canvas.DrawLine(pen, Width - 1, 0, Width - 1, Height);
			if (Focused && Enabled)
				using (var pen = new Pen(StudioTheme.Blend(StudioTheme.Line, StudioTheme.Accent, 0.6)))
					canvas.DrawRectangle(pen, 1, selectedIndex * RowHeight + 1, Width - 4, RowHeight - 2);
		}
	}
}
