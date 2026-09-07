using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Threading;
using System.Windows.Forms;
using NAudio.CoreAudioApi;

namespace RSMods.Audio
{
	internal sealed class AudioRoutingPanel : UserControl
	{
		public bool IsBusy => isCommandRunning || (isGameRunning && latestStatus?.IsRecording == true) || video != null;
		private const long LowDiskBytes = 2L * 1024 * 1024 * 1024;
		private readonly string gameDirectory;
		private readonly string settingsPath;
		private readonly AudioInputModeConfiguration inputMode;
		private readonly ToolTip tips = new ToolTip { AutoPopDelay = 12000, InitialDelay = 400, ReshowDelay = 120 };
		private readonly StatusChip connectionChip = new StatusChip("Looking for Rocksmith");
		private readonly StatusChip recordingChip = new StatusChip("REC") { Visible = false };
		private readonly StatusChip inputChip = new StatusChip("Input");
		private readonly StatusChip captureChip = new StatusChip("Window capture") { Visible = false };
		private readonly StatusChip diskChip = new StatusChip("Disk");
		private readonly Label elapsedLabel = new Label { AutoSize = true, Text = "00:00.0", Font = StudioTheme.Timecode, ForeColor = StudioTheme.Ink, Margin = new Padding(0, 0, 12, 0) };
		private readonly Label deckHint = StudioTheme.Hint("");
		private readonly Label statusLabel = new Label { AutoEllipsis = true, UseMnemonic = false, Font = StudioTheme.Body, ForeColor = StudioTheme.Muted, TextAlign = ContentAlignment.MiddleLeft };
		private readonly Label inputModeLabel = StudioTheme.Hint("");
		private readonly Label spaceLabel = StudioTheme.Hint("");
		private readonly Label libraryLabel = StudioTheme.Hint("");
		private readonly LevelMeter meter = new LevelMeter { Dock = DockStyle.Fill };
		private readonly SegmentedControl captureMode = new SegmentedControl("Audio · WAV", "Video + audio · MP4");
		private readonly SegmentedControl inputSelector = new SegmentedControl("Cable", "ASIO");
		private readonly StudioSelector outputSelector = new StudioSelector { Dock = DockStyle.Fill };
		private readonly TextBox recordingDirectory = new TextBox { Dock = DockStyle.Fill };
		private readonly TakeList takes = new TakeList { Dock = DockStyle.Fill };
		private readonly StudioButton recordButton = new StudioButton("Record", StudioButtonKind.Record, true);
		private readonly StudioButton stopButton = new StudioButton("Stop & save");
		private readonly StudioButton applyButton = new StudioButton("Apply output", StudioButtonKind.Primary);
		private readonly StudioButton refreshButton = new StudioButton("Refresh devices");
		private readonly StudioButton browseButton = new StudioButton("Change folder");
		private readonly StudioButton openButton = new StudioButton("Open folder");
		private readonly StudioButton playButton = new StudioButton("Play take");
		private readonly StudioButton revealButton = new StudioButton("Show in folder");
		private readonly System.Windows.Forms.Timer statusTimer = new System.Windows.Forms.Timer { Interval = 500 };
		private readonly System.Windows.Forms.Timer mixerTimer = new System.Windows.Forms.Timer { Interval = 100 };
		/// <summary>Indexed by the bridge channel id, laid out on screen in console order.</summary>
		private readonly MixerStrip[] mixerFaders =
		{
			new MixerStrip(MixerChannel.Song),
			new MixerStrip(MixerChannel.PlayerOne),
			new MixerStrip(MixerChannel.Master, true),
			new MixerStrip(MixerChannel.PlayerTwo),
			new MixerStrip(MixerChannel.Microphone),
			new MixerStrip(MixerChannel.VoiceOver),
			new MixerStrip(MixerChannel.SoundEffects)
		};
		private readonly int?[] pendingVolumes = new int?[7];
		private readonly Label mixerHint = StudioTheme.Hint("Connect to Rocksmith to adjust playback volume.");
		private bool isApplyingMixer;
		private bool isShowingPlaybackStatus;
		private readonly Label currentBufferLabel = StudioTheme.Text("Current buffer: unavailable until Rocksmith connects.");
		private readonly SegmentedControl bufferMode = new SegmentedControl("Custom", "Auto finder") { SelectedIndex = 1 };
		private readonly NumericUpDown customBuffer = new NumericUpDown { Minimum = 1, Maximum = 48000, Width = 160, BackColor = StudioTheme.Field, ForeColor = StudioTheme.Ink, AccessibleName = "Custom output buffer in frames" };
		private readonly StudioButton applyBufferButton = new StudioButton("Apply custom buffer");
		private BufferTuningSnapshot bufferSnapshot;
		private readonly CheckBox longerBufferCheck = new CheckBox
		{
			Text = "Longer check (about 35 seconds per buffer)", AutoSize = true, ForeColor = StudioTheme.Ink,
			BackColor = StudioTheme.Surface, FlatStyle = FlatStyle.Flat, Font = StudioTheme.Body
		};
		private readonly StudioButton tuneButton = new StudioButton("Find stable buffer");
		private readonly StudioButton resetBufferButton = new StudioButton("Use device minimum");
		private readonly Label tuningLabel = StudioTheme.Hint("Test supported output periods while you play. This is not total input-to-speaker latency.");
		private CancellationTokenSource tuningCancellation;
		private AudioControlClient client;
		private AudioControlStatus latestStatus;
		private WindowCaptureRecorder video;
		private bool isCommandRunning;
		private bool isPolling;
		private bool isGameRunning;
		private bool syncingInputMode;
		private int ticks;
		private DateTime lastSignal = DateTime.UtcNow;
		private DateTime lastMixerEdit = DateTime.MinValue;

		public AudioRoutingPanel(string gameDirectory)
		{
			if (string.IsNullOrWhiteSpace(gameDirectory))
				throw new ArgumentException("Rocksmith folder is required.", nameof(gameDirectory));
			this.gameDirectory = gameDirectory;
			settingsPath = Path.Combine(gameDirectory, "AudioRouting.ini");
			inputMode = new AudioInputModeConfiguration(gameDirectory);
			BackColor = StudioTheme.Background;
			ForeColor = StudioTheme.Ink;
			Font = StudioTheme.Body;
			Padding = new Padding(20);
			AutoScroll = true;
			StudioTheme.StyleField(recordingDirectory);
			BuildLayout();
			StudioTheme.EnableDoubleBuffering(this);
			recordingDirectory.Text = ReadSetting("RecordingDirectory", Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "RSModsPlus"));
			WireEvents();
			RefreshDevices();
			takes.Load(recordingDirectory.Text, true);
			UpdateState();
			statusTimer.Start();
			for (int channel = 0; channel < mixerFaders.Length; channel++)
			{
				int selectedChannel = channel;
				mixerFaders[channel].VolumeChanged += (sender, args) =>
				{
					pendingVolumes[selectedChannel] = mixerFaders[selectedChannel].Value;
					lastMixerEdit = DateTime.UtcNow;
				};
			}
			mixerTimer.Tick += ApplyMixer;
			mixerTimer.Start();
			bufferMode.SelectedIndexChanged += (sender, args) => UpdateState();
			applyBufferButton.Click += ApplyCustomBuffer;
		}

		private void BuildLayout()
		{
			var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3, BackColor = Color.Transparent };
			root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
			root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
			root.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
			root.Controls.Add(BuildHeader(), 0, 0);
			var tabs = new StudioTabs { Dock = DockStyle.Fill };
			tabs.TabPages.Add(BuildFilledTab("Mixer", BuildMixerCard()));
			tabs.TabPages.Add(BuildTab("Recording", BuildDeck(), BuildTakesCard(), BuildFilesCard()));
			tabs.TabPages.Add(BuildTab("Setup", Columns(BuildRoutingCard(), BuildInputCard()), BuildBufferCard()));
			root.Controls.Add(tabs, 0, 1);
			root.Controls.Add(BuildStatusBar(), 0, 2);
			Controls.Add(root);
		}

		private static TabPage BuildFilledTab(string title, Control section)
		{
			var page = new TabPage(title) { BackColor = StudioTheme.Background, Padding = new Padding(12) };
			section.Dock = DockStyle.Fill;
			page.Controls.Add(section);
			return page;
		}

		private static TabPage BuildTab(string title, params Control[] sections)
		{
			var page = new TabPage(title) { BackColor = StudioTheme.Background, Padding = new Padding(12), AutoScroll = true };
			var body = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 1, BackColor = StudioTheme.Background };
			body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			foreach (var section in sections)
			{
				section.Dock = DockStyle.Fill;
				if (title == "Recording" && section.MinimumSize.Height == 0 && !section.AutoSize) section.MinimumSize = new Size(0, 240);
				body.RowStyles.Add(new RowStyle(SizeType.AutoSize));
				body.Controls.Add(section);
			}
			page.Controls.Add(body);
			return page;
		}

		private Control BuildHeader()
		{
			var header = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, BackColor = Color.Transparent, Margin = new Padding(0, 0, 0, 14) };
			header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			header.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			var titles = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0), BackColor = Color.Transparent };
			titles.Controls.Add(new Label { Text = "Audio bridge", Font = StudioTheme.Display, ForeColor = StudioTheme.Ink, AutoSize = true, Margin = new Padding(0) });
			titles.Controls.Add(new Label { Text = "Mixing, recording and a steadier audio setup.", Font = StudioTheme.Body, ForeColor = StudioTheme.Muted, AutoSize = true, Margin = new Padding(0, 2, 0, 0) });
			header.Controls.Add(titles, 0, 0);
			connectionChip.Anchor = AnchorStyles.Right;
			header.Controls.Add(connectionChip, 1, 0);
			return header;
		}

		private Control BuildDeck()
		{
			var deck = new StudioCard { Dock = DockStyle.Fill };
			var top = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, BackColor = Color.Transparent, Margin = new Padding(0) };
			top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			top.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			var clock = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0), BackColor = Color.Transparent };
			recordingChip.Margin = new Padding(0, 18, 0, 0);
			clock.Controls.Add(elapsedLabel);
			clock.Controls.Add(recordingChip);
			top.Controls.Add(clock, 0, 0);
			captureMode.Anchor = AnchorStyles.Right;
			captureMode.Margin = new Padding(0, 14, 0, 0);
			top.Controls.Add(captureMode, 1, 0);
			deck.Add(top);
			deck.Add(meter);
			var transport = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0, 6, 0, 4), BackColor = Color.Transparent };
			recordButton.MinimumTextWidth = 150;
			stopButton.MinimumTextWidth = 150;
			transport.Controls.Add(recordButton);
			transport.Controls.Add(stopButton);
			deck.Add(transport);
			var chips = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0, 4, 0, 0), BackColor = Color.Transparent };
			chips.Controls.Add(inputChip);
			chips.Controls.Add(captureChip);
			chips.Controls.Add(diskChip);
			deck.Add(chips);
			deckHint.MaximumSize = Size.Empty;
			deck.Add(deckHint);
			return deck;
		}

		private Control BuildMixerCard()
		{
			var card = new StudioCard("Game playback volume", true) { Dock = DockStyle.Fill };
			card.Add(StudioTheme.Hint("Balance game audio and both players. Levels follow Rocksmith while connected. Click a speaker to mute; double click a fader for 100%."));
			card.Add(BuildConsole(), true);
			card.Add(mixerHint);
			return card;
		}

		/// <summary>Master on its own, the two players side by side, then everything the game plays.</summary>
		private Control BuildConsole()
		{
			var banks = new Control[]
			{
				new MixerGroup("Master", Strip(MixerChannel.Master)),
				new MixerDivider(),
				new MixerGroup("Players", Strip(MixerChannel.PlayerOne), Strip(MixerChannel.PlayerTwo)),
				new MixerDivider(),
				new MixerGroup("Game audio", Strip(MixerChannel.Song), Strip(MixerChannel.SoundEffects), Strip(MixerChannel.VoiceOver), Strip(MixerChannel.Microphone))
			};
			var console = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = banks.Length + 2, RowCount = 1, Margin = new Padding(0, 14, 0, 10), BackColor = StudioTheme.Surface };
			console.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
			console.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
			for (int index = 0; index < banks.Length; index++)
			{
				console.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
				console.Controls.Add(banks[index], index + 1, 0);
			}
			console.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
			return console;
		}

		private MixerStrip Strip(MixerChannel channel)
		{
			return mixerFaders[(int)channel];
		}

		private Control BuildRoutingCard()
		{
			var card = new StudioCard("Playback device");
			card.Add(outputSelector);
			var buttons = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, BackColor = Color.Transparent };
			buttons.Controls.Add(applyButton);
			buttons.Controls.Add(refreshButton);
			card.Add(buttons);
			return card;
		}

		private Control BuildBufferCard()
		{
			var card = new StudioCard("Output buffer");
			card.Add(currentBufferLabel);
			card.Add(bufferMode);
			customBuffer.Margin = new Padding(0, 8, 12, 4);
			card.Add(Row(customBuffer, applyBufferButton));
			card.Add(Row(tuneButton, resetBufferButton));
			longerBufferCheck.Margin = new Padding(0, 6, 0, 2);
			card.Add(longerBufferCheck);
			card.Add(StudioTheme.Hint("Quick check: about 10 seconds per buffer. Keep Rocksmith focused and play normally."));
			card.Add(tuningLabel);
			card.Add(StudioTheme.Hint("Frames at 48 kHz: 480 frames = 10 ms. This is the output period, not total guitar latency."));
			return card;
		}

		private Control BuildInputCard()
		{
			var card = new StudioCard("Guitar input");
			card.Add(inputSelector);
			card.Add(inputModeLabel);
			return card;
		}
		private Control BuildFilesCard()
		{
			var card = new StudioCard("Takes folder") { Dock = DockStyle.Fill };
			spaceLabel.MaximumSize = Size.Empty;
			card.Add(FieldRow(recordingDirectory, browseButton, openButton));
			card.Add(spaceLabel);
			return card;
		}

		private static Control Columns(Control left, Control right)
		{
			var pair = new TableLayoutPanel { Dock = DockStyle.Top, ColumnCount = 2, RowCount = 1, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0), BackColor = Color.Transparent };
			pair.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
			pair.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
			left.Margin = new Padding(0, 0, 6, 12);
			right.Margin = new Padding(6, 0, 0, 12);
			left.Dock = DockStyle.Fill;
			right.Dock = DockStyle.Fill;
			pair.Controls.Add(left, 0, 0);
			pair.Controls.Add(right, 1, 0);
			return pair;
		}

		private static Control Row(params Control[] controls)
		{
			var row = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0), BackColor = Color.Transparent };
			foreach (Control control in controls)
				row.Controls.Add(control);
			return row;
		}

		private static Control FieldRow(Control field, params Control[] buttons)
		{
			var row = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0), BackColor = Color.Transparent };
			row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			field.Dock = DockStyle.Fill;
			field.Margin = new Padding(0, 13, 14, 0);
			row.Controls.Add(field, 0, 0);
			var group = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0), BackColor = Color.Transparent };
			foreach (Control button in buttons)
				group.Controls.Add(button);
			row.Controls.Add(group, 1, 0);
			return row;
		}

		private Control BuildTakesCard()
		{
			var card = new StudioCard("Recent takes", true) { Dock = DockStyle.Fill };
			card.Add(takes, true);
			var buttons = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0, 8, 0, 0), BackColor = Color.Transparent };
			buttons.Controls.Add(playButton);
			buttons.Controls.Add(revealButton);
			card.Add(buttons);
			card.Add(libraryLabel);
			return card;
		}

		private Control BuildStatusBar()
		{
			var bar = new Panel { Dock = DockStyle.Fill, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Padding = new Padding(2, 10, 2, 0), BackColor = Color.Transparent };
			bar.Paint += (sender, args) =>
			{
				using (var pen = new Pen(StudioTheme.Line))
					args.Graphics.DrawLine(pen, 0, 0, bar.Width, 0);
			};
			statusLabel.Dock = DockStyle.Fill;
			SetStatus("Ready when Rocksmith is.", ChipTone.Idle);
			bar.Controls.Add(statusLabel);
			return bar;
		}

		private void WireEvents()
		{
			recordButton.Click += Record;
			stopButton.Click += Stop;
			applyButton.Click += ApplyOutput;
			tuneButton.Click += TuneBuffer;
			resetBufferButton.Click += ResetBuffer;
			refreshButton.Click += (sender, args) => { RefreshDevices(); SetStatus("Playback devices refreshed.", ChipTone.Info); };
			browseButton.Click += ChooseFolder;
			openButton.Click += OpenFolder;
			playButton.Click += (sender, args) => Launch(takes.SelectedPath);
			revealButton.Click += (sender, args) => Reveal(takes.SelectedPath);
			takes.DoubleClick += (sender, args) => Launch(takes.SelectedPath);
			takes.SelectedIndexChanged += (sender, args) => UpdateTakeButtons();
			captureMode.SelectedIndexChanged += (sender, args) => UpdateState();
			outputSelector.SelectedIndexChanged += (sender, args) => UpdatePlaybackStatus();
			inputSelector.SelectedIndexChanged += (sender, args) => { if (!syncingInputMode) SwitchInputMode(inputSelector.SelectedIndex == 1); };
			recordingDirectory.Leave += (sender, args) => { takes.Load(recordingDirectory.Text, true); UpdateState(); };
			statusTimer.Tick += PollStatus;
			tips.SetToolTip(recordButton, "Start a take (F9)");
			tips.SetToolTip(stopButton, "Finish the take and write the file (F9)");
			tips.SetToolTip(applyButton, "Send the selected device to the running game, or arm it for the next launch");
			tips.SetToolTip(refreshButton, "Re-read the Windows playback devices (F5)");
			tips.SetToolTip(openButton, "Open the takes folder in Explorer");
			tips.SetToolTip(browseButton, "Pick the folder takes are written to");
			tips.SetToolTip(captureMode, "WAV captures game audio only. MP4 also records the Rocksmith window.");
			tips.SetToolTip(inputSelector, "ASIO uses RS_ASIO for low latency. Cable uses the Windows audio path.");
		}

		protected override bool ProcessCmdKey(ref Message message, Keys keyData)
		{
			if (keyData == Keys.F9)
			{
				if (recordButton.Enabled)
					Record(this, EventArgs.Empty);
				else if (stopButton.Enabled)
					Stop(this, EventArgs.Empty);
				return true;
			}
			if (keyData == Keys.F5)
			{
				RefreshDevices();
				return true;
			}
			return base.ProcessCmdKey(ref message, keyData);
		}

		private void RefreshDevices()
		{
			string selected = (outputSelector.SelectedItem as AudioDeviceChoice)?.Id ?? ReadSetting("OutputDevice", "");
			try
			{
				outputSelector.Items.Clear();
				using (var enumerator = new MMDeviceEnumerator())
				{
					if (selected.Length == 0)
					{
						using (var defaultOutput = enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Console)) selected = defaultOutput.ID;
					}
					foreach (MMDevice device in enumerator.EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active))
					{
						using (device)
						{
							int index = outputSelector.Items.Add(new AudioDeviceChoice(device.ID, device.FriendlyName));
							if (device.ID == selected)
								outputSelector.SelectedIndex = index;
						}
					}
				}
				if (outputSelector.SelectedIndex < 0 && selected.Length > 0)
					outputSelector.SelectedIndex = outputSelector.Items.Add(new AudioDeviceChoice(selected, "Unavailable device · " + selected));
			}
			catch (COMException error) { SetStatus(error.Message, ChipTone.Bad); }
		}

		private async void PollStatus(object sender, EventArgs args)
		{
			if (isPolling || isCommandRunning)
				return;
			isPolling = true;
			try
			{
				if (client == null)
				{
					foreach (var process in Process.GetProcessesByName("Rocksmith2014"))
					{
						using (process)
							if (string.Equals(Path.GetDirectoryName(process.MainModule.FileName), gameDirectory.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
							{
								client = new AudioControlClient(process.Id);
								break;
							}
					}
				}
				if (client == null)
					throw new IOException("Launch Rocksmith with shared audio enabled.");
				latestStatus = await client.SendAsync(1);
				if (bufferSnapshot == null || ticks % 4 == 0)
				{
					var snapshot = BufferTuningSnapshot.Parse(await client.SendAsync(5), false);
					if (bufferSnapshot == null || bufferSnapshot.Endpoint != snapshot.Endpoint)
					{
						customBuffer.Minimum = 1;
						customBuffer.Maximum = snapshot.Maximum;
						customBuffer.Minimum = snapshot.Minimum;
						customBuffer.Increment = snapshot.Fundamental;
						customBuffer.Value = snapshot.Period;
						bufferMode.SelectedIndex = ReadBufferSetting(snapshot.Endpoint, "Mode") == "Custom" ? 0 : 1;
					}
					bufferSnapshot = snapshot;
				}
				if (IsDisposed)
					return;
				bool broken = latestStatus.OutputError < 0;
				connectionChip.Set(broken ? "Output unavailable · pick another device" : "Connected to Rocksmith", broken ? ChipTone.Bad : ChipTone.Good);
				elapsedLabel.Text = StudioFormat.Timecode(TimeSpan.FromSeconds(latestStatus.Frames / 48000.0));
				meter.SetLevel(latestStatus.Peak);
				Strip(MixerChannel.Master).SetLevel(latestStatus.Peak);
				if (latestStatus.Peak > 0)
					lastSignal = DateTime.UtcNow;
				if (latestStatus.RecordingError < 0)
					SetStatus("Recording stopped with error 0x" + latestStatus.RecordingError.ToString("X8") + ". Press Stop & save to finalize the take.", ChipTone.Bad);
				else if (latestStatus.IsRecording && (DateTime.UtcNow - lastSignal).TotalSeconds > 4)
					SetStatus("No sound is reaching the output. Check the device and the in-game volume before you play on.", ChipTone.Warn);
			}
			catch (Exception error)
			{
				if (IsDisposed)
					return;
				client = null;
				latestStatus = null;
				bufferSnapshot = null;
				connectionChip.Set(Summarize(error), ChipTone.Idle);
				if (error is NotSupportedException) SetStatus(error.Message, ChipTone.Bad);
				meter.Reset();
				Strip(MixerChannel.Master).ResetLevel();
			}
			finally
			{
				isPolling = false;
				if (!IsDisposed)
				{
					if (++ticks % 4 == 0)
						takes.Load(recordingDirectory.Text);
					UpdateState();
				}
			}
		}

		private async void ApplyMixer(object sender, EventArgs args)
		{
			if (isCommandRunning || isPolling || client == null || latestStatus == null) return;
			bool hasPendingVolume = false;
			foreach (var volume in pendingVolumes) hasPendingVolume |= volume.HasValue;
			if (!hasPendingVolume) return;
			isCommandRunning = true;
			isApplyingMixer = true;
			var requestedVolumes = (int?[])pendingVolumes.Clone();
			Array.Clear(pendingVolumes, 0, pendingVolumes.Length);
			UpdateMixer();
			try
			{
				for (int channel = 0; channel < requestedVolumes.Length; channel++)
				{
					if (requestedVolumes[channel].HasValue)
					{
						latestStatus = await client.SendAsync((uint)(7 + channel), requestedVolumes[channel].Value.ToString(System.Globalization.CultureInfo.InvariantCulture));
					}
				}
			}
			catch (Exception error)
			{
				Array.Clear(pendingVolumes, 0, pendingVolumes.Length);
				latestStatus = null;
				if (!IsDisposed) SetStatus("Playback volume: " + error.Message, ChipTone.Bad);
			}
			finally
			{
				isApplyingMixer = false;
				isCommandRunning = false;
				if (!IsDisposed) UpdateMixer();
			}
		}

		private void UpdateMixer()
		{
			bool unavailable = false;
			for (int channel = 0; channel < mixerFaders.Length; channel++)
			{
				var fader = mixerFaders[channel];
				float volume = latestStatus == null ? -1 : latestStatus.Volumes[channel];
				if (float.IsNaN(volume) || float.IsInfinity(volume) || volume < 0 || volume > 100)
				{
					pendingVolumes[channel] = null;
					fader.SetUnavailable();
					unavailable = true;
					continue;
				}
				fader.Enabled = !isCommandRunning || isApplyingMixer;
				bool justEdited = (DateTime.UtcNow - lastMixerEdit).TotalMilliseconds < 750;
				if (!isApplyingMixer && !fader.IsAdjusting && !justEdited && !pendingVolumes[channel].HasValue) fader.SetVolume(volume);
			}
			mixerHint.Text = latestStatus == null ? "Connect to Rocksmith to adjust playback volume."
				: unavailable ? "Some playback channels are unavailable in Rocksmith."
				: "Playback volume only · guitar input and note detection stay unchanged.";
		}

		private static string Summarize(Exception error)
		{
			if (error is NotSupportedException) return "Audio bridge update required";
			return error is IOException || error is TimeoutException ? "Rocksmith not connected" : "Connection error";
		}

		private async void Record(object sender, EventArgs args)
		{
			if (client == null || isCommandRunning || !recordButton.Enabled)
				return;
			isCommandRunning = true;
			UpdateState();
			while (isPolling)
				await Task.Delay(15);
			try
			{
				if (!Path.IsPathRooted(recordingDirectory.Text))
					throw new ArgumentException("Choose a full recording folder path.");
				SavePreferences();
				if (captureMode.SelectedIndex == 1)
				{
					video = new WindowCaptureRecorder();
					await video.StartAsync(client.ProcessId, recordingDirectory.Text);
				}
				lastSignal = DateTime.UtcNow;
				latestStatus = await client.SendAsync(2, Path.GetFullPath(recordingDirectory.Text));
				SetStatus("Recording game audio" + (video != null ? " and video" : "") + " · press Stop & save to finish this take.", ChipTone.Info);
			}
			catch (Exception error) { video?.Dispose(); video = null; SetStatus(error.Message, ChipTone.Bad); }
			finally { isCommandRunning = false; UpdateState(); }
		}

		private async void Stop(object sender, EventArgs args)
		{
			if (isCommandRunning)
				return;
			isCommandRunning = true;
			UpdateState();
			while (isPolling)
				await Task.Delay(15);
			try
			{
				if (client == null)
					throw new IOException("The game disconnected. Reconnect to finalize game audio.");
				latestStatus = await client.SendAsync(3);
				SetStatus("Saving take…", ChipTone.Info);
				string file = video != null ? await video.StopAsync(latestStatus) : latestStatus.FilePath;
				SetStatus("Saved " + Path.GetFileName(file), ChipTone.Good);
				takes.Load(recordingDirectory.Text, true);
				takes.Select(file);
			}
			catch (Exception error) { SetStatus(error.Message, ChipTone.Bad); }
			finally { video?.Dispose(); video = null; isCommandRunning = false; UpdateState(); }
		}

		private async void ResetBuffer(object sender, EventArgs args)
		{
			if (isCommandRunning || !(outputSelector.SelectedItem is AudioDeviceChoice output)) return;
			isCommandRunning = true;
			UpdateState();
			try
			{
				while (isPolling) await Task.Delay(25);
				if (!WritePrivateProfileString("Output buffer " + output.Id, "PeriodFrames", null, settingsPath))
					throw new IOException("Could not clear the saved buffer setting.");
				if (!WritePrivateProfileString("Output buffer " + output.Id, "Mode", "Automatic", settingsPath))
					throw new IOException("Could not save the automatic buffer mode.");
				if (client != null && latestStatus?.EndpointId == output.Id)
					latestStatus = await client.SendAsync(4, output.Id);
				tuningLabel.Text = "Saved buffer cleared for " + output.Name + ". The device minimum is selected when this output opens.";
			}
			catch (Exception error) { tuningLabel.Text = error.Message; }
			finally { isCommandRunning = false; UpdateState(); }
		}

		private async void ApplyCustomBuffer(object sender, EventArgs args)
		{
			if (!applyBufferButton.Enabled || client == null || bufferSnapshot == null) return;
			var selectedBuffer = bufferSnapshot;
			uint frames = (uint)customBuffer.Value;
			if (frames < selectedBuffer.Minimum || frames > selectedBuffer.Maximum || frames % selectedBuffer.Fundamental != 0)
			{
				SetStatus("Choose a supported buffer in steps of " + selectedBuffer.Fundamental + " frames.", ChipTone.Bad);
				return;
			}
			isCommandRunning = true;
			UpdateState();
			try
			{
				while (isPolling) await Task.Delay(25);
				latestStatus = await client.SendAsync(6, frames.ToString(System.Globalization.CultureInfo.InvariantCulture) + "\n" + selectedBuffer.Endpoint);
				bufferSnapshot = BufferTuningSnapshot.Parse(await client.SendAsync(5), false);
				if (bufferSnapshot.Period != frames || bufferSnapshot.Endpoint != selectedBuffer.Endpoint)
					throw new IOException("Rocksmith did not confirm the requested output buffer.");
				if (!WritePrivateProfileString("Output buffer " + selectedBuffer.Endpoint, "PeriodFrames", frames.ToString(System.Globalization.CultureInfo.InvariantCulture), settingsPath))
					throw new IOException("The custom buffer is active, but could not be saved for the next launch.");
				if (!WritePrivateProfileString("Output buffer " + selectedBuffer.Endpoint, "Mode", "Custom", settingsPath))
					throw new IOException("The custom buffer is active, but its mode could not be saved.");
				SetStatus("Custom buffer applied and saved: " + frames + " frames (" + (frames / 48.0).ToString("0.##") + " ms).", ChipTone.Good);
			}
			catch (Exception error) { SetStatus(error.Message, ChipTone.Bad); }
			finally { isCommandRunning = false; UpdateState(); }
		}

		private async void TuneBuffer(object sender, EventArgs args)
		{
			if (tuningCancellation != null) { tuningCancellation.Cancel(); return; }
			if (client == null || isCommandRunning || latestStatus?.IsRecording == true || video != null) return;
			tuningCancellation = new CancellationTokenSource();
			isCommandRunning = true;
			tuneButton.Text = "Cancel buffer test";
			UpdateState();
			try
			{
				while (isPolling) await Task.Delay(25);
				var tuner = new AudioBufferTuner(client.SendAsync, () =>
				{
					GetWindowThreadProcessId(GetForegroundWindow(), out uint processId);
					return processId == client.ProcessId;
				}, message => tuningLabel.Text = message, (endpoint, period) =>
				{
					if (!WritePrivateProfileString("Output buffer " + endpoint, "PeriodFrames", period.ToString(System.Globalization.CultureInfo.InvariantCulture), settingsPath))
						throw new IOException("Could not save the tested output period.");
					if (!WritePrivateProfileString("Output buffer " + endpoint, "Mode", "Automatic", settingsPath))
						throw new IOException("Could not save the automatic buffer mode.");
				});
				uint selected = await tuner.RunAsync(tuningCancellation.Token, longerBufferCheck.Checked);
				tuningLabel.Text = "No empty-output events or repeated audio detected during this check. Saved: " + selected + " frames · " + (selected / 48.0).ToString("0.##") + " ms. This short check does not guarantee lasting stability.";
			}
			catch (OperationCanceledException) { tuningLabel.Text = "Test cancelled; previous output period restored."; }
			catch (Exception error) { tuningLabel.Text = error.Message; }
			finally
			{
				tuningCancellation.Dispose(); tuningCancellation = null;
				isCommandRunning = false; tuneButton.Text = "Find stable buffer";
				UpdateState();
			}
		}

		[DllImport("user32.dll")]
		private static extern IntPtr GetForegroundWindow();

		[DllImport("user32.dll")]
		private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

		private async void ApplyOutput(object sender, EventArgs args)
		{
			if (isCommandRunning || !(outputSelector.SelectedItem is AudioDeviceChoice output))
				return;
			isCommandRunning = true;
			UpdateState();
			while (isPolling)
				await Task.Delay(15);
			try
			{
				if (client != null)
				{
					latestStatus = await client.SendAsync(4, output.Id);
					if (latestStatus.OutputError < 0 || latestStatus.EndpointId != output.Id)
						throw new IOException("Rocksmith did not confirm the selected playback device.");
					WriteSetting("OutputDevice", latestStatus.EndpointId);
					isShowingPlaybackStatus = true;
					UpdatePlaybackStatus();
				}
				else
				{
					EnableBridge(output.Id);
					SetStatus("Armed for the next launch. Start Rocksmith and this window takes over playback and recording.", ChipTone.Good);
				}
			}
			catch (Exception error) { SetStatus(error.Message, ChipTone.Bad); }
			finally { isCommandRunning = false; UpdateState(); }
		}

		private void ChooseFolder(object sender, EventArgs args)
		{
			using (var dialog = new FolderBrowserDialog { Description = "Where should takes be saved?" })
			{
				if (Directory.Exists(recordingDirectory.Text))
					dialog.SelectedPath = recordingDirectory.Text;
				if (dialog.ShowDialog(this) != DialogResult.OK)
					return;
				recordingDirectory.Text = dialog.SelectedPath;
				takes.Load(recordingDirectory.Text, true);
				UpdateState();
			}
		}

		private void OpenFolder(object sender, EventArgs args)
		{
			try
			{
				Directory.CreateDirectory(recordingDirectory.Text);
				Process.Start("explorer.exe", "\"" + recordingDirectory.Text + "\"");
			}
			catch (Exception error) { SetStatus(error.Message, ChipTone.Bad); }
		}

		private void Launch(string path)
		{
			if (string.IsNullOrEmpty(path) || !File.Exists(path))
				return;
			try { Process.Start(new ProcessStartInfo(path) { UseShellExecute = true }); }
			catch (Exception error) { SetStatus(error.Message, ChipTone.Bad); }
		}

		private void Reveal(string path)
		{
			if (string.IsNullOrEmpty(path) || !File.Exists(path))
				return;
			try { Process.Start("explorer.exe", "/select,\"" + path + "\""); }
			catch (Exception error) { SetStatus(error.Message, ChipTone.Bad); }
		}

		private void UpdateState()
		{
			UpdateMixer();
			UpdatePlaybackStatus();
			bool recording = latestStatus?.IsRecording == true;
			bool custom = bufferMode.SelectedIndex == 0;
			customBuffer.Visible = applyBufferButton.Visible = custom;
			tuneButton.Visible = !custom;
			longerBufferCheck.Visible = !custom;
			longerBufferCheck.Enabled = !isCommandRunning;
			if (tuningCancellation == null) tuneButton.Text = bufferSnapshot != null && bufferSnapshot.Minimum == bufferSnapshot.Maximum
				? "Check audio stability" : "Find stable buffer";
			bufferMode.Enabled = !isCommandRunning;
			bool canSetBuffer = client != null && bufferSnapshot != null && !isCommandRunning && !recording && video == null
				&& (outputSelector.SelectedItem as AudioDeviceChoice)?.Id == bufferSnapshot.Endpoint;
			customBuffer.Enabled = applyBufferButton.Enabled = canSetBuffer;
			currentBufferLabel.Text = bufferSnapshot == null ? "Current buffer: unavailable until Rocksmith connects."
				: "Current: " + bufferSnapshot.Period + " frames · " + (bufferSnapshot.Period / 48.0).ToString("0.##") + " ms at 48 kHz · " + DescribeBufferMode() + "\n"
				+ (bufferSnapshot.Minimum == bufferSnapshot.Maximum ? "This device exposes only this output period. Auto finder can test its stability."
					: "Supported: " + bufferSnapshot.Minimum + "–" + bufferSnapshot.Maximum + " frames, in steps of " + bufferSnapshot.Fundamental + ".");
			bool wantsVideo = captureMode.SelectedIndex == 1;
			bool captureReady = WindowCaptureRecorder.Supported;
			string blocker = DescribeBlocker(recording, wantsVideo, captureReady);
			recordButton.Enabled = blocker == null && !isCommandRunning && !recording && video == null;
			stopButton.Enabled = !isCommandRunning && (recording || video != null || latestStatus?.RecordingError < 0);
			applyButton.Enabled = !isCommandRunning && outputSelector.SelectedItem is AudioDeviceChoice;
			tuneButton.Enabled = tuningCancellation != null || (client != null && !isCommandRunning && !recording && video == null);
			resetBufferButton.Enabled = !isCommandRunning && !recording && video == null && outputSelector.SelectedItem is AudioDeviceChoice;
			outputSelector.Enabled = !isCommandRunning;
			captureMode.Enabled = !recording && !isCommandRunning && video == null;
			recordingChip.Visible = recording || video != null;
			recordingChip.Tone = ChipTone.Bad;
			elapsedLabel.ForeColor = recording ? StudioTheme.Record : StudioTheme.Ink;
			deckHint.Text = recording
				? "Take running · writing " + (wantsVideo ? "audio and video" : "audio") + " to " + recordingDirectory.Text
				: blocker ?? "Ready. Press Record, or F9, to start a take.";
			deckHint.ForeColor = blocker == null || recording ? StudioTheme.Muted : StudioTheme.Warning;
			captureChip.Visible = wantsVideo;
			captureChip.Set(captureReady ? "Window capture ready" : "Window capture unavailable", captureReady ? ChipTone.Good : ChipTone.Bad);
			UpdateDisk();
			UpdateInputMode();
			UpdateTakeButtons();
			libraryLabel.Text = takes.Summary;
			tips.SetToolTip(recordingDirectory, recordingDirectory.Text);
		}

		private string DescribeBlocker(bool recording, bool wantsVideo, bool captureReady)
		{
			if (client == null)
				return "Rocksmith is not connected. Pick a playback device, press Apply output, then launch the game.";
			if (recording || video != null)
				return null;
			if (latestStatus?.RecordingError < 0)
				return "The last take ended with an error. Press Stop & save to close it before recording again.";
			if (!Path.IsPathRooted(recordingDirectory.Text))
				return "Choose a full folder path for takes before recording.";
			if (wantsVideo && !captureReady)
				return WindowCaptureRecorder.Requirement;
			return null;
		}

		private void UpdateDisk()
		{
			long free = StudioFormat.FreeSpace(recordingDirectory.Text);
			if (free < 0)
			{
				diskChip.Set("Folder unavailable", ChipTone.Warn);
				spaceLabel.Text = "That folder is not reachable right now.";
				return;
			}
			diskChip.Set(StudioFormat.Bytes(free) + " free", free < LowDiskBytes ? ChipTone.Warn : ChipTone.Idle);
			var audioTime = TimeSpan.FromSeconds(free / (double)StudioFormat.AudioBytesPerSecond);
			spaceLabel.Text = StudioFormat.Bytes(free) + " free · about " + StudioFormat.Length(audioTime) + " of 48 kHz 16-bit stereo audio"
				+ (captureMode.SelectedIndex == 1 ? " · video adds roughly 1 GB every 10 minutes" : "");
		}

		private void UpdateInputMode()
		{
			try
			{
				var mode = inputMode.ReadMode();
				isGameRunning = inputMode.IsGameRunning();
				syncingInputMode = true;
				inputSelector.SelectedIndex = mode == AudioInputMode.Asio ? 1 : 0;
				syncingInputMode = false;
				inputSelector.Enabled = !isGameRunning && !isCommandRunning && mode != AudioInputMode.Unavailable;
				inputChip.Set(mode == AudioInputMode.Asio ? "ASIO input" : mode == AudioInputMode.Cable ? "Cable input" : "RS_ASIO not installed",
					mode == AudioInputMode.Unavailable ? ChipTone.Warn : ChipTone.Info);
				inputModeLabel.Text = mode == AudioInputMode.Unavailable
					? "Install RS_ASIO in the Rocksmith folder to unlock ASIO."
					: isGameRunning ? "Close Rocksmith to change the input mode." : "Applies the next time Rocksmith launches.";
			}
			catch (Exception error)
			{
				syncingInputMode = false;
				inputSelector.Enabled = false;
				inputChip.Set("Input files conflict", ChipTone.Bad);
				inputModeLabel.Text = error.Message;
			}
		}

		private void UpdateTakeButtons()
		{
			bool selected = takes.SelectedPath != null;
			playButton.Enabled = selected;
			revealButton.Enabled = selected;
		}

		private void SwitchInputMode(bool enableAsio)
		{
			try
			{
				inputMode.SetAsioEnabled(enableAsio);
				SetStatus(enableAsio ? "ASIO enabled. Launch Rocksmith when ready." : "Cable mode enabled. Launch Rocksmith when ready.", ChipTone.Good);
			}
			catch (Exception error) { SetStatus(error.Message, ChipTone.Bad); }
			UpdateState();
		}

		private void UpdatePlaybackStatus()
		{
			if (!isShowingPlaybackStatus) return;
			string message;
			var tone = ChipTone.Good;
			if (latestStatus == null)
			{
				message = "Rocksmith disconnected · playback device unavailable.";
				tone = ChipTone.Idle;
			}
			else if (latestStatus.OutputError < 0 || string.IsNullOrEmpty(latestStatus.EndpointId))
			{
				message = "Playback output unavailable · choose another device.";
				tone = ChipTone.Bad;
			}
			else
			{
				string name = null;
				foreach (AudioDeviceChoice device in outputSelector.Items)
				{
					if (device.Id == latestStatus.EndpointId)
					{
						name = device.Name;
						break;
					}
				}
				message = name == null ? "Active playback device: " + latestStatus.EndpointId : "Now playing through " + name;
				var selected = outputSelector.SelectedItem as AudioDeviceChoice;
				if (selected != null && selected.Id != latestStatus.EndpointId)
				{
					message += " · Selected " + selected.Name + "; press Apply output to switch.";
				}
			}
			SetStatus(message, tone);
			isShowingPlaybackStatus = true;
		}

		private void SetStatus(string message, ChipTone tone)
		{
			isShowingPlaybackStatus = false;
			statusLabel.Text = message;
			tips.SetToolTip(statusLabel, message);
			statusLabel.ForeColor = tone == ChipTone.Bad ? StudioTheme.Record
				: tone == ChipTone.Warn ? StudioTheme.Warning
				: tone == ChipTone.Good ? StudioTheme.Positive
				: StudioTheme.Muted;
		}

		private void SavePreferences()
		{
			WriteSetting("RecordingDirectory", Path.GetFullPath(recordingDirectory.Text));
		}

		private void EnableBridge(string outputId)
		{
			var asio = File.Exists(Path.Combine(gameDirectory, "RS_ASIO.dll")) ? new AsioOutputConfiguration(gameDirectory) : null;
			string temporary = settingsPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
			try
			{
				if (File.Exists(settingsPath))
					File.Copy(settingsPath, temporary);
				else
					File.WriteAllText(temporary, "[Audio]\r\n", Encoding.Unicode);
				if (!WritePrivateProfileString("Audio", "OutputDevice", outputId, temporary)
					|| !WritePrivateProfileString("Audio", "Enabled", "1", temporary))
					throw new IOException("Could not prepare audio settings.");
				asio?.Apply(true, ReadSetting("Enabled", "0") == "1");
				if (File.Exists(settingsPath))
					File.Replace(temporary, settingsPath, null);
				else
					File.Move(temporary, settingsPath);
			}
			catch
			{
				asio?.UndoFailedSave();
				throw;
			}
			finally { if (File.Exists(temporary)) File.Delete(temporary); }
		}

		private string ReadSetting(string key, string defaultValue)
		{
			var value = new StringBuilder(2048);
			GetPrivateProfileString("Audio", key, defaultValue, value, value.Capacity, settingsPath);
			return value.ToString();
		}

		private string ReadBufferSetting(string endpoint, string key)
		{
			var value = new StringBuilder(128);
			GetPrivateProfileString("Output buffer " + endpoint, key, "", value, value.Capacity, settingsPath);
			return value.ToString();
		}

		private string DescribeBufferMode()
		{
			string saved = ReadBufferSetting(bufferSnapshot.Endpoint, "PeriodFrames");
			if (saved.Length == 0)
				return bufferSnapshot.Period == bufferSnapshot.Minimum ? "Automatic (device minimum)" : "Unsaved runtime setting";
			if (saved != bufferSnapshot.Period.ToString(System.Globalization.CultureInfo.InvariantCulture)) return "Runtime differs from saved buffer";
			string mode = ReadBufferSetting(bufferSnapshot.Endpoint, "Mode");
			return mode == "Custom" ? "Custom" : mode == "Automatic" ? "Automatic (finder result)" : "Saved (mode not recorded)";
		}

		private void WriteSetting(string key, string value)
		{
			if (value.IndexOfAny(new[] { '\r', '\n', '\0' }) >= 0)
				throw new ArgumentException("Invalid setting value.");
			if (!WritePrivateProfileString("Audio", key, value, settingsPath))
				throw new IOException("Could not save audio settings.");
		}

		protected override void Dispose(bool disposing)
		{
			if (disposing)
			{
				statusTimer.Stop();
				mixerTimer.Stop();
				mixerTimer.Dispose();
				statusTimer.Dispose();
				tips.Dispose();
				video?.Dispose();
			}
			base.Dispose(disposing);
		}

		[DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetPrivateProfileStringW")]
		private static extern uint GetPrivateProfileString(string section, string key, string defaultValue, StringBuilder value, int capacity, string path);

		[DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "WritePrivateProfileStringW", SetLastError = true)]
		private static extern bool WritePrivateProfileString(string section, string key, string value, string path);
	}
}
