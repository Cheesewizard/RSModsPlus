using System;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
using RSMods.Audio;
using RSMods.Util;

namespace RSMods
{
	// The game-closed audio setup, shown inline on the RSModsPlus tab's "Audio bridge" page so it lives in the
	// main RSMods window rather than only in the separate bridge window. Only settings that must be changed with
	// Rocksmith closed belong here (input mode, bridge power). The live controls (mixer, guitar input,
	// recording, output routing) are the in-game overlay, and the full bridge window still opens for the heavier
	// setup (recording folder, output device, driver install) via the button at the bottom.
	public partial class MainForm
	{
		[DllImport("kernel32", CharSet = CharSet.Unicode)]
		private static extern int GetPrivateProfileStringW(string section, string key, string def, StringBuilder ret, int size, string path);
		[DllImport("kernel32", CharSet = CharSet.Unicode)]
		private static extern bool WritePrivateProfileStringW(string section, string key, string val, string path);

		private CheckBox audioSetupPower;
		private SegmentedControl audioSetupInput;
		private Label audioSetupStatus;
		private bool syncingAudioSetup;

		// 0 = Real Tone Cable, 1 = ASIO interface. Kept in one place so the rocker and the read/write paths agree.
		private const int InputSegmentCable = 0;
		private const int InputSegmentAsio = 1;

		private static string AudioRoutingIniPath => Path.Combine(GenUtil.GetRSDirectory(), "AudioRouting.ini");

		private void InitializeAudioBridgeSetup(FlowLayoutPanel page)
		{
			page.Controls.Add(CreateFeatureDescription(
				"Set these with Rocksmith closed. The live controls - mixer, guitar input, recording and output - are in the in-game overlay: press \\ (backslash) while playing."));

			page.Controls.Add(CreateSectionLabel("Power", new Padding(0, 8, 0, 4)));
			audioSetupPower = new CheckBox { Text = "Audio bridge enabled", ForeColor = StudioTheme.Ink, Font = StudioTheme.Body, AutoSize = true, Margin = new Padding(0, 2, 0, 2) };
			audioSetupPower.CheckedChanged += (sender, args) => { if (!syncingAudioSetup) SetAudioBridgePower(audioSetupPower.Checked); };
			page.Controls.Add(audioSetupPower);
			page.Controls.Add(CreateFeatureDescription("Turns the audio bridge on or off. RS_ASIO.ini is left exactly as you set it, and Note by Note is unaffected."));

			page.Controls.Add(CreateSectionLabel("Guitar input", new Padding(0, 10, 0, 6)));
			audioSetupInput = new SegmentedControl("Real Tone Cable", "ASIO interface") { Margin = new Padding(0, 0, 0, 2) };
			audioSetupInput.SelectedIndexChanged += (sender, args) => { if (!syncingAudioSetup) SetAudioInputMode(audioSetupInput.SelectedIndex == InputSegmentAsio); };
			page.Controls.Add(audioSetupInput);
			page.Controls.Add(CreateFeatureDescription("Switches which input Rocksmith uses. Restart Rocksmith after changing it. RS_ASIO.ini is never modified - only the RS_ASIO files are enabled or disabled."));

			audioSetupStatus = new Label { Text = "", Font = StudioTheme.Small, ForeColor = StudioTheme.Muted, AutoSize = true, MaximumSize = new Size(810, 0), Margin = new Padding(0, 4, 0, 8) };
			page.Controls.Add(audioSetupStatus);

			var openBridge = new Button
			{
				Text = "Audio bridge app",
				Image = LoadAudioBridgeGlyph(),
				ImageAlign = ContentAlignment.MiddleLeft,
				TextAlign = ContentAlignment.MiddleLeft,
				TextImageRelation = TextImageRelation.ImageBeforeText,
				AutoSize = true,
				Padding = new Padding(10, 9, 16, 9),
				Margin = new Padding(0, 6, 0, 0)
			};
			openBridge.Click += OpenAudioRouting;
			page.Controls.Add(openBridge);
			page.Controls.Add(CreateFeatureDescription("Opens the bridge window for recording, the output device, and driver install."));

			page.VisibleChanged += (sender, args) => { if (page.Visible) RefreshAudioBridgeSetup(); };
			RefreshAudioBridgeSetup();
		}

		private static Label CreateSectionLabel(string text, Padding margin)
		{
			return new Label { Text = text.ToUpperInvariant(), Font = StudioTheme.SectionFont, ForeColor = StudioTheme.Faint, AutoSize = true, Margin = margin };
		}

		// The bridge's own 256px app icon, rendered at button size for the "Audio bridge app" button so the setup
		// page and the bridge window read as the same thing. Falls back to no image if the resource cannot be sized.
		private static Image LoadAudioBridgeGlyph()
		{
			try
			{
				int side = 20;
				using (var source = Properties.Resources.AudioBridgeIcon.ToBitmap())
				{
					var glyph = new Bitmap(side, side);
					using (var canvas = Graphics.FromImage(glyph))
					{
						canvas.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
						canvas.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
						canvas.DrawImage(source, new Rectangle(0, 0, side, side));
					}
					return glyph;
				}
			}
			catch
			{
				return null;
			}
		}

		// Re-read the on-disk state so the controls reflect what is actually set, and disable them while the game
		// is running (these are game-closed settings).
		private void RefreshAudioBridgeSetup()
		{
			if (audioSetupPower == null) return;
			syncingAudioSetup = true;
			try
			{
				var config = new AudioInputModeConfiguration(GenUtil.GetRSDirectory());
				bool gameRunning = config.IsGameRunning();

				var power = new StringBuilder(16);
				GetPrivateProfileStringW("Audio", "MasterEnabled", "1", power, power.Capacity, AudioRoutingIniPath);
				audioSetupPower.Checked = power.ToString() == "1";
				audioSetupPower.Enabled = !gameRunning;

				AudioInputMode mode;
				try { mode = config.ReadMode(); } catch { mode = AudioInputMode.Unavailable; }
				bool modeSettable = mode != AudioInputMode.Unavailable && !gameRunning;
				audioSetupInput.Enabled = modeSettable;
				audioSetupInput.SelectedIndex = mode == AudioInputMode.Asio ? InputSegmentAsio : InputSegmentCable;

				audioSetupStatus.Text = gameRunning
					? "Rocksmith is running. Close it to change these."
					: mode == AudioInputMode.Unavailable ? "RS_ASIO is not installed, so the input mode cannot be set here." : "";
			}
			finally { syncingAudioSetup = false; }
		}

		private void SetAudioBridgePower(bool enabled)
		{
			var config = new AudioInputModeConfiguration(GenUtil.GetRSDirectory());
			if (config.IsGameRunning())
			{
				MessageBox.Show(this, "Close Rocksmith before turning the audio bridge on or off.", "Audio bridge", MessageBoxButtons.OK, MessageBoxIcon.Warning);
				RefreshAudioBridgeSetup();
				return;
			}
			string path = AudioRoutingIniPath;
			if (enabled)
			{
				WritePrivateProfileStringW("Audio", "MasterEnabled", "1", path);
			}
			else
			{
				WritePrivateProfileStringW("Audio", "Enabled", "0", path);
				WritePrivateProfileStringW("Audio", "MasterEnabled", "0", path);
			}
			RefreshAudioBridgeSetup();
		}

		private void SetAudioInputMode(bool asio)
		{
			try
			{
				new AudioInputModeConfiguration(GenUtil.GetRSDirectory()).SetAsioEnabled(asio);
			}
			catch (Exception error)
			{
				MessageBox.Show(this, error.Message, "Input mode", MessageBoxButtons.OK, MessageBoxIcon.Warning);
			}
			RefreshAudioBridgeSetup();
		}
	}
}
