using System;
using System.Drawing;
using System.Windows.Forms;
using RSMods.Audio;

namespace RSMods
{
	public partial class MainForm
	{
		// The tab is one page: the game-closed audio setup (bridge power, input mode, ASIO bridge driver). Note by
		// Note and Drop Pedal settings moved into the in-game overlay on 2026-09-23, so the old three-page sidebar
		// (Note by Note / Drop Pedal / Audio bridge) was removed, along with UI.NoteByNote.cs and UI.MlService.cs.
		private void InitializeRsModsPlusPages()
		{
			tab_RSModsPlus.SuspendLayout();
			// The designer still places the old cable-input options and audio-status groups on this tab; both moved
			// into the overlay, so take them off. Left in place they drew over the page.
			tab_RSModsPlus.Controls.Remove(groupBox_RSModsPlus_CableInput);
			tab_RSModsPlus.Controls.Remove(groupBox_RSModsPlus_AudioStatus);
			tab_RSModsPlus.BackColor = OverlayLook.Window;
			tab_RSModsPlus.ForeColor = OverlayLook.Text;
			var page = new FlowLayoutPanel
			{
				Name = "audioBridgePage", Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown,
				WrapContents = false, AutoScroll = true, BackColor = OverlayLook.Window, ForeColor = OverlayLook.Text, Font = StudioTheme.Body
			};
			page.Padding = new Padding(page.LogicalToDeviceUnits(28), page.LogicalToDeviceUnits(22), page.LogicalToDeviceUnits(12), page.LogicalToDeviceUnits(18));
			// Fill first, then Top: WinForms docks in reverse z-order, so the header must be added last.
			tab_RSModsPlus.Controls.Add(page);
			tab_RSModsPlus.Controls.Add(new BrandHeader { Dock = DockStyle.Top });
			InitializeAudioBridgeSetup(page);
			StyleFeatureControls(tab_RSModsPlus);
			StudioTheme.EnableDoubleBuffering(tab_RSModsPlus);
			tab_RSModsPlus.ResumeLayout(true);
		}

		private static Label CreateFeatureDescription(string text)
		{
			return new Label { Text = text, Font = StudioTheme.Body, ForeColor = StudioTheme.Muted, AutoSize = true, MaximumSize = new Size(810, 0), Margin = new Padding(0, 0, 0, 16) };
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
