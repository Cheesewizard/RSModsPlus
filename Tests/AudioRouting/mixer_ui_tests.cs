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
				VerifyBridgeOwnedSettings(assembly);
				var windowType = assembly.GetType("RSMods.Audio.AudioRoutingWindow", true);
				Directory.CreateDirectory(arguments[1]);
				File.WriteAllText(Path.Combine(arguments[1], "cache.psarc"), "fixture");
				File.WriteAllText(Path.Combine(arguments[1], "RS_ASIO.dll.disabled"), "fixture");
				File.WriteAllText(Path.Combine(arguments[1], "avrt.dll.disabled"), "fixture");
				File.WriteAllText(Path.Combine(arguments[1], "RS_ASIO.ini"),
					"[Config]\r\nEnableAsio=0\r\nEnableWasapiInputs=1\r\n\r\n[Asio.Input.0]\r\nDriver=\r\n\r\n"
					+ "[Asio.Input.1]\r\nDriver=\r\n\r\n[Asio.Input.Mic]\r\nDriver=\r\n");
				File.WriteAllText(Path.Combine(arguments[1], "AudioRouting.ini"), "[Audio]\r\nMasterEnabled=1\r\n");
				File.WriteAllText(Path.Combine(arguments[1], "RSMods.ini"),
					"[Mod Settings]\r\nAudioDiagnosticsOverlay = off\r\n\r\n[Note by Note]\r\nNoteByNoteDetectionOverlay = off\r\n");
				assembly.GetType("RSMods.Data.Constants", true).GetProperty("RSFolder").SetValue(null, Path.GetFullPath(arguments[1]));
				using (var backgroundWindow = (Form)Activator.CreateInstance(windowType, BindingFlags.Instance | BindingFlags.NonPublic,
					null, new object[] { Path.GetFullPath(arguments[1]), System.Diagnostics.Process.GetCurrentProcess().Id, false }, null))
				{
					// The bridge auto-starts behind Rocksmith, so its first show must land it minimised -
					// but still create the taskbar button (a window whose very first show is already
					// minimised never gets one) and end up opaque so the taskbar icon is the real one.
					backgroundWindow.Show();
					Application.DoEvents();
					if (backgroundWindow.WindowState != FormWindowState.Minimized || !backgroundWindow.ShowInTaskbar)
						throw new Exception("Rocksmith-started Audio Bridge did not minimise on first show with a taskbar entry to restore it from.");
					if (backgroundWindow.Opacity != 1)
						throw new Exception("Rocksmith-started Audio Bridge stayed transparent after its start-minimised show.");
				}
				using (var window = (Form)Activator.CreateInstance(windowType, Path.GetFullPath(arguments[1])))
				{
					if (window.Text != "RSModsPlus 4 · Audio bridge") throw new Exception("Audio bridge title does not identify the matching RSModsPlus version.");
					var panel = (Control)windowType.GetField("panel", PRIVATE_INSTANCE).GetValue(window);
					var panelType = panel.GetType();
					var diagnosticsOverlay = panelType.GetField("diagnosticsOverlayCheck", PRIVATE_INSTANCE).GetValue(panel);
					var detectionOverlay = panelType.GetField("detectionOverlayCheck", PRIVATE_INSTANCE).GetValue(panel);
					var checkedProperty = diagnosticsOverlay.GetType().GetProperty("Checked");
					if ((bool)checkedProperty.GetValue(diagnosticsOverlay) || (bool)checkedProperty.GetValue(detectionOverlay))
						throw new Exception("Audio Bridge overlay toggles did not load their saved off state.");
					checkedProperty.SetValue(diagnosticsOverlay, true);
					checkedProperty.SetValue(detectionOverlay, true);
					string savedOverlaySettings = File.ReadAllText(Path.Combine(arguments[1], "RSMods.ini"));
					int modSection = savedOverlaySettings.IndexOf("[Mod Settings]", StringComparison.Ordinal);
					int diagnosticsSetting = savedOverlaySettings.IndexOf("AudioDiagnosticsOverlay = on", StringComparison.Ordinal);
					int noteSection = savedOverlaySettings.IndexOf("[Note by Note]", StringComparison.Ordinal);
					int detectionSetting = savedOverlaySettings.IndexOf("NoteByNoteDetectionOverlay = on", StringComparison.Ordinal);
					if (modSection < 0 || diagnosticsSetting < modSection || noteSection < diagnosticsSetting || detectionSetting < noteSection)
						throw new Exception("Audio Bridge overlay toggles were not persisted in their owning sections.");
					using (var reconstructedPanel = (Control)Activator.CreateInstance(panelType, Path.GetFullPath(arguments[1])))
					{
						var reconstructedType = reconstructedPanel.GetType();
						if (!(bool)checkedProperty.GetValue(reconstructedType.GetField("diagnosticsOverlayCheck", PRIVATE_INSTANCE).GetValue(reconstructedPanel))
							|| !(bool)checkedProperty.GetValue(reconstructedType.GetField("detectionOverlayCheck", PRIVATE_INSTANCE).GetValue(reconstructedPanel)))
							throw new Exception("Audio Bridge overlay toggles did not survive panel reconstruction.");
					}
					var recordingHotkeySelector = (ComboBox)panelType.GetField("recordingHotkeySelector", PRIVATE_INSTANCE).GetValue(panel);
					if ((Keys)recordingHotkeySelector.SelectedItem != Keys.F9)
						throw new Exception("Recording hotkey did not default to F9.");
					if (!recordingHotkeySelector.Items.Contains(Keys.F1) || !recordingHotkeySelector.Items.Contains(Keys.F24))
						throw new Exception("Recording hotkey selector does not expose the established safe-key catalogue.");
					recordingHotkeySelector.SelectedItem = Keys.F10;
					if (!File.ReadAllText(Path.Combine(arguments[1], "RSMods.ini")).Contains("RecordingHotkey = VK_F10"))
						throw new Exception("Changed recording hotkey was not persisted in RSMods.ini.");
					if (File.ReadAllText(Path.Combine(arguments[1], "AudioRouting.ini")).Contains("RecordingHotkey"))
						throw new Exception("Recording hotkey leaked into AudioRouting.ini.");
					var recordingHotkeyStatus = (Label)panelType.GetField("recordingHotkeyStatus", PRIVATE_INSTANCE).GetValue(panel);
						if (!recordingHotkeyStatus.Text.Contains("In-game hotkey: F10"))
						throw new Exception("Recording page does not explain the active in-game recording hotkey.");
					var hotkeyTips = (ToolTip)panelType.GetField("tips", PRIVATE_INSTANCE).GetValue(panel);
					var hotkeyRecordButton = (Control)panelType.GetField("recordButton", PRIVATE_INSTANCE).GetValue(panel);
					if (!hotkeyTips.GetToolTip(hotkeyRecordButton).Contains("F10"))
						throw new Exception("Record tooltip did not follow the rebound hotkey.");
					var masterPower = (Control)panelType.GetField("masterPower", PRIVATE_INSTANCE).GetValue(panel);
					var isOnProperty = masterPower.GetType().GetProperty("IsOn");
					if (masterPower.GetType().Name != "RockerToggle" || !(bool)isOnProperty.GetValue(masterPower) || masterPower.Width < 130)
						throw new Exception("Audio Bridge master power is not an On/Off rocker showing its saved state.");
					isOnProperty.SetValue(masterPower, false);
					var bridgePages = (Control[])panelType.GetField("pages", PRIVATE_INSTANCE).GetValue(panel);
					if ((bool)isOnProperty.GetValue(masterPower) || EnabledPages(bridgePages) != 0)
						throw new Exception("Audio Bridge off did not disable its controls; power=" + isOnProperty.GetValue(masterPower)
							+ ", enabledPages=" + EnabledPages(bridgePages)
							+ ", status=" + ((Label)panelType.GetField("statusLabel", PRIVATE_INSTANCE).GetValue(panel)).Text + ".");
					if (!File.ReadAllText(Path.Combine(arguments[1], "AudioRouting.ini")).Contains("MasterEnabled=0"))
						throw new Exception("Audio Bridge off was not persisted.");
					isOnProperty.SetValue(masterPower, true);
					if (!(bool)isOnProperty.GetValue(masterPower) || EnabledPages(bridgePages) != bridgePages.Length)
						throw new Exception("Audio Bridge on did not re-enable its controls.");
					File.Delete(Path.Combine(arguments[1], "AudioRouting.ini"));
					using (var defaultPanel = (Control)Activator.CreateInstance(panelType, Path.GetFullPath(arguments[1])))
					{
						var defaultPower = (Control)panelType.GetField("masterPower", PRIVATE_INSTANCE).GetValue(defaultPanel);
						if (!(bool)defaultPower.GetType().GetProperty("IsOn").GetValue(defaultPower))
							throw new Exception("A fresh Audio Bridge install did not default to On.");
					}

					File.WriteAllText(Path.Combine(arguments[1], "AudioRouting.ini"), "[Audio]\r\nMasterEnabled=0\r\n");
					using (var runningPanel = (Control)Activator.CreateInstance(panelType, BindingFlags.Instance | BindingFlags.NonPublic,
						null, new object[] { Path.GetFullPath(arguments[1]), new Func<bool>(() => true) }, null))
					{
						var runningPower = (Control)panelType.GetField("masterPower", PRIVATE_INSTANCE).GetValue(runningPanel);
						var runningIsOn = runningPower.GetType().GetProperty("IsOn");
						if (runningPower.Enabled)
							throw new Exception("Audio Bridge power rocker stayed enabled while Rocksmith was running; cached game state="
								+ panelType.GetField("isGameRunning", PRIVATE_INSTANCE).GetValue(runningPanel) + ".");
						var runningPowerHost = (Control)panelType.GetField("masterPowerHost", PRIVATE_INSTANCE).GetValue(runningPanel);
						var tips = (ToolTip)panelType.GetField("tips", PRIVATE_INSTANCE).GetValue(runningPanel);
						if (!tips.GetToolTip(runningPowerHost).Contains("Close Rocksmith"))
							throw new Exception("Disabled Audio Bridge power rocker has no close-Rocksmith hover help.");
						runningIsOn.SetValue(runningPower, true);
						if ((bool)runningIsOn.GetValue(runningPower)
							|| !File.ReadAllText(Path.Combine(arguments[1], "AudioRouting.ini")).Contains("MasterEnabled=0"))
							throw new Exception("Disabled Audio Bridge power rocker changed state while Rocksmith was running.");
						var runningStatus = (Label)panelType.GetField("statusLabel", PRIVATE_INSTANCE).GetValue(runningPanel);
						if (!runningStatus.Text.Contains("Close Rocksmith"))
							throw new Exception("Disabled Audio Bridge power rocker did not explain that Rocksmith must be closed: " + runningStatus.Text);
					}
					((Timer)panelType.GetField("statusTimer", PRIVATE_INSTANCE).GetValue(panel)).Stop();
					((Timer)panelType.GetField("mixerTimer", PRIVATE_INSTANCE).GetValue(panel)).Stop();
					var selector = (ComboBox)panelType.GetField("outputSelector", PRIVATE_INSTANCE).GetValue(panel);
					var choiceType = assembly.GetType("RSMods.Audio.AudioDeviceChoice", true);
					var choicesType = typeof(System.Collections.Generic.List<>).MakeGenericType(choiceType);
					var choices = (System.Collections.IList)Activator.CreateInstance(choicesType);
					choices.Add(Activator.CreateInstance(choiceType, "old-usb-port", "M-Track"));
					panelType.GetMethod("UpdateDevices", PRIVATE_INSTANCE).Invoke(panel, new object[] { choices, "old-usb-port" });
					if (selector.SelectedIndex != 0) throw new Exception("Available output was not selected.");
					var oldChoice = selector.SelectedItem;
					panelType.GetMethod("UpdateDevices", PRIVATE_INSTANCE).Invoke(panel, new object[] { choices, "old-usb-port" });
					if (!ReferenceEquals(oldChoice, selector.SelectedItem)) throw new Exception("Unchanged devices rebuilt the selection.");
					choices.Clear();
					choices.Add(Activator.CreateInstance(choiceType, "new-usb-port", "M-Track"));
					panelType.GetMethod("UpdateDevices", PRIVATE_INSTANCE).Invoke(panel, new object[] { choices, "old-usb-port" });
					if (selector.Items.Count != 1 || selector.SelectedIndex != -1) throw new Exception("USB replacement retained or selected the stale endpoint.");
					if (((Control)panelType.GetField("applyButton", PRIVATE_INSTANCE).GetValue(panel)).Enabled) throw new Exception("Disconnected output can still be applied.");
					choices.Clear();
					panelType.GetMethod("UpdateDevices", PRIVATE_INSTANCE).Invoke(panel, new object[] { choices, "new-usb-port" });
					if (selector.Items.Count != 0) throw new Exception("Unavailable devices remain in the list.");
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
					var performanceBolt = (Label)panelType.GetField("perfBolt", PRIVATE_INSTANCE).GetValue(panel);
					var getControlState = typeof(Control).GetMethod("GetState", BindingFlags.Instance | BindingFlags.NonPublic);
					panelType.GetField("proxyInstalled", PRIVATE_INSTANCE).SetValue(panel, true);
					status.GetType().GetProperty("EndpointId").SetValue(status, "(passthrough)");
					panelType.GetMethod("UpdatePlaybackStatus", PRIVATE_INSTANCE).Invoke(panel, null);
					if (!(bool)getControlState.Invoke(performanceBolt, new object[] { 2 }) || !outputMessage.Text.Contains("high-performance")) throw new Exception("Live ASIO passthrough did not show its performance state.");
					panelType.GetField("isSilentProxy", PRIVATE_INSTANCE).SetValue(panel, true);
					status.GetType().GetProperty("EndpointId").SetValue(status, "(silent)");
					panelType.GetMethod("UpdatePlaybackStatus", PRIVATE_INSTANCE).Invoke(panel, null);
					if ((bool)getControlState.Invoke(performanceBolt, new object[] { 2 }) || !outputMessage.Text.Contains("still running silently")) throw new Exception("Virtual silent output was presented as live ASIO.");
					panelType.GetField("isSilentProxy", PRIVATE_INSTANCE).SetValue(panel, false);
					panelType.GetField("isStartingProxy", PRIVATE_INSTANCE).SetValue(panel, true);
					status.GetType().GetProperty("EndpointId").SetValue(status, "(starting)");
					panelType.GetMethod("UpdatePlaybackStatus", PRIVATE_INSTANCE).Invoke(panel, null);
					if ((bool)getControlState.Invoke(performanceBolt, new object[] { 2 }) || !outputMessage.Text.Contains("remains available")) throw new Exception("Starting proxy was presented as active audio.");
					panelType.GetField("isStartingProxy", PRIVATE_INSTANCE).SetValue(panel, false);
					panelType.GetField("isDowngradingProxy", PRIVATE_INSTANCE).SetValue(panel, true);
					status.GetType().GetProperty("EndpointId").SetValue(status, "(downgrading)");
					panelType.GetMethod("UpdatePlaybackStatus", PRIVATE_INSTANCE).Invoke(panel, null);
					if ((bool)getControlState.Invoke(performanceBolt, new object[] { 2 }) || !outputMessage.Text.Contains("downgrading")) throw new Exception("ASIO loss did not show a temporary downgrade.");
					panelType.GetField("isDowngradingProxy", PRIVATE_INSTANCE).SetValue(panel, false);
					panelType.GetField("routeSourceStalled", PRIVATE_INSTANCE).SetValue(panel, true);
					panelType.GetField("isRouting", PRIVATE_INSTANCE).SetValue(panel, true);
					panelType.GetField("routedEndpointId", PRIVATE_INSTANCE).SetValue(panel, "speaker-a");
					status.GetType().GetProperty("EndpointId").SetValue(status, "(route-stalled)speaker-a");
					panelType.GetMethod("UpdatePlaybackStatus", PRIVATE_INSTANCE).Invoke(panel, null);
					if ((bool)getControlState.Invoke(performanceBolt, new object[] { 2 }) || !outputMessage.Text.Contains("recovering") || outputMessage.Text.Contains("Restart")) throw new Exception("Stalled ASIO source was presented as a terminal error.");
					status.GetType().GetProperty("EndpointId").SetValue(status, "(route-virtual)speaker-a");
					panelType.GetMethod("UpdateRouteState", PRIVATE_INSTANCE).Invoke(panel, new object[] { "(route-virtual)speaker-a" });
					panelType.GetMethod("UpdatePlaybackStatus", PRIVATE_INSTANCE).Invoke(panel, null);
					if (!(bool)panelType.GetField("isRouting", PRIVATE_INSTANCE).GetValue(panel)
						|| !(bool)panelType.GetField("isVirtualRoute", PRIVATE_INSTANCE).GetValue(panel)
						|| (bool)panelType.GetField("routeSourceStalled", PRIVATE_INSTANCE).GetValue(panel)
						|| (string)panelType.GetField("routedEndpointId", PRIVATE_INSTANCE).GetValue(panel) != "speaker-a")
						throw new Exception("Virtual route status did not retain route identity and promotion state.");
					panelType.GetField("routeSourceStalled", PRIVATE_INSTANCE).SetValue(panel, false);
					panelType.GetField("isRouting", PRIVATE_INSTANCE).SetValue(panel, false);
					var bridgeChip = (Control)panelType.GetField("bridgeChip", PRIVATE_INSTANCE).GetValue(panel);
					foreach (string stateField in new[] { "isSilentProxy", "isStartingProxy", "isDowngradingProxy" })
					{
						panelType.GetField(stateField, PRIVATE_INSTANCE).SetValue(panel, true);
						panelType.GetMethod("UpdateBridgeChip", PRIVATE_INSTANCE).Invoke(panel, new object[] { false });
						if (bridgeChip.Text == "Bridge off") throw new Exception(stateField + " was contradicted by a Bridge off chip.");
						panelType.GetField(stateField, PRIVATE_INSTANCE).SetValue(panel, false);
					}
					panelType.GetField("routeSourceStalled", PRIVATE_INSTANCE).SetValue(panel, true);
					panelType.GetMethod("UpdateBridgeChip", PRIVATE_INSTANCE).Invoke(panel, new object[] { false });
					if (bridgeChip.Text != "Bridge recovering") throw new Exception("Stalled route did not show bridge recovery.");
					panelType.GetField("routeSourceStalled", PRIVATE_INSTANCE).SetValue(panel, false);
					status.GetType().GetProperty("EndpointId").SetValue(status, "speaker-a");
					status.GetType().GetProperty("OutputError").SetValue(status, 0);
					panelType.GetMethod("UpdateBridgeChip", PRIVATE_INSTANCE).Invoke(panel, new object[] { false });
					if (bridgeChip.Text != "Bridge on") throw new Exception("Healthy permanent Windows output was presented as Bridge off.");
					panelType.GetField("proxyInstalled", PRIVATE_INSTANCE).SetValue(panel, false);
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					outputs.SelectedIndex = -1;
					status.GetType().GetProperty("OutputError").SetValue(status, -1);
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					panelType.GetField("isShowingPlaybackStatus", PRIVATE_INSTANCE).SetValue(panel, false);
					status.GetType().GetProperty("EndpointId").SetValue(status, "speaker-b");
					status.GetType().GetProperty("OutputError").SetValue(status, 0);
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					if (outputs.SelectedIndex != 1 || !outputMessage.Text.StartsWith("Now playing through Speaker B")) throw new Exception("USB recovery left blank selection or stale warning: selected=" + outputs.SelectedIndex + ", text=" + outputMessage.Text);
					outputs.SelectedIndex = 0;
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					if (outputs.SelectedIndex != 0 || !outputMessage.Text.Contains("Selected Speaker A")) throw new Exception("Polling overwrote unapplied selection.");
					outputs.SelectedIndex = -1;
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					if (outputs.SelectedIndex != 1) throw new Exception("Unchanged device list prevented recovery selection.");
					status.GetType().GetProperty("EndpointId").SetValue(status, "speaker-a");
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					if (!outputMessage.Text.StartsWith("Now playing through Speaker A")) throw new Exception("Active speaker identity incorrect.");
					outputs.SelectedIndex = 1;
					if (!outputMessage.Text.StartsWith("Now playing through Speaker A") || !outputMessage.Text.Contains("Selected Speaker B")) throw new Exception("Unapplied selection reported as active.");
					status.GetType().GetProperty("EndpointId").SetValue(status, "speaker-b");
					panelType.GetMethod("UpdatePlaybackStatus", PRIVATE_INSTANCE).Invoke(panel, null);
					if (!outputMessage.Text.StartsWith("Now playing through Speaker B")) throw new Exception("Speaker change left stale output status.");
					status.GetType().GetProperty("OutputError").SetValue(status, unchecked((int)0x8000000A));
					panelType.GetMethod("UpdatePlaybackStatus", PRIVATE_INSTANCE).Invoke(panel, null);
					if (!outputMessage.Text.Contains("Opening") || outputMessage.Text.Contains("Now playing")) throw new Exception("Pending output claims active playback.");
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
					var buffer = assembly.GetType("RSMods.Audio.AudioBufferSnapshot").GetMethod("Parse").Invoke(null, new[] { diagnostics });
					panelType.GetField("bufferSnapshot", PRIVATE_INSTANCE).SetValue(panel, buffer);
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					var bufferLabel = (Label)panelType.GetField("currentBufferLabel", PRIVATE_INSTANCE).GetValue(panel);
					var asioLabel = (Label)panelType.GetField("asioBufferLabel", PRIVATE_INSTANCE).GetValue(panel);
					var updateAsio = panelType.GetMethod("UpdateAsioBuffer", PRIVATE_INSTANCE);
					diagnostics.GetType().GetProperty("FilePath").SetValue(diagnostics, "asioInputFrames=128 asioInputRate=48000");
					updateAsio.Invoke(panel, new[] { diagnostics });
						if (!asioLabel.Text.Contains("128 frames") || !asioLabel.Text.Contains("2.67 ms") || !bufferLabel.Text.Contains("Windows playback buffer: 480 frames"))
						throw new Exception("ASIO and Windows buffers were not distinguished.");
					diagnostics.GetType().GetProperty("FilePath").SetValue(diagnostics, "asioInputFrames=128 asioInputRate=96000");
					updateAsio.Invoke(panel, new[] { diagnostics });
					if (!asioLabel.Text.Contains("1.33 ms")) throw new Exception("ASIO duration ignored negotiated sample rate.");
					diagnostics.GetType().GetProperty("FilePath").SetValue(diagnostics, "asioInputFrames=0 asioInputRate=0");
					updateAsio.Invoke(panel, new[] { diagnostics });
					if (!asioLabel.Text.Contains("no active")) throw new Exception("Inactive ASIO retained a buffer value.");
					diagnostics.GetType().GetProperty("FilePath").SetValue(diagnostics, "");
					updateAsio.Invoke(panel, new[] { diagnostics });
					if (!asioLabel.Text.Contains("updated")) throw new Exception("Missing ASIO telemetry was not reported.");
					diagnostics.GetType().GetProperty("FilePath").SetValue(diagnostics, "asioInputFrames=128 asioInputRate=48000");
					updateAsio.Invoke(panel, new[] { diagnostics });
					panelType.GetField("syncingInputMode", PRIVATE_INSTANCE).SetValue(panel, true);
					var inputSelector = panelType.GetField("inputSelector", PRIVATE_INSTANCE).GetValue(panel);
					inputSelector.GetType().GetProperty("SelectedIndex").SetValue(inputSelector, 1);
					panelType.GetField("syncingInputMode", PRIVATE_INSTANCE).SetValue(panel, false);
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					if (!bufferLabel.Text.Contains("480 frames") || !bufferLabel.Text.Contains("Automatic (device default)"))
						throw new Exception("Current buffer/mode readout is incorrect.");
					File.WriteAllText(Path.Combine(arguments[1], "AudioRouting.ini"), "[Output buffer fixture]\r\nPeriodFrames=480\r\nMode=Custom\r\n");
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					if (!bufferLabel.Text.Contains("Custom")) throw new Exception("Saved custom mode is missing.");
					File.Delete(Path.Combine(arguments[1], "AudioRouting.ini"));
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					var recordingSource = (Control)panelType.GetField("recordingSource", PRIVATE_INSTANCE).GetValue(panel);
					panelType.GetField("client", PRIVATE_INSTANCE).SetValue(panel,
						Activator.CreateInstance(assembly.GetType("RSMods.Audio.AudioControlClient", true), int.MaxValue));
					var selectedSource = recordingSource.GetType().GetProperty("SelectedIndex");
					selectedSource.SetValue(recordingSource, 1);
					var recordButton = (Control)panelType.GetField("recordButton", PRIVATE_INSTANCE).GetValue(panel);
					var deckHint = (Label)panelType.GetField("deckHint", PRIVATE_INSTANCE).GetValue(panel);
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					if (recordButton.Enabled || !deckHint.Text.Contains("active Player 1")) throw new Exception("Unavailable dry input did not block recording.");
					status.GetType().GetProperty("IsDryInputReady").SetValue(status, true);
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					if (!recordButton.Enabled) throw new Exception("Ready dry input could not record.");
					status.GetType().GetProperty("IsRecording").SetValue(status, true);
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
					if (recordingSource.Enabled) throw new Exception("Audio source could change during a take.");
					status.GetType().GetProperty("IsRecording").SetValue(status, false);
					panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
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
						var pageControls = (Control[])panelType.GetField("pages", PRIVATE_INSTANCE).GetValue(panel);
						var nav = (Control)panelType.GetField("nav", PRIVATE_INSTANCE).GetValue(panel);
						var navIndex = nav.GetType().GetProperty("SelectedIndex");
						updateAsio.Invoke(panel, new[] { diagnostics });
						panelType.GetMethod("UpdateState", PRIVATE_INSTANCE).Invoke(panel, null);
						var takes = (ListView)panelType.GetField("takes", PRIVATE_INSTANCE).GetValue(panel);
						takes.Items.Clear();
						for (int takeIndex = 0; takeIndex < 8; takeIndex++)
						{
							takes.Items.Add(new ListViewItem(new[] { "Rocksmith-20260908-170321-8492-" + takeIndex + ".wav", "00:17", "3.2 MB", "8 Sep  17:03" }));
						}
						takes.Items[0].Selected = true;
						((Label)panelType.GetField("libraryLabel", PRIVATE_INSTANCE).GetValue(panel)).Text = "8 sample takes · 25.6 MB";
						for (int tabIndex = 0; tabIndex < pageControls.Length; tabIndex++)
						{
							navIndex.SetValue(nav, tabIndex);
							preview.PerformLayout();
							using (var tabBitmap = new Bitmap(preview.Width, preview.Height))
							{
								preview.DrawToBitmap(tabBitmap, new Rectangle(Point.Empty, tabBitmap.Size));
								tabBitmap.Save(Path.Combine(arguments[1], "tab-" + tabIndex + ".png"));
							}
						}
						navIndex.SetValue(nav, 2);
						foreach (var previewSize in new[] { new Size(940, 620), new Size(1060, 880), new Size(1400, 1000) })
						{
							preview.ClientSize = previewSize;
							preview.PerformLayout();
							// Comfortable sizes must show four complete recent takes; the small size only has to
							// keep the first row whole (the list scrolls - the redesign reserves the height for
							// transport and take options).
							if (previewSize.Height >= 880
								? takes.GetItemRect(3).Bottom > takes.ClientSize.Height
								: takes.GetItemRect(0).Bottom > takes.ClientSize.Height)
								throw new Exception("Recent takes clipped at " + previewSize
									+ "; row4bottom=" + takes.GetItemRect(3).Bottom + ", clientHeight=" + takes.ClientSize.Height);
							using (var recordingBitmap = new Bitmap(preview.Width, preview.Height))
							{
								preview.DrawToBitmap(recordingBitmap, new Rectangle(Point.Empty, recordingBitmap.Size));
								recordingBitmap.Save(Path.Combine(arguments[1], "recording-" + previewSize.Width + ".png"));
							}
						}
						preview.Scale(new SizeF(1.5f, 1.5f));
						preview.ClientSize = new Size(1500, 960);
						preview.PerformLayout();
						if (takes.GetItemRect(3).Bottom > takes.ClientSize.Height)
							throw new Exception("Recent takes cannot display four complete rows at 150% scaling.");
						using (var scaledBitmap = new Bitmap(preview.Width, preview.Height))
						{
							preview.DrawToBitmap(scaledBitmap, new Rectangle(Point.Empty, scaledBitmap.Size));
							scaledBitmap.Save(Path.Combine(arguments[1], "recording-scaled.png"));
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

		private static void VerifyBridgeOwnedSettings(Assembly assembly)
		{
			var readSettingsType = assembly.GetType("RSMods.ReadSettings", true);
			var bridgeOwnedIdentifiers = (string[])readSettingsType.GetField("BridgeOwnedIdentifiers").GetValue(null);
			var requiredIdentifierFields = new[]
			{
				"AsioInputGainIdentifier",
				"NoiseGateThresholdIdentifier",
				"CompressorStrengthIdentifier",
				"HumFilterIdentifier",
				"RocksmithGateOverrideIdentifier",
				"RocksmithGateThresholdIdentifier"
			};

			foreach (var fieldName in requiredIdentifierFields)
			{
				var identifier = (string)readSettingsType.GetField(fieldName).GetValue(null);
				if (Array.IndexOf(bridgeOwnedIdentifiers, identifier) < 0)
					throw new Exception(fieldName + " is written by Audio Bridge but is not refreshed before the main settings file is saved.");
			}
		}

		private static int EnabledPages(Control[] pages)
		{
			int enabled = 0;
			foreach (Control page in pages)
				if (page != null && page.Enabled) enabled++;
			return enabled;
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
