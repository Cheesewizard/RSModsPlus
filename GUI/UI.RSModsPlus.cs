using System;
using System.Drawing;
using System.Windows.Forms;
using RSMods.Audio;

namespace RSMods
{
	public partial class MainForm
	{
		private ListBox featureNavigation;
		private readonly FlowLayoutPanel[] featurePages = new FlowLayoutPanel[2];

		private void InitializeRsModsPlusPages()
		{
			tab_RSModsPlus.SuspendLayout();
			// The designer still places the old cable-input options and audio-status groups on this tab.
			// Both moved into the audio bridge (its Diagnostics page), so take them off the tab; left in
			// place they sat above the feature pages and drew over them.
			tab_RSModsPlus.Controls.Remove(groupBox_RSModsPlus_CableInput);
			tab_RSModsPlus.Controls.Remove(groupBox_RSModsPlus_AudioStatus);
			var navigation = new Panel { Dock = DockStyle.Left, Width = 208, BackColor = StudioTheme.Surface, Padding = new Padding(18, 24, 18, 12) };
			navigation.Controls.Add(new Label { Text = "RSMODSPLUS", ForeColor = StudioTheme.Muted, Dock = DockStyle.Top, Height = 32, Font = StudioTheme.SectionFont });
			featureNavigation = new ListBox
			{
				Location = new Point(12, 62), Size = new Size(184, 204), BorderStyle = BorderStyle.None,
				Font = new Font("Segoe UI", 10), DrawMode = DrawMode.OwnerDrawFixed, ItemHeight = 56,
				BackColor = StudioTheme.Surface, ForeColor = StudioTheme.Ink, IntegralHeight = false, AccessibleName = "RSModsPlus features"
			};
			featureNavigation.Items.AddRange(new object[] { "Note by Note", "Drop Pedal", "Audio bridge" });
			featureNavigation.DrawItem += DrawFeatureNavigation;
			featureNavigation.SelectedIndexChanged += SelectFeaturePage;
			navigation.Controls.Add(featureNavigation);
			var content = new Panel { Dock = DockStyle.Fill, BackColor = StudioTheme.Background };
			tab_RSModsPlus.BackColor = StudioTheme.Background;
			tab_RSModsPlus.ForeColor = StudioTheme.Ink;
			tab_RSModsPlus.Controls.Add(content);
			tab_RSModsPlus.Controls.Add(navigation);
			for (var index = 0; index < featurePages.Length; index++)
			{
				featurePages[index] = new FlowLayoutPanel
				{
					Name = "featurePage" + index, Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown,
					WrapContents = false, AutoScroll = true, Padding = new Padding(28, 24, 12, 18), BackColor = StudioTheme.Background, ForeColor = StudioTheme.Ink,
					Font = StudioTheme.Body, Visible = false
				};
				content.Controls.Add(featurePages[index]);
				featurePages[index].Controls.Add(new Label
				{
					Text = featureNavigation.Items[index].ToString(), AutoSize = true,
					Font = StudioTheme.Display, ForeColor = StudioTheme.Ink, Margin = new Padding(0, 0, 0, 16)
				});
			}
			InitializeNoteByNoteControls(featurePages[0]);
			InitializeMlServiceControls(featurePages[0]);
			InitializeDropPedalControls(featurePages[1]);
			StyleFeatureControls(tab_RSModsPlus);
			StudioTheme.EnableDoubleBuffering(tab_RSModsPlus);
			featureNavigation.SelectedIndex = 0;
			tab_RSModsPlus.ResumeLayout(true);
		}

		private static Label CreateFeatureDescription(string text)
		{
			return new Label { Text = text, Font = StudioTheme.Body, ForeColor = StudioTheme.Muted, AutoSize = true, MaximumSize = new Size(810, 0), Margin = new Padding(0, 0, 0, 16) };
		}

		private void SelectFeaturePage(object sender, EventArgs args)
		{
			if (featureNavigation.SelectedIndex == 2)
			{
				OpenAudioRouting(null, EventArgs.Empty);
				featureNavigation.SelectedIndex = 0;
				return;
			}
			for (var index = 0; index < featurePages.Length; index++)
			{
				featurePages[index].Visible = index == featureNavigation.SelectedIndex;
			}
		}

		private void DrawFeatureNavigation(object sender, DrawItemEventArgs args)
		{
			if (args.Index < 0) return;
			var selected = (args.State & DrawItemState.Selected) != 0;
			using (var background = new SolidBrush(StudioTheme.Surface)) args.Graphics.FillRectangle(background, args.Bounds);
			var bounds = new Rectangle(args.Bounds.X + 2, args.Bounds.Y + 3, args.Bounds.Width - 4, args.Bounds.Height - 6);
			args.Graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
			if (selected)
			{
				using (var path = StudioTheme.RoundedRectangle(bounds, 7))
				using (var fill = new SolidBrush(StudioTheme.Elevated)) args.Graphics.FillPath(fill, path);
				using (var accent = new SolidBrush(StudioTheme.Accent)) args.Graphics.FillRectangle(accent, bounds.X, bounds.Y + 12, 3, bounds.Height - 24);
			}
			var title = new Rectangle(bounds.X + 14, bounds.Y + 5, bounds.Width - 20, 22);
			TextRenderer.DrawText(args.Graphics, featureNavigation.Items[args.Index].ToString(), StudioTheme.Strong, title, StudioTheme.Ink,
				TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
			var descriptions = new[] { "Practice & detection", "Pitch & display", "Opens the live audio bridge" };
			TextRenderer.DrawText(args.Graphics, descriptions[args.Index], StudioTheme.Small,
				new Rectangle(title.X, bounds.Y + 27, title.Width, 18), StudioTheme.Muted, TextFormatFlags.Left | TextFormatFlags.NoPrefix);
			args.DrawFocusRectangle();
		}

		private static void StyleFeatureControls(Control parent)
		{
			foreach (Control child in parent.Controls)
			{
				if (child is GroupBox)
				{
					child.BackColor = StudioTheme.Surface;
					child.ForeColor = StudioTheme.Muted;
				}
				if (child is CheckBox)
				{
					child.ForeColor = StudioTheme.Ink;
					child.Font = StudioTheme.Body;
				}
				if (child is TextBox textBox) StudioTheme.StyleField(textBox);
				if (child is Button button && !(child is RSMods.Controls.ColourChip))
				{
					button.FlatStyle = FlatStyle.Flat;
					button.FlatAppearance.BorderColor = StudioTheme.Line;
					button.BackColor = StudioTheme.Field;
					button.ForeColor = StudioTheme.Ink;
					button.Font = StudioTheme.Body;
					button.Cursor = Cursors.Hand;
				}
				StyleFeatureControls(child);
			}
		}

	}
}
