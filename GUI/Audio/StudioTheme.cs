using System.Drawing;
using System.Reflection;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace RSMods.Audio
{
	internal static class StudioTheme
	{
		public static readonly Color Background = Color.FromArgb(18, 20, 26);
		public static readonly Color Surface = Color.FromArgb(27, 31, 39);
		public static readonly Color Elevated = Color.FromArgb(35, 40, 50);
		public static readonly Color Well = Color.FromArgb(13, 15, 20);
		public static readonly Color Field = Color.FromArgb(36, 41, 52);
		public static readonly Color Line = Color.FromArgb(48, 55, 68);
		public static readonly Color Ink = Color.FromArgb(233, 237, 243);
		public static readonly Color Muted = Color.FromArgb(156, 166, 180);
		public static readonly Color Faint = Color.FromArgb(110, 120, 134);
		public static readonly Color Record = Color.FromArgb(233, 76, 82);
		public static readonly Color Positive = Color.FromArgb(60, 205, 140);
		public static readonly Color Warning = Color.FromArgb(240, 172, 52);
		public static readonly Color Accent = Color.FromArgb(82, 145, 250);
		public static readonly Color Violet = Color.FromArgb(150, 132, 250);
		public static readonly Color Teal = Color.FromArgb(70, 190, 205);
		public static readonly Color Neutral = Color.FromArgb(45, 52, 66);

		public static readonly Font Display = new Font("Segoe UI", 20F, FontStyle.Bold);
		public static readonly Font Timecode = new Font("Consolas", 28F, FontStyle.Bold);
		public static readonly Font Body = new Font("Segoe UI", 9.75F);
		public static readonly Font Strong = new Font("Segoe UI", 9.75F, FontStyle.Bold);
		public static readonly Font Small = new Font("Segoe UI", 8.25F);
		public static readonly Font Readout = new Font("Segoe UI", 11F, FontStyle.Bold);
		public static readonly Font SectionFont = new Font("Segoe UI", 8.25F, FontStyle.Bold);

		public static Label Text(string text)
		{
			return new Label { Text = text, AutoSize = true, UseMnemonic = false, Font = Body, ForeColor = Ink, Margin = new Padding(0, 4, 0, 4) };
		}

		public static Label Hint(string text)
		{
			return new Label { Text = text, AutoSize = true, UseMnemonic = false, Font = Small, ForeColor = Muted, MaximumSize = new Size(420, 0), Margin = new Padding(0, 2, 0, 6) };
		}

		public static Label Section(string text)
		{
			return new Label { Text = text.ToUpperInvariant(), AutoSize = true, UseMnemonic = false, Font = SectionFont, ForeColor = Faint, Margin = new Padding(0, 10, 0, 4) };
		}

		public static void StyleField(TextBox field)
		{
			field.ReadOnly = true;
			field.Cursor = Cursors.Arrow;
			field.BackColor = Field;
			field.ForeColor = Ink;
			field.Font = Body;
			field.BorderStyle = BorderStyle.FixedSingle;
			field.Margin = new Padding(0, 2, 0, 6);
		}

		public static void StyleSelector(ComboBox selector)
		{
			selector.BackColor = Field;
			selector.ForeColor = Ink;
			selector.Font = Body;
			selector.FlatStyle = FlatStyle.Flat;
			selector.DropDownStyle = ComboBoxStyle.DropDownList;
			selector.ItemHeight = 22;
			selector.DrawMode = DrawMode.OwnerDrawFixed;
			selector.Margin = new Padding(0, 2, 0, 6);
			selector.DrawItem += DrawSelectorItem;
		}

		/// <summary>
		/// Turns on double buffering for the plain containers in a tree. Panels and layout panels
		/// paint straight to the screen by default, so a window of custom-drawn controls tears and
		/// flickers no matter how well each control buffers itself.
		/// </summary>
		public static void EnableDoubleBuffering(Control root)
		{
			if (root == null)
				return;
			var buffered = typeof(Control).GetProperty("DoubleBuffered", BindingFlags.Instance | BindingFlags.NonPublic);
			if (buffered == null)
				return;
			ApplyDoubleBuffering(root, buffered);
		}

		private static void ApplyDoubleBuffering(Control control, PropertyInfo buffered)
		{
			if (control is Panel || control is TableLayoutPanel || control is FlowLayoutPanel || control is TabPage || control is UserControl || control is Form)
				buffered.SetValue(control, true, null);
			foreach (Control child in control.Controls)
				ApplyDoubleBuffering(child, buffered);
		}

		public static GraphicsPath RoundedRectangle(Rectangle bounds, int radius)
		{
			int diameter = radius * 2;
			var path = new GraphicsPath();
			if (diameter <= 0 || bounds.Width <= diameter || bounds.Height <= diameter)
			{
				path.AddRectangle(bounds);
				return path;
			}
			path.AddArc(bounds.X, bounds.Y, diameter, diameter, 180, 90);
			path.AddArc(bounds.Right - diameter - 1, bounds.Y, diameter, diameter, 270, 90);
			path.AddArc(bounds.Right - diameter - 1, bounds.Bottom - diameter - 1, diameter, diameter, 0, 90);
			path.AddArc(bounds.X, bounds.Bottom - diameter - 1, diameter, diameter, 90, 90);
			path.CloseFigure();
			return path;
		}

		public static Color Shift(Color color, int amount)
		{
			return Color.FromArgb(color.A, Clamp(color.R + amount), Clamp(color.G + amount), Clamp(color.B + amount));
		}

		public static Color Blend(Color color, Color towards, double weight)
		{
			return Color.FromArgb(
				(int)(color.R + (towards.R - color.R) * weight),
				(int)(color.G + (towards.G - color.G) * weight),
				(int)(color.B + (towards.B - color.B) * weight));
		}

		private static int Clamp(int value)
		{
			return value < 0 ? 0 : value > 255 ? 255 : value;
		}

		private static void DrawSelectorItem(object sender, DrawItemEventArgs args)
		{
			var selector = (ComboBox)sender;
			bool highlighted = (args.State & DrawItemState.Selected) != 0 && (args.State & DrawItemState.ComboBoxEdit) == 0;
			using (var background = new SolidBrush(highlighted ? Blend(Field, Accent, 0.45) : Field))
				args.Graphics.FillRectangle(background, args.Bounds);
			if (args.Index >= 0)
			{
				var text = new Rectangle(args.Bounds.X + 6, args.Bounds.Y, args.Bounds.Width - 12, args.Bounds.Height);
				TextRenderer.DrawText(args.Graphics, selector.Items[args.Index].ToString(), selector.Font, text, Ink,
					TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
			}
		}
	}
}
