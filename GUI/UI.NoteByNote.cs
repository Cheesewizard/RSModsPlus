using System;
using System.Drawing;
using System.Windows.Forms;
using RSMods.Controls;

namespace RSMods
{
	public partial class MainForm
	{
		private CheckBox noteByNoteDetectionCheckBox;
		private NumericUpDown noteByNoteUiSize;
		private NumericUpDown noteByNoteTargetSize;
		private FeatureColourSettings noteByNoteColours;
		private FeatureColourSettings dropPedalColours;
		private bool loadingNoteByNoteControls;

		private void InitializeNoteByNoteControls(FlowLayoutPanel page)
		{
			noteByNoteColours = new FeatureColourSettings(ReadSettings.NOTE_BY_NOTE_CUSTOM_COLOURS_IDENTIFIER,
				new[] { ReadSettings.NOTE_BY_NOTE_NEUTRAL_COLOR_IDENTIFIER, ReadSettings.NOTE_BY_NOTE_CONFIRMED_COLOR_IDENTIFIER,
					ReadSettings.NOTE_BY_NOTE_PARTIAL_COLOR_IDENTIFIER, ReadSettings.NOTE_BY_NOTE_REJECTED_COLOR_IDENTIFIER },
				new[] { "Text / target", "Confirmed", "Partial", "Rejected" },
				new[] { "FFFFFF", "55DD77", "FFAA44", "FF5555" }, ReadSettings.ProcessSettings, SaveSettings_Save);
			page.Controls.Add(noteByNoteColours);
			noteByNoteDetectionCheckBox = new CheckBox
			{
				Text = "Show on-screen detection", AutoSize = true, Margin = new Padding(0, 6, 0, 8)
			};
			noteByNoteDetectionCheckBox.CheckedChanged += SaveNoteByNoteVisibility;
			page.Controls.Add(noteByNoteDetectionCheckBox);
			page.Controls.Add(CreateFeatureDescription("Shows Native, Enhanced, ML and Target while practising.\nHiding the readout keeps note detection active."));
			noteByNoteUiSize = CreateNoteByNoteSizeControl(page, "UI text size (%)", ReadSettings.NOTE_BY_NOTE_UI_SIZE_IDENTIFIER);
			noteByNoteTargetSize = CreateNoteByNoteSizeControl(page, "Target text size (%)", ReadSettings.NOTE_BY_NOTE_TARGET_SIZE_IDENTIFIER);
			page.Controls.Add(CreateFeatureDescription("UI size changes Native, Enhanced and ML. Target size is independent.\n100% is the original size. Changes apply while the game is running."));
			page.Controls.Add(CreateFeatureDescription("Enable Note by Note from the Riff Repeater menu in Rocksmith."));
		}

		private NumericUpDown CreateNoteByNoteSizeControl(FlowLayoutPanel page, string label, string identifier)
		{
			var row = new FlowLayoutPanel
			{
				AutoSize = true, WrapContents = false, Margin = new Padding(0, 4, 0, 4)
			};
			row.Controls.Add(new Label { Text = label, Width = 170, AutoSize = false, Margin = new Padding(0, 5, 8, 0) });
			var control = new NumericUpDown
			{
				Minimum = 50, Maximum = 300, Increment = 25, Value = 100, Width = 80,
				BackColor = Audio.StudioTheme.Field, ForeColor = Audio.StudioTheme.Ink, AccessibleName = label
			};
			control.ValueChanged += (sender, args) =>
			{
				if (loadingNoteByNoteControls) return;
				SaveSettings_Save(identifier, control.Value.ToString(System.Globalization.CultureInfo.InvariantCulture));
			};
			// Never step on the mouse wheel. Every ValueChanged writes the ini immediately and the
			// game applies it live, so a wheel scroll over the page while this box had focus walked
			// the value down 25% per notch and silently shrank the in-game readout (2026-09-15: the
			// UI size was found at 50%, the minimum, with nobody having chosen it). Marking the wheel
			// event handled makes NumericUpDown skip its own step; the arrows and typing still work.
			control.MouseWheel += (sender, args) =>
			{
				if (args is HandledMouseEventArgs wheel) wheel.Handled = true;
			};
			row.Controls.Add(control);
			page.Controls.Add(row);
			return control;
		}

		private static void LoadNoteByNoteSize(NumericUpDown control, string identifier)
		{
			var text = ReadSettings.ProcessSettings(identifier);
			if (!int.TryParse(text, out var size) || size < control.Minimum || size > control.Maximum)
				throw new FormatException(identifier + "must be a whole percentage between 50 and 300.");
			control.Value = size;
		}

		private void InitializeDropPedalControls(FlowLayoutPanel page)
		{
			dropPedalColours = new FeatureColourSettings(ReadSettings.DropPedalCustomOverlayColorsIdentifier,
				new[] { ReadSettings.DropPedalOverlayDownColorIdentifier, ReadSettings.DropPedalOverlayUpColorIdentifier, ReadSettings.DropPedalOverlayStatusColorIdentifier },
				new[] { "Pitch down", "Pitch up", "Status" }, new[] { "6BE06B", "FFC24D", "FFFFFF" }, ReadSettings.ProcessSettings, SaveSettings_Save);
			page.Controls.Add(dropPedalColours);
			checkBox_DropPedal.Margin = new Padding(0, 6, 0, 8);
			page.Controls.Add(checkBox_DropPedal);
			page.Controls.Add(CreateFeatureDescription("Shift your guitar's tuning without retuning the strings.\nThe pedal toggle cycles through Drop Pedal, Speaker Mode and Off."));
			var keybindings = new RSMods.Audio.StudioButton("Edit pedal shortcuts") { Margin = new Padding(0, 8, 0, 12) };
			keybindings.Click += (sender, args) => TabController.SelectedTab = tab_Keybindings;
			page.Controls.Add(keybindings);
		}

		private void LoadNoteByNoteControls()
		{
			loadingNoteByNoteControls = true;
			try
			{
				noteByNoteDetectionCheckBox.Checked = ReadSettings.ProcessSettings(ReadSettings.NOTE_BY_NOTE_DETECTION_OVERLAY_IDENTIFIER) == "on";
				LoadNoteByNoteSize(noteByNoteUiSize, ReadSettings.NOTE_BY_NOTE_UI_SIZE_IDENTIFIER);
				LoadNoteByNoteSize(noteByNoteTargetSize, ReadSettings.NOTE_BY_NOTE_TARGET_SIZE_IDENTIFIER);
				noteByNoteColours.LoadSettings();
				dropPedalColours.LoadSettings();
			}
			finally
			{
				loadingNoteByNoteControls = false;
			}
		}

		private void SaveNoteByNoteVisibility(object sender, EventArgs args)
		{
			if (loadingNoteByNoteControls) return;
			SaveSettings_Save(ReadSettings.NOTE_BY_NOTE_DETECTION_OVERLAY_IDENTIFIER, noteByNoteDetectionCheckBox.Checked ? "on" : "off");
		}
	}
}
