using System.Collections.Generic;
using System.Drawing;
using System.Windows.Forms;

namespace RSMods.Audio
{
	/// <summary>
	/// A framed panel: 1 px border, a solid header strip carrying the title, content below. Every section of
	/// the bridge sits in one of these so the tabs share one framing language.
	/// </summary>
	internal sealed class StudioCard : Panel
	{
		public const int HeaderHeight = 30;
		private readonly TableLayoutPanel body;
		private readonly List<Label> wrapLabels = new List<Label>();
		private readonly string title;

		public StudioCard(string title = null, bool stretch = false)
		{
			this.title = title;
			BackColor = StudioTheme.Surface;
			int top = string.IsNullOrEmpty(title) ? 14 : HeaderHeight + 12;
			Padding = new Padding(17, top, 17, 14);
			Margin = new Padding(0, 0, 0, 12);
			AutoSize = !stretch;
			AutoSizeMode = AutoSizeMode.GrowAndShrink;
			SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
			body = new TableLayoutPanel
			{
				Dock = DockStyle.Fill,
				ColumnCount = 1,
				AutoSize = !stretch,
				AutoSizeMode = AutoSizeMode.GrowAndShrink
			};
			body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			Controls.Add(body);
		}

		public void Add(Control control, bool fill = false)
		{
			body.RowStyles.Add(fill ? new RowStyle(SizeType.Percent, 100) : new RowStyle(SizeType.AutoSize));
			if (fill)
				control.Dock = DockStyle.Fill;
			else if (control is Label label && label.AutoSize)
			{
				// An AutoSize label lays its text on a single line and gets clipped by the card on a
				// narrow column. To wrap it we have to give it a definite maximum width; that width is
				// the card's, so it is set (and kept in sync) from the card's own size in ApplyWrapWidth.
				label.Anchor = AnchorStyles.Left | AnchorStyles.Right;
				wrapLabels.Add(label);
				ApplyWrapWidth();
			}
			body.Controls.Add(control, 0, body.RowCount++);
		}

		// Cap each wrapping label at the card's inner width so its text wraps to the card and the label
		// grows in height, instead of running off the edge. Re-run whenever the card is resized.
		private void ApplyWrapWidth()
		{
			int available = ClientSize.Width - Padding.Horizontal;
			if (available <= 1) return;
			foreach (var label in wrapLabels)
			{
				int width = available - label.Margin.Horizontal;
				if (width > 1) label.MaximumSize = new Size(width, 0);
			}
		}

		protected override void OnClientSizeChanged(System.EventArgs e)
		{
			base.OnClientSizeChanged(e);
			ApplyWrapWidth();
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			base.OnPaint(e);
			var canvas = e.Graphics;
			if (!string.IsNullOrEmpty(title))
			{
				using (var brush = new SolidBrush(StudioTheme.Header))
					canvas.FillRectangle(brush, 1, 1, Width - 2, HeaderHeight);
				using (var pen = new Pen(StudioTheme.Line))
					canvas.DrawLine(pen, 1, HeaderHeight, Width - 2, HeaderHeight);
				TextRenderer.DrawText(canvas, title.ToUpperInvariant(), StudioTheme.SectionFont,
					new Rectangle(16, 1, Width - 32, HeaderHeight), StudioTheme.Ink,
					TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.EndEllipsis);
			}
			using (var pen = new Pen(StudioTheme.Line))
				canvas.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
		}
	}
}
