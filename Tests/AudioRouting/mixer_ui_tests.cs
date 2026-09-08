using System;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Windows.Forms;

namespace RSMods.Tests
{
	internal static class MixerUiTests
	{
		private const BindingFlags PRIVATE_INSTANCE = BindingFlags.Instance | BindingFlags.NonPublic;

		[STAThread]
		private static int Main(string[] arguments)
		{
			try
			{
				Application.EnableVisualStyles();
				var assembly = Assembly.LoadFrom(Path.GetFullPath(arguments[0]));
				var windowType = assembly.GetType("RSMods.Audio.AudioRoutingWindow", true);
				Directory.CreateDirectory(arguments[1]);
				using (var window = (Form)Activator.CreateInstance(windowType, Path.GetFullPath(arguments[1])))
				{
					var panel = (Control)windowType.GetField("panel", PRIVATE_INSTANCE).GetValue(window);
					var panelType = panel.GetType();
					((Timer)panelType.GetField("statusTimer", PRIVATE_INSTANCE).GetValue(panel)).Stop();
					((Timer)panelType.GetField("mixerTimer", PRIVATE_INSTANCE).GetValue(panel)).Stop();
					var faders = (Array)panelType.GetField("mixerFaders", PRIVATE_INSTANCE).GetValue(panel);
					var pending = (int?[])panelType.GetField("pendingVolumes", PRIVATE_INSTANCE).GetValue(panel);
					var status = Activator.CreateInstance(assembly.GetType("RSMods.Audio.AudioControlStatus", true));
					var volumes = new float[] {80, 35, 90, 42, 25, 65, 55};
					status.GetType().GetProperty("Volumes").SetValue(status, volumes);
					foreach (Control fader in faders)
					{
						if (((Control)fader.GetType().GetField("slider", PRIVATE_INSTANCE).GetValue(fader)).Enabled) throw new Exception("Offline fader enabled.");
					}
					panelType.GetField("latestStatus", PRIVATE_INSTANCE).SetValue(panel, status);
					panelType.GetMethod("UpdateMixer", PRIVATE_INSTANCE).Invoke(panel, null);
					for (int channel = 0; channel < 7; channel++)
					{
						var fader = (Control)faders.GetValue(channel);
						if ((int)fader.GetType().GetProperty("Value").GetValue(fader) != volumes[channel] || pending[channel].HasValue) throw new Exception("Readback changed a channel or queued a write.");
						var slider = (Control)fader.GetType().GetField("slider", PRIVATE_INSTANCE).GetValue(fader);
						slider.GetType().GetMethod("OnKeyDown", PRIVATE_INSTANCE).Invoke(slider, new object[] {new KeyEventArgs(Keys.Down)});
						if (pending[channel] != (int)volumes[channel] - 1) throw new Exception("Wrong channel queued.");
						pending[channel] = null;
						fader.GetType().GetMethod("SetVolume").Invoke(fader, new object[] {volumes[channel]});
					}
					var song = (Control)faders.GetValue(0);
					var outputs = (ComboBox)panelType.GetField("outputSelector", PRIVATE_INSTANCE).GetValue(panel);
					outputs.Items.Clear();
					var deviceType = assembly.GetType("RSMods.Audio.AudioDeviceChoice", true);
					outputs.Items.Add(Activator.CreateInstance(deviceType, "speaker-a", "Speaker A"));
					outputs.Items.Add(Activator.CreateInstance(deviceType, "speaker-b", "Speaker B"));
					outputs.SelectedIndex = 0;
					status.GetType().GetProperty("EndpointId").SetValue(status, "speaker-a");
					panelType.GetField("isShowingPlaybackStatus", PRIVATE_INSTANCE).SetValue(panel, true);
					panelType.GetMethod("UpdatePlaybackStatus", PRIVATE_INSTANCE).Invoke(panel, null);
					var outputMessage = (Label)panelType.GetField("statusLabel", PRIVATE_INSTANCE).GetValue(panel);
					if (outputMessage.Text != "Now playing through Speaker A") throw new Exception("Active speaker identity incorrect.");
					outputs.SelectedIndex = 1;
					if (!outputMessage.Text.StartsWith("Now playing through Speaker A") || !outputMessage.Text.Contains("Selected Speaker B")) throw new Exception("Unapplied selection reported as active.");
					status.GetType().GetProperty("EndpointId").SetValue(status, "speaker-b");
					panelType.GetMethod("UpdatePlaybackStatus", PRIVATE_INSTANCE).Invoke(panel, null);
					if (outputMessage.Text != "Now playing through Speaker B") throw new Exception("Speaker change left stale output status.");
					status.GetType().GetProperty("OutputError").SetValue(status, -1);
					panelType.GetMethod("UpdatePlaybackStatus", PRIVATE_INSTANCE).Invoke(panel, null);
					if (outputMessage.Text.Contains("Now playing")) throw new Exception("Failed output still claims playback.");
					status.GetType().GetProperty("OutputError").SetValue(status, 0);
					panelType.GetField("latestStatus", PRIVATE_INSTANCE).SetValue(panel, null);
					panelType.GetMethod("UpdatePlaybackStatus", PRIVATE_INSTANCE).Invoke(panel, null);
					if (!outputMessage.Text.Contains("disconnected")) throw new Exception("Disconnect left stale speaker status.");
					panelType.GetField("latestStatus", PRIVATE_INSTANCE).SetValue(panel, status);
					var diagnostics = Activator.CreateInstance(status.GetType());
					diagnostics.GetType().GetProperty("EndpointId").SetValue(diagnostics, "fixture");
					diagnostics.GetType().GetProperty("FilePath").SetValue(diagnostics,
						"engineMinimum=480 engineMaximum=480 engineFundamental=480 enginePeriod=480 emptyOutputObservations=0 repeatedGameBlocks=0");
					var buffer = assembly.GetType("RSMods.Audio.BufferTuningSnapshot").GetMethod("Parse").Invoke(null, new[] { diagnostics, (object)false });
					panelType.GetField("bufferSnapshot", PRIVATE_INSTANCE).SetValue(panel, buffer);
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					var bufferLabel = (Label)panelType.GetField("currentBufferLabel", PRIVATE_INSTANCE).GetValue(panel);
					if (!bufferLabel.Text.Contains("480 frames") || !bufferLabel.Text.Contains("Automatic (device minimum)"))
						throw new Exception("Current buffer/mode readout is incorrect.");
					File.WriteAllText(Path.Combine(arguments[1], "AudioRouting.ini"), "[Output buffer fixture]\r\nPeriodFrames=480\r\nMode=Custom\r\n");
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					if (!bufferLabel.Text.Contains("Custom")) throw new Exception("Saved custom mode is missing.");
					using (var preview = new MixerPreviewWindow())
					{
						preview.Controls.Add(panel);
						preview.Show();
						preview.PerformLayout();
						panelType.GetField("latestStatus", PRIVATE_INSTANCE).SetValue(panel, null);
						panelType.GetMethod("UpdateMixer", PRIVATE_INSTANCE).Invoke(panel, null);
						foreach (Control fader in faders)
						{
							var offlineSlider = (Control)fader.GetType().GetField("slider", PRIVATE_INSTANCE).GetValue(fader);
							if (!offlineSlider.Visible || offlineSlider.Enabled) throw new Exception("Offline slider must remain visible and disabled.");
						}
						using (var offlineBitmap = new Bitmap(song.Parent.Width, song.Parent.Height))
						{
							song.Parent.DrawToBitmap(offlineBitmap, new Rectangle(Point.Empty, offlineBitmap.Size));
							offlineBitmap.Save(Path.Combine(arguments[1], "mixer-offline.png"));
						}
						panelType.GetField("latestStatus", PRIVATE_INSTANCE).SetValue(panel, status);
						panelType.GetMethod("UpdateMixer", PRIVATE_INSTANCE).Invoke(panel, null);
						using (var bitmap = new Bitmap(preview.Width, preview.Height))
						{
							preview.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
							bitmap.Save(Path.Combine(arguments[1], "mixer-window.png"));
						}
						using (var bitmap = new Bitmap(song.Parent.Width, song.Parent.Height))
						{
							song.Parent.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
							bitmap.Save(Path.Combine(arguments[1], "mixer-detail.png"));
						}
						var tabs = (TabControl)panel.Controls[0].Controls[1];
						for (int tabIndex = 0; tabIndex < tabs.TabCount; tabIndex++)
						{
							tabs.SelectedIndex = tabIndex;
							preview.PerformLayout();
							using (var tabBitmap = new Bitmap(preview.Width, preview.Height))
							{
								preview.DrawToBitmap(tabBitmap, new Rectangle(Point.Empty, tabBitmap.Size));
								tabBitmap.Save(Path.Combine(arguments[1], "tab-" + tabIndex + ".png"));
							}
						}
						window.Controls.Add(panel);
					}
					volumes[4] = -1;
					panelType.GetMethod("UpdateMixer", PRIVATE_INSTANCE).Invoke(panel, null);
					for (int channel = 0; channel < 7; channel++)
					{
						var fader = (Control)faders.GetValue(channel);
						var slider = (Control)fader.GetType().GetField("slider", PRIVATE_INSTANCE).GetValue(fader);
						if (slider.Enabled == (channel == 4)) throw new Exception("Unavailable channel affected another fader.");
					}
				}
				Console.WriteLine("PASS: offline/error states, independent readback, no writes from polling; window rendered off screen with fixture volumes.");
				return 0;
			}
			catch (Exception error)
			{
				Console.Error.WriteLine(error);
				return 1;
			}
		}
	}

	internal sealed class MixerPreviewWindow : Form
	{
		protected override bool ShowWithoutActivation => true;

		public MixerPreviewWindow()
		{
			Text = "Audio bridge · layout preview (fixture volumes)";
			StartPosition = FormStartPosition.Manual;
			Location = new Point(-30000, -30000);
			ClientSize = new Size(1060, 880);
			ShowInTaskbar = false;
		}
	}
}
