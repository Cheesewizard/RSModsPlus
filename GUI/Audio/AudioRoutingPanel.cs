using System;
using System.Diagnostics;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Threading;
using System.Windows.Forms;
using NAudio.CoreAudioApi;
using NAudio.CoreAudioApi.Interfaces;

namespace RSMods.Audio
{
	internal sealed class AudioRoutingPanel : UserControl, IMMNotificationClient
	{
		public bool IsBusy => isCommandRunning || (isGameRunning && latestStatus?.IsRecording == true) || video != null;
		private const long LowDiskBytes = 2L * 1024 * 1024 * 1024;
		/// <summary>Sentinel the DLL reports as the active endpoint while the control pipe is hosted but no output is routed (AudioRouting.ini Enabled=0). Kept in sync with SharedOutput.cpp PassthroughStatus.</summary>
		private const string PassthroughEndpoint = "(passthrough)";
		private const string SilentEndpoint = "(silent)";
		private const string StartingEndpoint = "(starting)";
		private const string DowngradingEndpoint = "(downgrading)";
		// While an alternate-device route is live the host reports "(route)<deviceId>" as the endpoint, so
		// the GUI can name the device and tell a route from plain passthrough. See SharedOutput RouteEndpointTag.
		private const string RoutePrefix = "(route)";
		private const string RouteVirtualPrefix = "(route-virtual)";
		// The host reports "(route-stalled)<deviceId>" when a route is live but its ASIO source has stopped
		// (the interface was unplugged): the WASAPI backend stays open with nothing to play. We flag that so the
		// footer says "no audio, restart" instead of a healthy route. See SharedOutput RouteFeedStalled.
		private const string RouteStalledPrefix = "(route-stalled)";
		/// <summary>Poll rate while the bridge answers. Fast enough that the meters read as live; the pipe exchange is a few kilobytes.</summary>
		private const int ConnectedPollMilliseconds = 100;
		/// <summary>Poll rate while looking for the game, when each tick also enumerates processes.</summary>
		private const int SearchingPollMilliseconds = 500;
		/// <summary>How often the slower housekeeping (buffer status, takes folder) rides along with a poll.</summary>
		private const int HousekeepingMilliseconds = 2000;
		private const string LIMITER_HINT = "Limits game output to the configured ceiling. The active audio bridge uses a look-ahead brickwall limiter to stop transients above the ceiling before they reach the output. Adds a few milliseconds of output latency and prevents clipping. Does not control the interface volume knob.";
		private static readonly Keys[] recordingHotkeys = (Keys[])KeyConversion.KeyDownDictionary.Clone();

		internal Keys RecordingHotkey => (Keys)recordingHotkeySelector.SelectedItem;

		private readonly string gameDirectory;
		private readonly string settingsPath;
		private readonly AudioInputModeConfiguration inputMode;
		private readonly ToolTip tips = new ToolTip { AutoPopDelay = 12000, InitialDelay = 400, ReshowDelay = 120 };
		// Last text handed to the ToolTip per control. SetToolTip re-registers the tool every call, and the
		// status poll used to call it several times per 100 ms tick with unchanged text.
		private readonly Dictionary<Control, string> tipTexts = new Dictionary<Control, string>();
		private readonly StatusChip connectionChip = new StatusChip("Looking for Rocksmith");
		private readonly StatusChip bridgeChip = new StatusChip("Bridge off") { Visible = false };
		private readonly StatusChip recordingChip = new StatusChip("REC") { Visible = false };
		private readonly StatusChip inputChip = new StatusChip("Input");
		private readonly StatusChip captureChip = new StatusChip("Window capture") { Visible = false };
		private readonly RockerToggle masterPower = new RockerToggle();
		private readonly Panel masterPowerHost = new Panel { Margin = new Padding(0) };
		private readonly StatusChip diskChip = new StatusChip("Disk");
		// Fixed size so a tick of the clock never re-lays out the deck; wide enough for h:mm:ss.f.
		private readonly Label elapsedLabel = new Label { AutoSize = false, Width = 196, Height = 44, Text = "00:00.0", Font = StudioTheme.Timecode, ForeColor = StudioTheme.Ink, TextAlign = ContentAlignment.MiddleLeft, Margin = new Padding(0, 0, 12, 0), UseMnemonic = false };
		private readonly Label deckHint = StudioTheme.Hint("");
		private readonly Label recordingHotkeyStatus = StudioTheme.Hint("");
		private readonly Label statusLabel = new Label { AutoEllipsis = true, UseMnemonic = false, Font = StudioTheme.Body, ForeColor = StudioTheme.Muted, TextAlign = ContentAlignment.MiddleLeft };
		// A lightning bolt in the header, top-right by the connection chip: it appears (amber) only while the game
		// is playing straight out the low-latency ASIO device, so "lit" = high-performance output is live.
		private readonly Label perfBolt = new Label { Text = "⚡", AutoSize = true, UseMnemonic = false, Font = new Font("Segoe UI", 15F, FontStyle.Bold), ForeColor = StudioTheme.Warning, Margin = new Padding(0, 4, 10, 0), Visible = false };
		private readonly Label inputModeLabel = StudioTheme.Hint("");
		private readonly Label spaceLabel = StudioTheme.Hint("");
		private readonly Label libraryLabel = StudioTheme.Hint("");
		private readonly LevelMeter meter = new LevelMeter { Dock = DockStyle.Fill };
		private readonly SegmentedControl captureMode = new SegmentedControl("Audio (WAV)", "Video (MP4)");
		private readonly SegmentedControl recordingSource = new SegmentedControl("Game mix", "Dry guitar");
		private readonly StudioSelector recordingHotkeySelector = new StudioSelector { Width = 130, DropDownStyle = ComboBoxStyle.DropDownList, AccessibleName = "In-game recording hotkey" };
		private bool syncingRecordingHotkey;
		private readonly SegmentedControl inputSelector = new SegmentedControl("Real Tone Cable", "ASIO interface");
		private readonly StudioCheck eventDrivenCaptureCheck = new StudioCheck("Read the cable with Windows audio events") { Margin = new Padding(0, 10, 0, 0) };
		private readonly Label eventDrivenCaptureHint = StudioTheme.Hint("Event-driven capture of the Real Tone Cable. Tone is unchanged; RS_ASIO stays off in cable mode. Restart Rocksmith after changing it.");
		private readonly StudioCheck cableForPlayerTwoCheck = new StudioCheck("Use the Real Tone Cable for Player 2") { Margin = new Padding(0, 10, 0, 0) };
		private readonly Label cableForPlayerTwoHint = StudioTheme.Hint("Adds the cable as a second input after the ASIO interface for multiplayer. RS_ASIO.ini is not changed. Applies live while Rocksmith is connected.");
		private bool syncingEventDrivenCapture;
		private bool syncingCableForPlayerTwo;
		// Guitar input make-up gain. A Real Tone Cable carries a hot built-in preamp; an interface
		// through RS_ASIO arrives much quieter, so the game's level-sensitive note gate mutes sustains
		// and bends early. This lifts the input to cable level. The slider and the DLL/RSMods.ini all
		// work in tenths of a dB (0..200 = 0..+20 dB).
		private readonly StudioSlider inputGainSlider = new StudioSlider { Minimum = 0, Maximum = 200, SmallChange = 1, LargeChange = 10, AccessibleName = "Guitar input make-up gain, tenths of a decibel" };
		// Typeable dB box paired with the slider (both in 0.1 dB): type an exact value or nudge by 0.1; clamped 0..+20.
		private readonly StudioNumber gainInput = new StudioNumber { DecimalPlaces = 1, Increment = 0.1M, Minimum = 0.0M, Maximum = 20.0M, Value = 0.0M };
		private readonly StudioCheck inputGainEnableCheck = new StudioCheck("Input gain");
		private bool syncingInputGain;
		private int? pendingInputGainTenths;
		private int persistedInputGainTenths;
		// The game DLL applies the saved make-up gain and noise gate once at its own launch, but the bridge
		// process may have come up at a different value (e.g. the ini was changed while it kept running). On a
		// fresh connection, push the persisted values once so the sliders match what is actually live, rather
		// than showing a setting that only takes effect when the user nudges the control.
		private bool pushInputSettingsOnConnect;
		// Guitar input noise gate (soft downward expander in the DLL). The slider counts whole-dB
		// threshold steps; the rightmost notch (-40) means Off. RSMods.ini and the DLL store tenths of a
		// dB (0 = off, else a negative threshold).
		// Noise gate: an on/off, a fine (0.1 dB) slider and a typeable box that share one threshold in tenths of a
		// dBFS. Off collapses to just the checkbox. The slider works in tenths so each step is 0.1 dB (1 dB was far
		// too coarse to sit on a noise floor); the numeric box lets you type an exact value and clamps silly input.
		private readonly StudioCheck gateEnableCheck = new StudioCheck("Noise gate");
		private readonly StudioSlider noiseGateSlider = new StudioSlider { Minimum = -800, Maximum = -200, SmallChange = 1, LargeChange = 10, AccessibleName = "Guitar input noise gate threshold, tenths of a decibel" };
		private readonly StudioNumber gateThresholdInput = new StudioNumber { DecimalPlaces = 1, Increment = 0.1M, Minimum = -80.0M, Maximum = -20.0M, Value = -50.0M };
		private bool syncingNoiseGate;
		private int? pendingNoiseGateTenths;
		private int persistedNoiseGateTenths;
		// Rocksmith gate (DLL forces the game's own P1_NoiseFloor RTPC). Its own on/off + threshold, kept
		// separate from the pre-signal noise gate above; the two combine to give native / pre / both.
		private readonly StudioCheck rgEnableCheck = new StudioCheck("Rocksmith gate");
		private readonly StudioSlider rgGateSlider = new StudioSlider { Minimum = -1000, Maximum = 100, SmallChange = 1, LargeChange = 10, AccessibleName = "Rocksmith gate threshold (P1_NoiseFloor), tenths of a decibel" };
		private readonly StudioNumber rgGateInput = new StudioNumber { DecimalPlaces = 1, Increment = 0.1M, Minimum = -100.0M, Maximum = 10.0M, Value = -59.3M };
		private bool syncingRgGate;
		private (bool on, int tenths)? pendingRocksmithGate;
		private bool persistedRgOverride;
		private int persistedRgTenths;
		// Mains-hum notch (DLL, front of chain). One value in RSMods.ini: 0 = off, else the base frequency in
		// Hz (typically 50 or 60, but any 20..120 the DLL honors). Removes the ground-loop hum comb a grounded
		// interface injects and a cable does not. Slider and box share the value in whole Hz.
		private readonly StudioCheck humFilterCheck = new StudioCheck("Hum filter");
		private readonly StudioSlider humFilterSlider = new StudioSlider { Minimum = 20, Maximum = 120, SmallChange = 1, LargeChange = 10, AccessibleName = "Hum filter base frequency in hertz" };
		private readonly StudioNumber humFilterInput = new StudioNumber { DecimalPlaces = 0, Increment = 1, Minimum = 20, Maximum = 120, Value = 50 };
		private bool syncingHumFilter;
		private int? pendingHumFilter;
		private int persistedHumFilter;
		// Guitar input compressor (DLL). One "strength" macro 0..100 (0 = Off) flattens the natural
		// string-beat wobble before the game amp, so a quiet interface input doesn't warble the way a hot
		// cable's amp-compressed signal doesn't. RSMods.ini and the DLL store the 0-100 value directly.
		private readonly StudioSlider compressorSlider = new StudioSlider { Minimum = 0, Maximum = 100, SmallChange = 1, LargeChange = 10, AccessibleName = "Guitar input compressor strength" };
		// Typeable strength box paired with the slider (0-100%, 0 = off); type an exact value or nudge by 1. Clamped.
		private readonly StudioNumber compInput = new StudioNumber { DecimalPlaces = 0, Increment = 1M, Minimum = 0M, Maximum = 100M, Value = 0M };
		private readonly StudioCheck compressorEnableCheck = new StudioCheck("Compressor");
		private bool syncingCompressor;
		private int? pendingCompressor;
		private int persistedCompressor;
		// Output limiter (proxy driver). Command "<limiterOn>,<ceiling>,<agcOn>,<target>" over op 16 (agcOn stays 0:
		// the AGC stage is retired from the UI, so only the look-ahead brickwall limiter that holds the ceiling runs).
		private readonly StudioCheck limiterCheck = new StudioCheck("Output limiter");
		// Ceiling. Slider and box both work in 0.1 dB from -24 dBFS to 0; RSMods.ini and the DLL store tenths of
		// a dBFS, and the proxy takes it as a linear ceiling (0 dBFS = full scale).
		private readonly StudioSlider limiterLevelSlider = new StudioSlider { Minimum = -240, Maximum = 0, SmallChange = 1, LargeChange = 10, AccessibleName = "Output limiter ceiling, tenths of a decibel" };
		// Typeable dB box paired with the ceiling slider (both 0.1 dB): type an exact value or nudge by 0.1. Clamped.
		private readonly StudioNumber ceilingInput = new StudioNumber { DecimalPlaces = 1, Increment = 0.1M, Minimum = -24.0M, Maximum = 0.0M, Value = -6.0M };
		private string pendingLimiter;
		private bool syncingLimiter;
		private readonly StudioSelector outputSelector = new StudioSelector { Dock = DockStyle.Fill };
		// One hint under the output selector: which transport the chosen device resolves to (high-performance
		// ASIO when the device has a driver, else the WASAPI bridge) and whether game-mix recording is ready.
		private readonly Label asioProxyLabel = StudioTheme.Hint("");
		private readonly TextBox recordingDirectory = new TextBox { Dock = DockStyle.Fill };
		private readonly TakeList takes = new TakeList { Dock = DockStyle.Fill };
		private readonly StudioButton recordButton = new StudioButton("Record", StudioButtonKind.Record, true);
		private readonly StudioButton stopButton = new StudioButton("Stop & save");
		private readonly StudioButton applyButton = new StudioButton("Apply output", StudioButtonKind.Primary);
		// Only shown when the proxy driver is actually registered (detected), so there is always a way to back out.
		private readonly StudioButton removeProxyButton = new StudioButton("Remove audio bridge driver") { Visible = false };
		private readonly StudioButton browseButton = new StudioButton("Change folder");
		private readonly StudioButton openButton = new StudioButton("Open folder");
		private readonly StudioButton playButton = new StudioButton("Play take");
		private readonly StudioButton revealButton = new StudioButton("Show in folder");
		private readonly System.Windows.Forms.Timer statusTimer = new System.Windows.Forms.Timer { Interval = SearchingPollMilliseconds };
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
		private readonly Label mixerHint = StudioTheme.Hint("Start Rocksmith to adjust playback volumes.");
		private bool isApplyingMixer;
		private bool isShowingPlaybackStatus;
		private bool hasPlaybackStatus;
		private string displayedPlaybackEndpoint;
		private int displayedPlaybackError;
		private readonly Label currentBufferLabel = StudioTheme.Text("Windows playback buffer: available once Rocksmith connects.");
		private readonly Label asioBufferLabel = StudioTheme.Text("ASIO input buffer: available once Rocksmith connects.");
		private readonly StudioCheck customBufferCheck = new StudioCheck("Use a custom playback buffer");
		private readonly StudioNumber customBuffer = new StudioNumber { Minimum = 1, Maximum = 48000, Width = 120, TextAlign = HorizontalAlignment.Left, AccessibleName = "Custom output buffer in frames", Margin = new Padding(0, 6, 12, 4) };
		private readonly StudioButton applyBufferButton = new StudioButton("Apply buffer");
		private readonly StudioButton measureLatencyButton = new StudioButton("Measure round-trip latency", StudioButtonKind.Primary);
		private readonly Label latencyReadout = StudioTheme.Text("Round-trip latency: not measured.");
		private readonly StudioCheck diagnosticsOverlayCheck = new StudioCheck("Show the audio diagnostics overlay in game");
		private readonly StudioCheck monitorOutputCheck = new StudioCheck("Log the game's output stream for diagnosis");
		private readonly StudioButton refreshDiagnosticsButton = new StudioButton("Refresh status");
		private readonly TextBox diagnosticsStatus = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, WordWrap = false, Dock = DockStyle.Fill };
		// Environment facts that cost real time to read (process list, RS_ASIO.ini, the registry, free disk
		// space). They used to be read on every 100 ms status poll, on the UI thread, which is most of why the
		// window felt slow. Now they are refreshed on the housekeeping tick, when the connection changes and
		// after any action that can change them, and every per-tick reader uses the cached copy.
		private AudioInputMode cachedMode = AudioInputMode.Unavailable;
		private string cachedModeError;
		private bool cachedProxyRegistered;
		private List<string> cachedAsioDrivers = new List<string>();
		private long cachedFreeSpace = -1;
		private AudioBufferSnapshot bufferSnapshot;
		private AudioControlClient client;
		private AudioControlStatus latestStatus;
		private WindowCaptureRecorder video;
		private bool isCommandRunning;
		private bool isPolling;
		private bool isGameRunning;
		private bool proxyInstalled;   // cached so the poll tick can hide the remove button without a registry read
		private bool isPassthrough;
		private bool isSilentProxy;
		private bool isStartingProxy;
		private bool isDowngradingProxy;
		private bool isRouting;                 // an alternate-device route is live (proxy world, no restart)
		private bool isVirtualRoute;            // the alternate route is fed by the proxy's virtual clock
		private string routedEndpointId = "";   // the device the route is currently rendering to
		private bool routeSourceStalled;        // route is live but its ASIO source has stalled (no audio)
		private bool displayedRouteStalled;     // last stall state pushed to the footer, for change detection
		private bool syncingInputMode;
		private bool syncingMasterPower;
		private bool masterEnabled;
		private bool masterOffRequiresClose;
		// Navigation column + the pages it shows. Pages are built once and swapped by visibility.
		private StudioNav nav;
		private readonly Control[] pages = new Control[5];
		private int ticks;
		private MMDeviceEnumerator deviceEnumerator;
		private int devicesChanged;
		private int enumeratingDevices;
		private DateTime lastSignal = DateTime.UtcNow;
		private DateTime lastMixerEdit = DateTime.MinValue;

		public AudioRoutingPanel(string gameDirectory) : this(gameDirectory, null)
		{
		}

		internal AudioRoutingPanel(string gameDirectory, Func<bool> isGameRunning)
		{
			if (string.IsNullOrWhiteSpace(gameDirectory))
				throw new ArgumentException("Rocksmith folder is required.", nameof(gameDirectory));
			this.gameDirectory = gameDirectory;
			settingsPath = Path.Combine(gameDirectory, "AudioRouting.ini");
			inputMode = new AudioInputModeConfiguration(gameDirectory, isGameRunning);
			this.isGameRunning = inputMode.IsGameRunning();
			BackColor = StudioTheme.Background;
			ForeColor = StudioTheme.Ink;
			Font = StudioTheme.Body;
			Padding = new Padding(20);
			AutoScroll = true;
			StudioTheme.StyleField(recordingDirectory);
			StudioTheme.StyleTips(tips);
			// Collapse the many intermediate layout passes the nested table/flow panels would
			// otherwise trigger as each child is added into a single pass.
			SuspendLayout();
			BuildLayout();
			ResumeLayout(true);
			syncingMasterPower = true;
			masterEnabled = ReadSetting("MasterEnabled", "1") == "1";
			masterPower.IsOn = masterEnabled;
			syncingMasterPower = false;
			if (!masterEnabled)
				ReconcileDisabledBridge();
			else
				ReconcileEnabledBridge();   // ini-only repair; RS_ASIO reads it at the next launch, so a running game is fine
			StudioTheme.EnableDoubleBuffering(this);
			recordingDirectory.Text = ReadSetting("RecordingDirectory", Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "RSModsPlus"));
			recordingSource.SelectedIndex = ReadSetting("RecordingSource", "Tone") == "Dry" ? 1 : 0;
			foreach (Keys key in recordingHotkeys)
				recordingHotkeySelector.Items.Add(key);
			Keys savedRecordingHotkey;
			if (!Enum.TryParse(RSMods.ReadSettings.ProcessSettings(RSMods.ReadSettings.RecordingHotkeyIdentifier), out savedRecordingHotkey)
				|| Array.IndexOf(recordingHotkeys, savedRecordingHotkey) < 0)
				savedRecordingHotkey = Keys.F9;
			syncingRecordingHotkey = true;
			recordingHotkeySelector.SelectedItem = savedRecordingHotkey;
			syncingRecordingHotkey = false;
			// The event-driven cable capture preference lives in RSMods.ini (shared with the game DLL),
			// not this window's AudioRouting.ini. Ships enabled, so anything but "off" reads as on.
			syncingEventDrivenCapture = true;
			eventDrivenCaptureCheck.Checked = RSMods.ReadSettings.ProcessSettings(RSMods.ReadSettings.ModernCableInputIdentifier) != "off";
			syncingEventDrivenCapture = false;
			syncingCableForPlayerTwo = true;
			cableForPlayerTwoCheck.Checked = RSMods.ReadSettings.ProcessSettings(RSMods.ReadSettings.CableForPlayerTwoIdentifier) == "on";
			syncingCableForPlayerTwo = false;
			// The make-up gain also lives in RSMods.ini (shared with the game DLL). Load the saved value.
			syncingInputGain = true;
			persistedInputGainTenths = ReadInputGainTenths();
			inputGainEnableCheck.Checked = persistedInputGainTenths > 0;
			SetInputGain(persistedInputGainTenths);
			syncingInputGain = false;
			// The noise gate threshold also lives in RSMods.ini (shared with the game DLL). 0 = off, else negative
			// tenths of a dBFS; when off, seed the controls at -50 dB so enabling it starts somewhere sensible.
			syncingNoiseGate = true;
			persistedNoiseGateTenths = ReadGateThresholdTenths();
			gateEnableCheck.Checked = persistedNoiseGateTenths < 0;
			SetGateThreshold(persistedNoiseGateTenths < 0 ? persistedNoiseGateTenths : -500);
			ApplyGateVisibility();
			syncingNoiseGate = false;
			// The Rocksmith gate override (on + forced P1_NoiseFloor threshold) also lives in RSMods.ini.
			syncingRgGate = true;
			ReadRocksmithGate(out persistedRgOverride, out persistedRgTenths);
			rgEnableCheck.Checked = persistedRgOverride;
			SetRgThreshold(persistedRgTenths);
			ApplyRgGateVisibility();
			syncingRgGate = false;
			// The mains-hum notch also lives in RSMods.ini (0 = off, else base Hz 50/60).
			syncingHumFilter = true;
			persistedHumFilter = ReadHumFilter();
			humFilterCheck.Checked = persistedHumFilter >= 20;
			SetHumFilter(persistedHumFilter >= 20 ? persistedHumFilter : 50);
			ApplyHumFilterVisibility();
			syncingHumFilter = false;
			// The compressor strength also lives in RSMods.ini (shared with the game DLL).
			syncingCompressor = true;
			persistedCompressor = ReadCompressorStrength();
			compressorEnableCheck.Checked = persistedCompressor > 0;
			SetCompressor(persistedCompressor);
			syncingCompressor = false;
			ApplyInputGainVisibility();
			ApplyCompressorVisibility();
			// The limiter toggle also lives in RSMods.ini (shared with the game DLL).
			syncingLimiter = true;
			limiterCheck.Checked = RSMods.ReadSettings.ProcessSettings(RSMods.ReadSettings.AudioBridgeLimiterIdentifier) == "1";
			limiterLevelSlider.Value = LimiterTenthsToSlider(ReadLimiterLevelTenths());
			UpdateLimiterReadout();
			ApplyOutputControlVisibility();
			syncingLimiter = false;
			WireEvents();
			deviceEnumerator = new MMDeviceEnumerator();
			Marshal.ThrowExceptionForHR(deviceEnumerator.RegisterEndpointNotificationCallback(this));
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
			customBufferCheck.CheckedChanged += (sender, args) => UpdateState();
			applyBufferButton.Click += ApplyBuffer;
			measureLatencyButton.Click += MeasureLatency;
			tips.SetToolTip(measureLatencyButton, "Sends a short probe out of the active output and listens for it on the guitar input. Connect a physical output to the input first.");
			RefreshEnvironment();
			UpdateState();
			// Device enumeration (COM) and the takes scan (disk) used to run here and held the
			// window off the screen until they finished. They now run once the handle exists,
			// after the first paint, so the bridge appears instantly and fills in a moment later.
		}

		private bool initialized;

		protected override void OnHandleCreated(EventArgs e)
		{
			base.OnHandleCreated(e);
			if (initialized)
				return;
			initialized = true;
			BeginInvoke((Action)InitializeDeferred);
		}

		private void ShowSelectedPage()
		{
			if (nav == null) return;
			var selected = pages[nav.SelectedIndex];
			if (selected == null) return;
			selected.Visible = true;
			selected.BringToFront();
			foreach (var page in pages)
				if (page != null && page != selected) page.Visible = false;
		}

		private void InitializeDeferred()
		{
			ShowSelectedPage();
			takes.Load(recordingDirectory.Text, true);
			RefreshEnvironment();
			UpdateState();
			statusTimer.Start();
			mixerTimer.Start();
			RefreshDevices();
		}

		/// <summary>
		/// Re-read the slow environment facts (see the cached* fields). Called on the housekeeping tick and
		/// after actions that change them, never from the fast poll or a paint.
		/// </summary>
		private void RefreshEnvironment()
		{
			isGameRunning = inputMode.IsGameRunning();
			try
			{
				cachedMode = inputMode.ReadMode();
				cachedModeError = null;
			}
			catch (Exception error)
			{
				cachedMode = AudioInputMode.Unavailable;
				cachedModeError = error.Message;
			}
			cachedProxyRegistered = AsioProxySetup.IsProxyRegistered();
			cachedAsioDrivers = AsioProxySetup.ListRealDrivers();
			RefreshDiskSpace();
		}

		private void RefreshDiskSpace()
		{
			cachedFreeSpace = StudioFormat.FreeSpace(recordingDirectory.Text);
		}

		/// <summary>Hands text to the ToolTip only when it changed; see tipTexts.</summary>
		private void SetTip(Control control, string text)
		{
			if (tipTexts.TryGetValue(control, out string current) && current == text)
				return;
			tipTexts[control] = text;
			tips.SetToolTip(control, text);
		}

		private void BuildLayout()
		{
			var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3 };
			root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
			root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
			root.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
			root.Controls.Add(BuildHeader(), 0, 0);

			// One flat navigation column on the left and the current page on the right.
			var body = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Margin = new Padding(0) };
			body.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			body.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
			nav = new StudioNav("Mixer", "Guitar", "Recording", "Setup", "Diagnostics") { Dock = DockStyle.Fill, Margin = new Padding(0, 0, 14, 0) };
			var host = new Panel { Dock = DockStyle.Fill, Margin = new Padding(0) };
			pages[0] = BuildPage(BuildMixerCard());
			pages[1] = BuildPage(BuildInputGainCard());
			pages[2] = BuildRecordingPage();
			pages[3] = BuildPage(Columns(BuildRoutingCard(), BuildInputCard()), Columns(BuildProtectionCard(), BuildBufferCard()));
			pages[4] = BuildDiagnosticsPage();
			// Every page is added visible and stacked, so all of them are laid out at the real window size on
			// the first pass (a page laid out while hidden measures its wrapping hints at a stub width and
			// comes back many lines tall). The inactive pages are hidden once the first layout has happened,
			// in InitializeDeferred; until then z-order alone decides which page shows.
			foreach (var page in pages)
				host.Controls.Add(page);
			pages[0].BringToFront();
			nav.SelectedIndexChanged += (sender, args) => ShowSelectedPage();
			body.Controls.Add(nav, 0, 0);
			body.Controls.Add(host, 1, 0);
			root.Controls.Add(body, 0, 1);
			root.Controls.Add(BuildStatusBar(), 0, 2);
			Controls.Add(root);
		}

		private Control BuildDiagnosticsPage()
		{
			var settings = new StudioCard("In-game diagnostics");
			settings.Add(StudioTheme.Hint("These controls belong to the running Audio bridge. The signal overlay updates live while Rocksmith is connected; no restart is needed."));
			settings.Add(diagnosticsOverlayCheck);
			settings.Add(StudioTheme.Hint("Shows latency, input level and stream health in Rocksmith."));
			settings.Add(monitorOutputCheck);
			settings.Add(StudioTheme.Hint("Experimental startup diagnostic. It is read when an output stream is opened and may hang Rocksmith on some systems; restart Rocksmith after changing it."));

			var status = new StudioCard("Audio status");
			status.Add(Row(refreshDiagnosticsButton));
			diagnosticsStatus.MinimumSize = new Size(0, 300);
			status.Add(diagnosticsStatus, true);
			WireDiagnosticsEvents();
			return BuildPage(settings, status);
		}

		private void WireDiagnosticsEvents()
		{
			diagnosticsOverlayCheck.Checked = ReadBridgeSetting(RSMods.ReadSettings.AudioDiagnosticsOverlayIdentifier, "on") != "off";
			monitorOutputCheck.Checked = ReadBridgeSetting(RSMods.ReadSettings.MonitorOutputIdentifier, "off") == "on";
			diagnosticsOverlayCheck.CheckedChanged += (sender, args) =>
			{
				SetDiagnosticsOverlay(diagnosticsOverlayCheck.Checked);
			};
			monitorOutputCheck.CheckedChanged += (sender, args) =>
			{
				SaveDiagnosticsSetting(RSMods.ReadSettings.MonitorOutputIdentifier, monitorOutputCheck.Checked ? "on" : "off", false);
			};
			refreshDiagnosticsButton.Click += (sender, args) => RefreshDiagnosticsStatus();
			RefreshDiagnosticsStatus();
		}

		private void SetDiagnosticsOverlay(bool enabled)
		{
			SaveDiagnosticsSetting(RSMods.ReadSettings.AudioDiagnosticsOverlayIdentifier, enabled ? "on" : "off", true);
		}

		private async void SaveDiagnosticsSetting(string identifier, string value, bool applyLive)
		{
			try
			{
				WriteModSetting(identifier, value);
				if (applyLive && client != null)
					await client.SendAsync(27, value == "on" ? "1" : "0");
				SetStatus(identifier == RSMods.ReadSettings.AudioDiagnosticsOverlayIdentifier
					? (value == "on" ? "Audio diagnostics overlay enabled live." : "Audio diagnostics overlay disabled live.")
					: "Output stream logging saved. Restart Rocksmith before testing it.", ChipTone.Good);
			}
			catch (Exception error)
			{
				SetStatus("Could not update the diagnostic setting: " + error.Message, ChipTone.Bad);
			}
		}

		private static string ReadBridgeSetting(string identifier, string defaultValue)
		{
			string value = RSMods.ReadSettings.ProcessSettings(identifier);
			return string.IsNullOrWhiteSpace(value) ? defaultValue : value;
		}

		private void RefreshDiagnosticsStatus()
		{
			try
			{
				var report = new StringBuilder();
				string rsDir = RSMods.Util.GenUtil.GetRSDirectory();
				if (string.IsNullOrEmpty(rsDir) || !Directory.Exists(rsDir))
				{
					diagnosticsStatus.Text = "Rocksmith folder not found.";
					return;
				}
				report.AppendLine("Rocksmith: " + (Process.GetProcessesByName("Rocksmith2014").Length > 0 ? "RUNNING" : "not running"));
				report.AppendLine("RS_ASIO.dll: " + (File.Exists(Path.Combine(rsDir, "RS_ASIO.dll")) ? "present" : "absent"));
				report.AppendLine("Audio diagnostics overlay: " + (ReadBridgeSetting(RSMods.ReadSettings.AudioDiagnosticsOverlayIdentifier, "on") == "off" ? "off" : "on") + " (live while connected)");
				report.AppendLine("Output stream logging: " + (ReadBridgeSetting(RSMods.ReadSettings.MonitorOutputIdentifier, "off") == "on" ? "on; restart required" : "off"));
				report.AppendLine();
				report.AppendLine("RSMods_debug.txt:");
				AppendMatchingLines(report, Path.Combine(rsDir, "RSMods_debug.txt"), new[] { "(CABLE INPUT)", "(OUTPUT)", "(AUDIO ROUTING)", "[AsioHook]", "[InputCapture]", "[ERROR]" }, 25);
				diagnosticsStatus.Text = report.ToString();
			}
			catch (Exception error)
			{
				diagnosticsStatus.Text = "Could not read audio status: " + error.Message;
			}
		}

		private static void AppendMatchingLines(StringBuilder report, string path, string[] needles, int maximum)
		{
			if (!File.Exists(path))
			{
				report.AppendLine("File not found.");
				return;
			}
			var matches = new List<string>();
			foreach (string line in File.ReadLines(path))
			{
				foreach (string needle in needles)
				{
					if (!line.IndexOf(needle, StringComparison.OrdinalIgnoreCase).Equals(-1))
					{
						matches.Add(line.Trim());
						break;
					}
				}
			}
			int start = Math.Max(0, matches.Count - maximum);
			for (int index = start; index < matches.Count; index++)
				report.AppendLine(matches[index]);
		}

		/// <summary>
		/// Stacks cards down a page. A stretch card (one built with stretch: true, such as the mixer console)
		/// takes whatever height the window has left, so the page fills the window instead of leaving a dark
		/// band under the last card; the other cards keep their natural height and the page scrolls if the
		/// window is too short for them.
		/// </summary>
		private static Control BuildPage(params Control[] sections)
		{
			bool stretches = false;
			foreach (var section in sections) stretches |= !section.AutoSize;
			var page = new Panel { Dock = DockStyle.Fill, Margin = new Padding(0), AutoScroll = !stretches };
			var body = new TableLayoutPanel { Dock = stretches ? DockStyle.Fill : DockStyle.Top, AutoSize = !stretches, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 1, Margin = new Padding(0) };
			body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			foreach (var section in sections)
			{
				section.Dock = DockStyle.Fill;
				bool fills = stretches && !section.AutoSize;
				if (fills && section.MinimumSize.Height == 0) section.MinimumSize = new Size(0, 200);
				body.RowStyles.Add(fills ? new RowStyle(SizeType.Percent, 100) : new RowStyle(SizeType.AutoSize));
				body.Controls.Add(section);
			}
			page.Controls.Add(body);
			return page;
		}

		// Recording page: Transport and Take options side by side, Recent takes filling the rest. Docked, so
		// the page never scrolls and never leaves an empty band.
		private Control BuildRecordingPage()
		{
			var page = new Panel { Dock = DockStyle.Fill, Margin = new Padding(0) };
			var body = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2, Margin = new Padding(0) };
			body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			body.RowStyles.Add(new RowStyle(SizeType.AutoSize));
			body.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
			body.Controls.Add(Columns(BuildDeck(), BuildOptionsCard()), 0, 0);
			var takes = BuildTakesCard();
			takes.Margin = new Padding(0);
			body.Controls.Add(takes, 0, 1);
			page.Controls.Add(body);
			return page;
		}

		// Header: power rocker, the window's name and a one-line summary on the left; the live chips on the right.
		// Containers carry no BackColor of their own: they inherit the panel's opaque Background, so nothing here
		// asks the parent to repaint underneath it.
		private Control BuildHeader()
		{
			var header = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0, 0, 0, 14) };
			header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			header.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			var titles = new TableLayoutPanel { ColumnCount = 2, RowCount = 2, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0) };
			titles.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			titles.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			titles.RowStyles.Add(new RowStyle(SizeType.AutoSize));
			titles.RowStyles.Add(new RowStyle(SizeType.AutoSize));
			masterPower.Location = Point.Empty;
			masterPowerHost.Size = masterPower.Size;
			masterPowerHost.Controls.Add(masterPower);
			masterPowerHost.Margin = new Padding(0, 4, 0, 0);
			tips.SetToolTip(masterPower, "Turns the audio bridge on or off while Rocksmith is closed. Note by Note is unaffected.");
			tips.SetToolTip(masterPowerHost, "Turns the audio bridge on or off while Rocksmith is closed. Note by Note is unaffected.");
			titles.Controls.Add(masterPowerHost, 0, 0);
			titles.SetRowSpan(masterPowerHost, 2);
			titles.Controls.Add(new Label { Text = "Audio bridge", Font = StudioTheme.Title, ForeColor = StudioTheme.Ink, AutoSize = true, UseMnemonic = false, Margin = new Padding(14, 0, 0, 0) }, 1, 0);
			titles.Controls.Add(new Label { Text = "Playback mixer, guitar input processing, recording and output routing for Rocksmith.", Font = StudioTheme.Body, ForeColor = StudioTheme.Muted, AutoSize = true, UseMnemonic = false, Margin = new Padding(14, 0, 0, 0) }, 1, 1);
			header.Controls.Add(titles, 0, 0);
			// Right side: the performance bolt (lit only when fast ASIO output is live) sits just left of the
			// connection chip. Right-anchored, so the chip stays put and the bolt appears to its left when it lights.
			var right = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, FlowDirection = FlowDirection.LeftToRight, Anchor = AnchorStyles.Right, Margin = new Padding(0, 6, 0, 0) };
			tips.SetToolTip(perfBolt, "Low-latency ASIO output is live: the game plays straight out of the ASIO device.");
			right.Controls.Add(perfBolt);
			connectionChip.Anchor = AnchorStyles.None;
			right.Controls.Add(connectionChip);
			bridgeChip.Anchor = AnchorStyles.None;
			tips.SetToolTip(bridgeChip, "Green while the bridge is routing the game's audio to your chosen output; grey while the game plays out of its own output untouched.");
			right.Controls.Add(bridgeChip);
			header.Controls.Add(right, 1, 0);
			return header;
		}

		// Transport card: clock and REC badge, the meter, Record / Stop, the state chips and the hints.
		private Control BuildDeck()
		{
			var deck = new StudioCard("Transport") { Dock = DockStyle.Fill };
			var clock = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0) };
			recordingChip.Margin = new Padding(0, 12, 0, 0);
			clock.Controls.Add(elapsedLabel);
			clock.Controls.Add(recordingChip);
			deck.Add(clock);
			deck.Add(meter);
			var transport = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0, 6, 0, 4) };
			recordButton.MinimumTextWidth = 150;
			stopButton.MinimumTextWidth = 150;
			transport.Controls.Add(recordButton);
			transport.Controls.Add(stopButton);
			deck.Add(transport);
			var chips = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0, 6, 0, 0) };
			chips.Controls.Add(inputChip);
			chips.Controls.Add(captureChip);
			chips.Controls.Add(diskChip);
			deck.Add(chips);
			deckHint.Margin = new Padding(0, 10, 0, 2);
			deck.Add(deckHint);
			recordingHotkeyStatus.Margin = new Padding(0, 2, 0, 0);
			deck.Add(recordingHotkeyStatus);
			return deck;
		}

		// Take options card: the three choices as captioned rows, then the folder the takes go to.
		private Control BuildOptionsCard()
		{
			var card = new StudioCard("Take options") { Dock = DockStyle.Fill };
			var options = new TableLayoutPanel { Dock = DockStyle.Top, ColumnCount = 2, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0) };
			options.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 76));
			options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			AddOptionRow(options, 0, "Format", captureMode);
			AddOptionRow(options, 1, "Source", recordingSource);
			recordingHotkeySelector.Margin = new Padding(0, 4, 0, 4);
			AddOptionRow(options, 2, "Hotkey", recordingHotkeySelector);
			recordingDirectory.Margin = new Padding(0, 4, 0, 4);
			AddOptionRow(options, 3, "Folder", recordingDirectory);
			recordingDirectory.Anchor = AnchorStyles.Left | AnchorStyles.Right;
			card.Add(options);
			var buttons = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0, 2, 0, 0) };
			buttons.Controls.Add(browseButton);
			buttons.Controls.Add(openButton);
			card.Add(buttons);
			spaceLabel.Margin = new Padding(0, 8, 0, 2);
			card.Add(spaceLabel);
			return card;
		}

		private static void AddOptionRow(TableLayoutPanel table, int row, string caption, Control control)
		{
			var label = StudioTheme.Caption(caption);
			label.Anchor = AnchorStyles.Left;
			label.Margin = new Padding(0, 0, 12, 0);
			control.Anchor = AnchorStyles.Left;
			table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
			table.Controls.Add(label, 0, row);
			table.Controls.Add(control, 1, row);
		}

		private Control BuildMixerCard()
		{
			var card = new StudioCard("Playback mixer", true) { Dock = DockStyle.Fill };
			card.Add(StudioTheme.Hint("Balances the game's audio buses. Levels follow Rocksmith while it is connected. Click a speaker to mute; double-click a fader for 100%."));
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
			var card = new StudioCard("Output device");
			card.Add(StudioTheme.Hint("Where Rocksmith plays. A device with an ASIO driver plays through ASIO; anything else is bridged over Windows audio with a little added latency."));
			card.Add(outputSelector);
			var buttons = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0, 4, 0, 0) };
			buttons.Controls.Add(applyButton);
			buttons.Controls.Add(removeProxyButton);
			card.Add(buttons);
			asioProxyLabel.Margin = new Padding(0, 8, 0, 2);
			card.Add(asioProxyLabel);
			card.Add(StudioTheme.Hint("Windows outputs can be switched while the game runs. Changing the ASIO output needs Rocksmith closed."));
			return card;
		}

		// Output protection: the proxy driver's look-ahead limiter. Its own card, so the routing card stays
		// about routing and the limiter reads as the safety feature it is.
		private Control BuildProtectionCard()
		{
			var card = new StudioCard("Output protection");
			card.Add(StudioTheme.Hint("Holds the game's output under a ceiling so a loud transient never reaches your ears at full scale. Adds a few milliseconds of output latency. Does not change the interface's volume knob."));
			var grid = FeatureGrid();
			AddFeature(grid, limiterCheck, limiterLevelSlider, ceilingInput, "dB", null);
			card.Add(grid);
			tips.SetToolTip(limiterCheck, LIMITER_HINT);
			return card;
		}

		private Control BuildBufferCard()
		{
			var card = new StudioCard("Audio buffers");
			card.Add(StudioTheme.Hint("Buffer sizes are not the whole guitar latency. The ASIO input buffer is set in your interface's own control panel and read when Rocksmith starts."));
			asioBufferLabel.Margin = new Padding(0, 8, 0, 2);
			card.Add(asioBufferLabel);
			card.Add(currentBufferLabel);
			customBufferCheck.Margin = new Padding(0, 10, 0, 2);
			card.Add(customBufferCheck);
			card.Add(Row(customBuffer, applyBufferButton));
			tips.SetToolTip(customBufferCheck, "Windows playback only. Automatic uses the playback device's default period.");
			tips.SetToolTip(applyBufferButton, "Apply the buffer to the live Windows playback stream and save it for the next launch.");
			measureLatencyButton.Margin = new Padding(0, 14, 8, 4);
			card.Add(Row(measureLatencyButton));
			latencyReadout.Margin = new Padding(0, 2, 0, 4);
			card.Add(latencyReadout);
			return card;
		}

		private Control BuildInputCard()
		{
			var card = new StudioCard("Guitar input");
			card.Add(StudioTheme.Hint("How Rocksmith hears the guitar. ASIO uses RS_ASIO and your interface for the lowest latency; the cable uses the Windows audio path."));
			card.Add(inputSelector);
			inputModeLabel.Margin = new Padding(0, 8, 0, 2);
			card.Add(inputModeLabel);
			card.Add(eventDrivenCaptureCheck);
			eventDrivenCaptureHint.Margin = new Padding(0, 0, 0, 2);
			card.Add(eventDrivenCaptureHint);
			card.Add(cableForPlayerTwoCheck);
			cableForPlayerTwoHint.Margin = new Padding(0, 0, 0, 2);
			card.Add(cableForPlayerTwoHint);
			return card;
		}

		// Guitar input processing lives on the Mixer tab (the controls are live levels, like the faders), as
		// one grid: toggle | slider | value | unit, then a one-line hint. Off features stay visible but
		// disabled, so the card never jumps as things are switched on and off.
		private Control BuildInputGainCard()
		{
			var card = new StudioCard("Guitar input processing");
			card.Add(StudioTheme.Hint("Shapes the guitar signal before Rocksmith hears it. Changes apply live and are saved."));
			var grid = FeatureGrid();
			AddFeature(grid, inputGainEnableCheck, inputGainSlider, gainInput, "dB",
				"Lifts a quiet interface input towards Real Tone Cable level, so the game's note gate stops cutting sustains short.");
			AddFeature(grid, gateEnableCheck, noiseGateSlider, gateThresholdInput, "dB",
				"Silences the input below this level between notes, so the gain above does not amplify hiss. Set it just above your noise floor.");
			AddFeature(grid, compressorEnableCheck, compressorSlider, compInput, "%",
				"Evens out level swings during sustained notes, the way a hot cable's signal already is.");
			AddFeature(grid, humFilterCheck, humFilterSlider, humFilterInput, "Hz",
				"Removes mains hum from a grounded interface. 50 Hz in the UK, EU and AU; 60 Hz in the US.");
			AddFeature(grid, rgEnableCheck, rgGateSlider, rgGateInput, "dB",
				"Overrides Rocksmith's own amp gate so decaying notes are not cut off. Lower values keep notes ringing longer.");
			card.Add(grid);
			return card;
		}

		/// <summary>Four-column grid for feature rows: toggle, slider (stretches), value box, unit.</summary>
		private static TableLayoutPanel FeatureGrid()
		{
			var grid = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 4, Margin = new Padding(0, 6, 0, 0) };
			grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 168));
			grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			return grid;
		}

		private static void AddFeature(TableLayoutPanel grid, StudioCheck toggle, StudioSlider slider, StudioNumber box, string unit, string hint)
		{
			int row = grid.RowStyles.Count;
			grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
			toggle.Anchor = AnchorStyles.Left;
			toggle.Margin = new Padding(0, 4, 8, 4);
			slider.Anchor = AnchorStyles.Left | AnchorStyles.Right;
			slider.Margin = new Padding(0, 2, 0, 2);
			box.Anchor = AnchorStyles.Left;
			box.Margin = new Padding(12, 2, 0, 2);
			var unitLabel = new Label { Text = unit, AutoSize = true, UseMnemonic = false, Font = StudioTheme.Small, ForeColor = StudioTheme.Muted, Anchor = AnchorStyles.Left, Margin = new Padding(6, 0, 0, 0) };
			grid.Controls.Add(toggle, 0, row);
			grid.Controls.Add(slider, 1, row);
			grid.Controls.Add(box, 2, row);
			grid.Controls.Add(unitLabel, 3, row);
			if (hint == null) return;
			grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
			var text = StudioTheme.Hint(hint);
			text.Anchor = AnchorStyles.Left | AnchorStyles.Right;   // wraps to the grid width and grows in height
			text.Margin = new Padding(0, 0, 0, 10);
			grid.Controls.Add(text, 0, row + 1);
			grid.SetColumnSpan(text, 4);
		}
		/// <summary>The ASIO driver name that reaches the same hardware as a Windows playback endpoint, or null
		/// when the endpoint has no ASIO twin. ASIO drivers and WASAPI endpoints are separate namespaces for the
		/// same box (e.g. "Speakers (3- M-Audio M-Track Solo and Duo)" vs "M-Audio M-Track Solo and Duo ASIO"),
		/// so we match on the device description carried inside the endpoint's friendly name.</summary>
		private string FindMatchingAsioDriver(string endpointFriendlyName)
		{
			if (string.IsNullOrEmpty(endpointFriendlyName)) return null;
			string core = DeviceCore(endpointFriendlyName);
			if (core.Length < 3) return null;
			foreach (var driver in cachedAsioDrivers)
			{
				string driverCore = DeviceCore(driver);
				if (driverCore.IndexOf(core, StringComparison.OrdinalIgnoreCase) >= 0
					|| core.IndexOf(driverCore, StringComparison.OrdinalIgnoreCase) >= 0)
					return driver;
			}
			return null;
		}

		/// <summary>The bare hardware description: the text inside the last parentheses of a WASAPI friendly name
		/// with any "N- " port prefix removed ("Speakers (3- M-Audio M-Track...)" -> "M-Audio M-Track..."), or the
		/// ASIO driver name with a trailing " ASIO" removed. Lets the two namespaces be compared.</summary>
		private static string DeviceCore(string name)
		{
			string s = name ?? "";
			int open = s.LastIndexOf('(');
			int close = s.LastIndexOf(')');
			if (open >= 0 && close > open) s = s.Substring(open + 1, close - open - 1);
			int dash = s.IndexOf("- ", StringComparison.Ordinal);
			if (dash >= 0 && dash <= 3) s = s.Substring(dash + 2);
			if (s.EndsWith(" ASIO", StringComparison.OrdinalIgnoreCase)) s = s.Substring(0, s.Length - 5);
			return s.Trim();
		}

		/// <summary>Refresh the single line under the output selector to describe how the selected device will be
		/// reached and whether game-mix recording is available, without applying anything.</summary>
		private void UpdateOutputHint()
		{
			AudioInputMode mode = cachedMode;
			bool installed = cachedProxyRegistered;
			proxyInstalled = installed;
			// A way to back out only exists once it is actually installed, and only while the game is closed:
			// Proxy registration is independent of the user-owned RS_ASIO.ini and is safe to inspect here.
			removeProxyButton.Visible = installed && !isGameRunning;
			limiterCheck.Enabled = installed;
			ApplyOutputControlVisibility();          // the ceiling follows the limiter's on/off
			string driver = mode == AudioInputMode.Asio && outputSelector.SelectedItem is AudioDeviceChoice output
				? FindMatchingAsioDriver(output.Name) : null;
			if (driver != null)
				asioProxyLabel.Text = installed
					? "Low-latency ASIO output through " + driver + ". Game-mix recording is ready."
					: "This device has an ASIO driver (" + driver + "). Press Apply output to use it and enable game-mix recording.";
			else
				asioProxyLabel.Text = "Windows output with a little added latency. Game-mix recording uses window capture.";
		}

		/// <summary>Back out completely: restore RS_ASIO to the real driver, then unregister the proxy (elevated).
		/// Unlink happens first and needs no admin, so even if the user declines the elevation the game is already
		/// back on its direct ASIO output.</summary>
		private void RemoveProxy(object sender, EventArgs args)
		{
			if (isCommandRunning) return;
			if (inputMode.IsGameRunning())
			{
				SetStatus("Close Rocksmith before removing the audio bridge driver; RS_ASIO reads the output at launch.", ChipTone.Warn);
				return;
			}
			isCommandRunning = true;
			UpdateState();
			try
			{
				AsioProxySetup.Unlink(gameDirectory);   // restore RS_ASIO.ini to the real driver (no admin)
				try
				{
					AsioProxySetup.Unregister(Path.Combine(gameDirectory, "RocksmithAudioBridge.dll"));
					RefreshEnvironment();
					UpdateOutputHint();
					SetStatus("Audio bridge driver removed. Output is back on your real ASIO device. Relaunch Rocksmith to apply.", ChipTone.Good);
				}
				catch (OperationCanceledException)
				{
					// Routing is already restored; the driver is still registered because the prompt was declined.
					RefreshEnvironment();
					UpdateOutputHint();
					SetStatus("Output restored to your real ASIO device. The bridge driver is still installed (removal needs admin approval).", ChipTone.Warn);
				}
			}
			catch (Exception error) { SetStatus("Could not remove the audio bridge driver: " + error.Message, ChipTone.Bad); }
			finally { isCommandRunning = false; UpdateState(); }
		}
		private static Control Columns(Control left, Control right)
		{
			var pair = new TableLayoutPanel { Dock = DockStyle.Top, ColumnCount = 2, RowCount = 1, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0) };
			pair.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
			pair.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
			left.Margin = new Padding(0, 0, 7, 14);
			right.Margin = new Padding(7, 0, 0, 14);
			left.Dock = DockStyle.Fill;
			right.Dock = DockStyle.Fill;
			pair.Controls.Add(left, 0, 0);
			pair.Controls.Add(right, 1, 0);
			return pair;
		}

		private static Control Row(params Control[] controls)
		{
			var row = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0) };
			foreach (Control control in controls)
				row.Controls.Add(control);
			return row;
		}

		private static Control FieldRow(Control field, params Control[] buttons)
		{
			var row = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0) };
			row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			field.Dock = DockStyle.Fill;
			field.Margin = new Padding(0, 13, 14, 0);
			row.Controls.Add(field, 0, 0);
			var group = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0) };
			foreach (Control button in buttons)
				group.Controls.Add(button);
			row.Controls.Add(group, 1, 0);
			return row;
		}

		private Control BuildTakesCard()
		{
			var card = new StudioCard("Recent takes", true) { Dock = DockStyle.Fill, MinimumSize = new Size(0, 220) };
			card.Add(takes, true);
			var buttons = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = new Padding(0, 10, 0, 0) };
			buttons.Controls.Add(playButton);
			buttons.Controls.Add(revealButton);
			card.Add(buttons);
			libraryLabel.Margin = new Padding(0, 8, 0, 2);
			card.Add(libraryLabel);
			return card;
		}

		private Control BuildStatusBar()
		{
			var bar = new Panel { Dock = DockStyle.Fill, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Padding = new Padding(2, 10, 2, 0) };
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
			masterPower.Toggled += (sender, args) => { if (!syncingMasterPower) SetMasterPower(masterPower.IsOn); };
			removeProxyButton.Click += RemoveProxy;
			browseButton.Click += ChooseFolder;
			openButton.Click += OpenFolder;
			playButton.Click += (sender, args) => Launch(takes.SelectedPath);
			revealButton.Click += (sender, args) => Reveal(takes.SelectedPath);
			takes.DoubleClick += (sender, args) => Launch(takes.SelectedPath);
			takes.SelectedIndexChanged += (sender, args) => UpdateTakeButtons();
			captureMode.SelectedIndexChanged += (sender, args) => UpdateState();
			recordingSource.SelectedIndexChanged += (sender, args) => UpdateState();
			recordingHotkeySelector.SelectedIndexChanged += RecordingHotkeySelectionChanged;
			outputSelector.SelectedIndexChanged += (sender, args) => { UpdateOutputHint(); UpdateState(); };
			outputSelector.DropDown += (sender, args) => RefreshDevices();
			inputSelector.SelectedIndexChanged += (sender, args) => { if (!syncingInputMode) SwitchInputMode(inputSelector.SelectedIndex == 1); };
			UpdateOutputHint();
			eventDrivenCaptureCheck.CheckedChanged += (sender, args) => { if (!syncingEventDrivenCapture) SaveEventDrivenCapture(); };
			cableForPlayerTwoCheck.CheckedChanged += (sender, args) => { if (!syncingCableForPlayerTwo) SaveCableForPlayerTwo(); };
			limiterCheck.CheckedChanged += (sender, args) =>
			{
				if (syncingLimiter) return;
				pendingLimiter = LimiterCommand();                                // applied live on the next mixer tick
				ApplyOutputControlVisibility();                                   // reveal the ceiling immediately
				UpdateState();                                                    // enable/disable the ceiling slider
				try { WriteLimiterSetting(limiterCheck.Checked); }                // persisted so it arms at next launch
				catch (Exception error) { SetStatus("Could not save the limiter setting: " + error.Message, ChipTone.Bad); }
			};
			limiterLevelSlider.ValueChanged += (sender, args) =>
			{
				if (syncingLimiter) return;
				UpdateLimiterReadout();
				pendingLimiter = LimiterCommand();
				lastMixerEdit = DateTime.UtcNow;
			};
			limiterLevelSlider.MouseUp += (sender, args) => PersistLimiterLevel();
			limiterLevelSlider.KeyUp += (sender, args) => PersistLimiterLevel();
			ceilingInput.ValueChanged += OnCeilingInputChanged;
			inputGainSlider.ValueChanged += OnInputGainChanged;
			inputGainSlider.MouseUp += (sender, args) => PersistInputGain();
			inputGainSlider.KeyUp += (sender, args) => PersistInputGain();
			gainInput.ValueChanged += OnGainInputChanged;
			inputGainEnableCheck.CheckedChanged += (sender, args) =>
			{
				if (syncingInputGain) return;
				ApplyInputGainVisibility();
				if (!inputGainEnableCheck.Checked)
				{
					syncingInputGain = true;
					SetInputGain(0);
					syncingInputGain = false;
				}
				PersistInputGain();
			};
			noiseGateSlider.ValueChanged += OnNoiseGateChanged;
			noiseGateSlider.MouseUp += (sender, args) => PersistNoiseGate();
			noiseGateSlider.KeyUp += (sender, args) => PersistNoiseGate();
			gateThresholdInput.ValueChanged += OnGateInputChanged;
			gateEnableCheck.CheckedChanged += (sender, args) =>
			{
				if (syncingNoiseGate) return;
				ApplyGateVisibility();
				pendingNoiseGateTenths = GateEffectiveTenths();
				lastMixerEdit = DateTime.UtcNow;
				PersistNoiseGate();
			};
			compressorSlider.ValueChanged += OnCompressorChanged;
			compressorSlider.MouseUp += (sender, args) => PersistCompressor();
			compressorSlider.KeyUp += (sender, args) => PersistCompressor();
			compInput.ValueChanged += OnCompInputChanged;
			compressorEnableCheck.CheckedChanged += (sender, args) =>
			{
				if (syncingCompressor) return;
				ApplyCompressorVisibility();
				if (!compressorEnableCheck.Checked)
				{
					syncingCompressor = true;
					SetCompressor(0);
					syncingCompressor = false;
				}
				PersistCompressor();
			};
			rgGateSlider.ValueChanged += OnRgGateChanged;
			rgGateSlider.MouseUp += (sender, args) => PersistRocksmithGate();
			rgGateSlider.KeyUp += (sender, args) => PersistRocksmithGate();
			rgGateInput.ValueChanged += OnRgInputChanged;
			rgEnableCheck.CheckedChanged += (sender, args) =>
			{
				if (syncingRgGate) return;
				ApplyRgGateVisibility();
				pendingRocksmithGate = (rgEnableCheck.Checked, RgThresholdTenths());
				lastMixerEdit = DateTime.UtcNow;
				PersistRocksmithGate();
			};
			humFilterSlider.ValueChanged += OnHumSliderChanged;
			humFilterSlider.MouseUp += (sender, args) => PersistHumFilter();
			humFilterSlider.KeyUp += (sender, args) => PersistHumFilter();
			humFilterInput.ValueChanged += OnHumInputChanged;
			humFilterCheck.CheckedChanged += (sender, args) =>
			{
				if (syncingHumFilter) return;
				ApplyHumFilterVisibility();
				pendingHumFilter = HumEffectiveHz();
				lastMixerEdit = DateTime.UtcNow;
				PersistHumFilter();
			};
			recordingDirectory.Leave += (sender, args) => { takes.Load(recordingDirectory.Text, true); RefreshDiskSpace(); UpdateState(); };
			statusTimer.Tick += PollStatus;
			UpdateRecordingHotkeyHints();
			tips.SetToolTip(applyButton, "Use the selected device. A device with an ASIO driver plays through ASIO (best latency, and game-mix recording works); anything else is bridged over Windows audio.");
			tips.SetToolTip(openButton, "Open the takes folder in Explorer.");
			tips.SetToolTip(browseButton, "Choose the folder takes are saved to.");
			tips.SetToolTip(captureMode, "Both formats record the selected audio source. MP4 also records the Rocksmith window.");
			tips.SetToolTip(recordingSource, "Game mix records everything you hear. Dry guitar records Player 1's input after pitch shifting and before any tone. Playback is unchanged either way.");
			tips.SetToolTip(inputSelector, "ASIO uses RS_ASIO and your interface for the lowest latency. The Real Tone Cable uses the Windows audio path.");
			tips.SetToolTip(eventDrivenCaptureCheck, "Reads the Real Tone Cable with Windows audio events. Whether it feels lower latency depends on the machine. Restart Rocksmith after changing it.");
			tips.SetToolTip(cableForPlayerTwoCheck, "Adds the RSModsPlus Real Tone Cable device after the configured ASIO input. RS_ASIO.ini is not changed. Applies live while Rocksmith is connected.");
			tips.SetToolTip(inputGainEnableCheck, "Adds gain to the guitar signal before Rocksmith hears it. 0 dB leaves the input unchanged. Applies live.");
			tips.SetToolTip(inputGainSlider, "Input gain from 0 to +20 dB. Type an exact value in the box or nudge by 0.1 dB.");
			tips.SetToolTip(gainInput, "Input gain from 0 to +20 dB. Type an exact value or nudge by 0.1 dB.");
			tips.SetToolTip(gateEnableCheck, "Mutes the input below the threshold between notes, before the input gain amplifies it. Too high cuts note tails; too low lets hiss through. Applies live.");
			tips.SetToolTip(noiseGateSlider, "Noise gate threshold from -80 to -20 dBFS. Type an exact value in the box or nudge by 0.1 dB.");
			tips.SetToolTip(gateThresholdInput, "Noise gate threshold from -80 to -20 dBFS. Type an exact value or nudge by 0.1 dB.");
			tips.SetToolTip(compressorEnableCheck, "Compresses the guitar input to even out level swings during sustained notes. Applies live.");
			tips.SetToolTip(compressorSlider, "Compressor strength from 0 to 100%. Type an exact value in the box.");
			tips.SetToolTip(compInput, "Compressor strength from 0 to 100%. Type an exact value or nudge by 1.");
			tips.SetToolTip(humFilterCheck, "Notches out mains hum and its harmonics, continuously and even during notes. Applies live.");
			tips.SetToolTip(humFilterSlider, "Mains frequency from 20 to 120 Hz. 50 Hz in the UK, EU and AU; 60 Hz in the US.");
			tips.SetToolTip(humFilterInput, "Mains frequency from 20 to 120 Hz. Type an exact value or nudge by 1 Hz.");
			tips.SetToolTip(rgEnableCheck, "Takes over Rocksmith's own amp noise gate so the game stops cutting a note as it decays. Applies live.");
			tips.SetToolTip(rgGateSlider, "Rocksmith gate threshold from -100 to +10 dB. Lower values keep the gate open for longer sustain.");
			tips.SetToolTip(rgGateInput, "Rocksmith gate threshold from -100 to +10 dB. Lower values keep the gate open for longer sustain.");
			tips.SetToolTip(limiterLevelSlider, "Limiter ceiling from -24 to 0 dBFS. Nothing leaves the bridge louder than this.");
			tips.SetToolTip(ceilingInput, "Limiter ceiling from -24 to 0 dBFS. Type an exact value or nudge by 0.1 dB.");
		}

		internal void ToggleRecordingFromHotkey()
		{
			if (recordButton.Enabled)
				Record(this, EventArgs.Empty);
			else if (stopButton.Enabled)
				Stop(this, EventArgs.Empty);
		}

		private void RecordingHotkeySelectionChanged(object sender, EventArgs args)
		{
			if (syncingRecordingHotkey)
				return;
			WriteRsModsSetting(RSMods.ReadSettings.RecordingHotkeyIdentifier, KeyConversion.VirtualKey(RecordingHotkey.ToString()), "[Keybinds]");
			RSMods.Util.WinMsgUtil.SendMsgToRS("update all");
			UpdateRecordingHotkeyHints();
			UpdateState();
		}

		private void UpdateRecordingHotkeyHints()
		{
			string key = RecordingHotkey.ToString();
			SetTip(recordButton, "Start a take (" + key + ")");
			SetTip(stopButton, "Finish the take and write the file (" + key + ")");
			recordingHotkeyStatus.Text = "In-game hotkey: " + key + ". Press it in Rocksmith to start or stop a take.";
			recordingHotkeyStatus.ForeColor = StudioTheme.Positive;
		}

		void IMMNotificationClient.OnDeviceStateChanged(string deviceId, DeviceState newState) => Interlocked.Exchange(ref devicesChanged, 1);

		void IMMNotificationClient.OnDeviceAdded(string deviceId) => Interlocked.Exchange(ref devicesChanged, 1);

		void IMMNotificationClient.OnDeviceRemoved(string deviceId) => Interlocked.Exchange(ref devicesChanged, 1);

		void IMMNotificationClient.OnDefaultDeviceChanged(DataFlow flow, Role role, string deviceId) => Interlocked.Exchange(ref devicesChanged, 1);

		void IMMNotificationClient.OnPropertyValueChanged(string deviceId, PropertyKey key) => Interlocked.Exchange(ref devicesChanged, 1);

		/// <summary>
		/// Reads the render endpoints on a background thread and posts the result back. Reading each
		/// device's <c>FriendlyName</c> is a COM property-store call, and on a cold audio stack a handful
		/// of devices can take a few hundred milliseconds; doing it on the UI thread froze the window
		/// (and the device dropdown) for that whole time. A throwaway enumerator keeps this off the
		/// shared <see cref="deviceEnumerator"/>, which is bound to this panel for change notifications.
		/// Overlapping calls coalesce: the list stays current via those notifications, so a second
		/// refresh while one is in flight is a no-op rather than a pile-up.
		/// </summary>
		private void RefreshDevices()
		{
			if (Interlocked.CompareExchange(ref enumeratingDevices, 1, 0) != 0)
				return;
			var previous = outputSelector.SelectedItem as AudioDeviceChoice;
			string selected = previous?.Id ?? ReadSetting("OutputDevice", "");
			Task.Run(() =>
			{
				try
				{
					var devices = new List<AudioDeviceChoice>();
					string resolved = selected;
					using (var reader = new MMDeviceEnumerator())
					{
						foreach (MMDevice device in reader.EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active))
						{
							using (device)
							{
								devices.Add(new AudioDeviceChoice(device.ID, device.FriendlyName));
							}
						}
						bool selectedIsActive = devices.Exists(device => device.Id == resolved);
						if (!selectedIsActive && devices.Count > 0)
						{
							using (var defaultOutput = reader.GetDefaultAudioEndpoint(DataFlow.Render, Role.Console)) resolved = defaultOutput.ID;
						}
					}
					PostToUi(() => UpdateDevices(devices, resolved));
				}
				catch (COMException error)
				{
					PostToUi(() =>
					{
						outputSelector.Items.Clear();
						UpdateState();
						SetStatus("Could not read playback devices: " + error.Message, ChipTone.Bad);
					});
				}
				finally
				{
					Interlocked.Exchange(ref enumeratingDevices, 0);
				}
			});
		}

		/// <summary>Runs an action on the UI thread, swallowing the race where the handle is gone during shutdown.</summary>
		private void PostToUi(Action action)
		{
			if (IsDisposed || !IsHandleCreated)
				return;
			try { BeginInvoke(action); }
			catch (InvalidOperationException) { }
		}

		private void UpdateDevices(List<AudioDeviceChoice> devices, string selected)
		{
			bool changed = outputSelector.Items.Count != devices.Count;
			for (int index = 0; !changed && index < devices.Count; index++)
			{
				var existing = (AudioDeviceChoice)outputSelector.Items[index];
				changed = existing.Id != devices[index].Id || existing.Name != devices[index].Name;
			}
			if (!changed)
			{
				if (!(outputSelector.SelectedItem is AudioDeviceChoice))
				{
					for (int index = 0; index < outputSelector.Items.Count; index++)
					{
						if (((AudioDeviceChoice)outputSelector.Items[index]).Id == selected)
						{
							outputSelector.SelectedIndex = index;
							break;
						}
					}
				}
				UpdateState();
				return;
			}

			outputSelector.BeginUpdate();
			try
			{
				outputSelector.Items.Clear();
				foreach (var device in devices)
				{
					int index = outputSelector.Items.Add(device);
					if (device.Id == selected) outputSelector.SelectedIndex = index;
				}
			}
			finally
			{
				outputSelector.EndUpdate();
				UpdateState();
			}
		}

		private async void PollStatus(object sender, EventArgs args)
		{
			if (!masterEnabled)
			{
				client = null;
				latestStatus = null;
				connectionChip.Set("Audio bridge off", ChipTone.Idle);
				bridgeChip.Visible = false;
				SetPollInterval(SearchingPollMilliseconds);
				SetStatus(masterOffRequiresClose
					? "Audio bridge is off. Close Rocksmith to restore its saved audio setup."
					: "Audio bridge is off. Audio setup is untouched; Note by Note still works.", ChipTone.Idle);
				return;
			}
			if (Interlocked.Exchange(ref devicesChanged, 0) != 0) RefreshDevices();
			if (isPolling || isCommandRunning)
				return;
			isPolling = true;
			bool wasConnected = client != null;
			try
			{
				if (client == null)
				{
					// About to (re)establish the pipe: queue a one-time push of the saved input settings
					// below, so the live bridge matches the sliders even if it launched at a different value.
					pushInputSettingsOnConnect = true;
					// Steam's launcher can briefly run a second Rocksmith2014.exe in the same folder that never
					// hosts the control pipe. Prefer whichever matching process is actually serving its pipe so
					// we connect reliably instead of latching onto the wrong one and reporting "not connected".
					AudioControlClient fallback = null;
					foreach (var process in Process.GetProcessesByName("Rocksmith2014"))
					{
						using (process)
						{
							if (!string.Equals(Path.GetDirectoryName(process.MainModule.FileName), gameDirectory.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
								continue;
							if (File.Exists(@"\\.\pipe\RSModsPlus.Audio." + process.Id))
							{
								client = new AudioControlClient(process.Id);
								break;
							}
							if (fallback == null)
								fallback = new AudioControlClient(process.Id);
						}
					}
					if (client == null)
						client = fallback;
				}
				if (client == null)
					throw new IOException("Launch Rocksmith with shared audio enabled.");
				latestStatus = await client.SendAsync(1);
				if (pushInputSettingsOnConnect)
				{
					pushInputSettingsOnConnect = false;
					await client.SendAsync(27, ReadBridgeSetting(RSMods.ReadSettings.AudioDiagnosticsOverlayIdentifier, "on") == "off" ? "0" : "1");
					// Reconcile the live bridge with the saved sliders. Only fill a value the user is not
					// mid-edit on, so a fresh change is never clobbered by the persisted setting.
					if (!pendingInputGainTenths.HasValue) pendingInputGainTenths = persistedInputGainTenths;
					if (!pendingNoiseGateTenths.HasValue) pendingNoiseGateTenths = persistedNoiseGateTenths;
					if (!pendingCompressor.HasValue) pendingCompressor = persistedCompressor;
					if (!pendingRocksmithGate.HasValue) pendingRocksmithGate = (persistedRgOverride, persistedRgTenths);
					if (!pendingHumFilter.HasValue) pendingHumFilter = persistedHumFilter;
				}
				// The bridge hosts its control pipe even when it is not routing audio (passthrough),
				// so a live connection no longer implies an owned output. In that state there is no
				// buffer to query and operation 5 carries no engine details, so skip it below.
				// A live route is still the passthrough world (the proxy owns the ASIO chain, no legacy WASAPI
				// session), so treat it as passthrough for buffer/gating purposes but remember the routed device.
				// A stalled route ("(route-stalled)<id>") is still a route for buffer/gating purposes, but its
				// source is dead. Check the stalled prefix first: it does not start with the plain RoutePrefix.
				UpdateRouteState(latestStatus.EndpointId);
				isSilentProxy = latestStatus.EndpointId == SilentEndpoint;
				isStartingProxy = latestStatus.EndpointId == StartingEndpoint;
				isDowngradingProxy = latestStatus.EndpointId == DowngradingEndpoint;
				isPassthrough = isRouting || latestStatus.EndpointId == PassthroughEndpoint || isSilentProxy || isStartingProxy || isDowngradingProxy;
				if (latestStatus.OutputError < 0 || isPassthrough)
				{
					bufferSnapshot = null;
				}
				if (latestStatus.OutputError >= 0 && !isPassthrough && (bufferSnapshot == null || IsHousekeepingTick))
				{
					var bufferStatus = await client.SendAsync(5);
					UpdateAsioBuffer(bufferStatus);
					if (bufferStatus.OutputError < 0)
					{
						latestStatus.OutputError = bufferStatus.OutputError;
						bufferSnapshot = null;
					}
					else
					{
						var snapshot = AudioBufferSnapshot.Parse(bufferStatus);
						if (bufferSnapshot == null || bufferSnapshot.Endpoint != snapshot.Endpoint)
						{
							customBuffer.Minimum = 1;
							customBuffer.Maximum = snapshot.Maximum;
							customBuffer.Minimum = snapshot.Minimum;
							customBuffer.Increment = snapshot.Fundamental;
							customBuffer.Value = snapshot.Period;
							customBufferCheck.Checked = ReadBufferSetting(snapshot.Endpoint, "PeriodFrames").Length > 0;
						}
						bufferSnapshot = snapshot;
					}
				}
				if (IsDisposed)
					return;
				SetPollInterval(ConnectedPollMilliseconds);
				bool broken = latestStatus.OutputError < 0;
				connectionChip.Set(broken ? "Connected · playback device unavailable" : "Connected", broken ? ChipTone.Bad : ChipTone.Good);
				UpdateBridgeChip(broken);
				elapsedLabel.Text = StudioFormat.Timecode(TimeSpan.FromSeconds(latestStatus.Frames / 48000.0));
				meter.SetLevel(latestStatus.Peak);
				// The Master strip is the output master, so show the proxy's measured output level there (peak of
				// L/R, 0..1 -> 0..1000). Falls back to the input peak when the proxy meter reports nothing.
				float outLevel = latestStatus.OutputPeak != null && latestStatus.OutputPeak.Length >= 2
					? Math.Max(latestStatus.OutputPeak[0], latestStatus.OutputPeak[1]) : 0f;
				Strip(MixerChannel.Master).SetLevel(outLevel > 0f ? (int)(outLevel * 1000f) : latestStatus.Peak);
				if (latestStatus.Peak > 0)
					lastSignal = DateTime.UtcNow;
				if (latestStatus.RecordingError < 0)
					SetStatus("Recording stopped with error 0x" + latestStatus.RecordingError.ToString("X8") + ". Press Stop & save to finalize the take.", ChipTone.Bad);
				else if (latestStatus.IsRecording && latestStatus.IsDryRecording && !latestStatus.IsDryInputReady)
					SetStatus("Dry guitar input is no longer arriving. Stop & save this take and check Player 1's input.", ChipTone.Warn);
				else if (latestStatus.IsRecording && !latestStatus.IsDryRecording && (DateTime.UtcNow - lastSignal).TotalSeconds > 4)
					SetStatus("No sound is reaching the output. Check the device and the in-game volume before you play on.", ChipTone.Warn);
			}
			catch (Exception error)
			{
				if (IsDisposed)
					return;
				client = null;
				latestStatus = null;
				bufferSnapshot = null;
				isPassthrough = false;
				isSilentProxy = false;
				isStartingProxy = false;
				isDowngradingProxy = false;
				isRouting = false;
				isVirtualRoute = false;
				routedEndpointId = "";
				routeSourceStalled = false;
				SetPollInterval(SearchingPollMilliseconds);
				connectionChip.Set(Summarize(error), ChipTone.Idle);
				bridgeChip.Visible = false;
				if (error is NotSupportedException) SetStatus(error.Message, ChipTone.Bad);
				meter.Reset();
				Strip(MixerChannel.Master).ResetLevel();
			}
			finally
			{
				isPolling = false;
				if (!IsDisposed)
				{
					ticks++;
					// The slow reads ride along with the housekeeping tick, or a connection change: a game that
					// just appeared or vanished changes what the Setup tab may do right now.
					if (IsHousekeepingTick || wasConnected != (client != null))
					{
						takes.Load(recordingDirectory.Text);
						RefreshEnvironment();
					}
					UpdateState();
				}
			}
		}

		/// <summary>True on the polls that also carry the slower housekeeping, whatever the current poll rate.</summary>
		private bool IsHousekeepingTick => ticks % Math.Max(1, HousekeepingMilliseconds / statusTimer.Interval) == 0;

		/// <summary>
		/// Polls fast while connected so the meters read as live, and slowly while searching, when
		/// every tick also walks the process list. Changing a running timer's interval restarts it,
		/// so the interval is only touched when it actually differs.
		/// </summary>
		private void SetPollInterval(int milliseconds)
		{
			if (statusTimer.Interval != milliseconds)
				statusTimer.Interval = milliseconds;
		}

		private async void ApplyMixer(object sender, EventArgs args)
		{
			if (!masterEnabled) return;
			if (isCommandRunning || isPolling || client == null || latestStatus == null) return;
			bool hasPendingVolume = false;
			foreach (var volume in pendingVolumes) hasPendingVolume |= volume.HasValue;
			bool hasPendingGain = pendingInputGainTenths.HasValue;
			bool hasPendingGate = pendingNoiseGateTenths.HasValue;
			bool hasPendingCompressor = pendingCompressor.HasValue;
			bool hasPendingLimiter = pendingLimiter != null;
			bool hasPendingRg = pendingRocksmithGate.HasValue;
			bool hasPendingHum = pendingHumFilter.HasValue;
			if (!hasPendingVolume && !hasPendingGain && !hasPendingGate && !hasPendingCompressor && !hasPendingLimiter && !hasPendingRg && !hasPendingHum) return;
			isCommandRunning = true;
			isApplyingMixer = true;
			var requestedVolumes = (int?[])pendingVolumes.Clone();
			int? requestedGain = pendingInputGainTenths;
			int? requestedGate = pendingNoiseGateTenths;
			int? requestedCompressor = pendingCompressor;
			string requestedLimiter = pendingLimiter;
			(bool on, int tenths)? requestedRg = pendingRocksmithGate;
			int? requestedHum = pendingHumFilter;
			Array.Clear(pendingVolumes, 0, pendingVolumes.Length);
			pendingInputGainTenths = null;
			pendingNoiseGateTenths = null;
			pendingCompressor = null;
			pendingLimiter = null;
			pendingRocksmithGate = null;
			pendingHumFilter = null;
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
				// Operation 15 = guitar input make-up gain, sent as tenths of a dB (matches RSMods.ini).
				if (requestedGain.HasValue)
					latestStatus = await client.SendAsync(15, requestedGain.Value.ToString(System.Globalization.CultureInfo.InvariantCulture));
				// Operation 19 = guitar input noise gate threshold, signed tenths of a dB (0 = off).
				if (requestedGate.HasValue)
					latestStatus = await client.SendAsync(19, requestedGate.Value.ToString(System.Globalization.CultureInfo.InvariantCulture));
				// Operation 20 = guitar input compressor strength, integer 0-100 (0 = off).
				if (requestedCompressor.HasValue)
					latestStatus = await client.SendAsync(20, requestedCompressor.Value.ToString(System.Globalization.CultureInfo.InvariantCulture));
				// Operation 16 = output level trim, "<gain>,<reserved>" linear (proxy driver).
				if (requestedLimiter != null)
					latestStatus = await client.SendAsync(16, requestedLimiter);
				// Operation 23 = Rocksmith gate override, "<on>,<tenths>" (on 0/1; tenths = P1_NoiseFloor).
				if (requestedRg.HasValue)
					latestStatus = await client.SendAsync(23, (requestedRg.Value.on ? "1," : "0,") + requestedRg.Value.tenths.ToString(System.Globalization.CultureInfo.InvariantCulture));
				// Operation 24 = mains-hum notch base frequency (0 = off, else 50 or 60).
				if (requestedHum.HasValue)
					latestStatus = await client.SendAsync(24, requestedHum.Value.ToString(System.Globalization.CultureInfo.InvariantCulture));
			}
			catch (Exception error)
			{
				// Keep every request that was not replaced by a newer edit. The pipe may fail during a
				// device/output refresh, and these values must be retried after the next connection.
				for (int channel = 0; channel < requestedVolumes.Length; channel++)
				{
					if (requestedVolumes[channel].HasValue && !pendingVolumes[channel].HasValue)
						pendingVolumes[channel] = requestedVolumes[channel];
				}
				if (requestedGain.HasValue && !pendingInputGainTenths.HasValue) pendingInputGainTenths = requestedGain;
				if (requestedGate.HasValue && !pendingNoiseGateTenths.HasValue) pendingNoiseGateTenths = requestedGate;
				if (requestedCompressor.HasValue && !pendingCompressor.HasValue) pendingCompressor = requestedCompressor;
				if (requestedLimiter != null && pendingLimiter == null) pendingLimiter = requestedLimiter;
				if (requestedRg.HasValue && !pendingRocksmithGate.HasValue) pendingRocksmithGate = requestedRg;
				if (requestedHum.HasValue && !pendingHumFilter.HasValue) pendingHumFilter = requestedHum;
				client = null;
				latestStatus = null;
				if (!IsDisposed) SetStatus("Live audio controls: " + error.Message, ChipTone.Bad);
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
					if (latestStatus != null) pendingVolumes[channel] = null;
					fader.SetUnavailable();
					unavailable = true;
					continue;
				}
				fader.Enabled = !isCommandRunning || isApplyingMixer;
				bool justEdited = (DateTime.UtcNow - lastMixerEdit).TotalMilliseconds < 750;
				if (!isApplyingMixer && !fader.IsAdjusting && !justEdited && !pendingVolumes[channel].HasValue) fader.SetVolume(volume);
			}
			mixerHint.Text = latestStatus == null ? "Start Rocksmith to adjust playback volumes."
				: unavailable ? "Some playback channels are not available in Rocksmith right now."
				: "Playback volume only. The guitar input and note detection are unchanged.";
		}

		private static string Summarize(Exception error)
		{
			if (error is NotSupportedException) return "Audio bridge update required";
			return error is IOException || error is TimeoutException ? "Rocksmith not connected" : "Connection error";
		}

		private async void Record(object sender, EventArgs args)
		{
			if (!masterEnabled) return;
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
				latestStatus = await client.SendAsync(recordingSource.SelectedIndex == 1 ? 14u : 2u, Path.GetFullPath(recordingDirectory.Text));
				SetStatus("Recording " + (latestStatus.IsDryRecording ? "dry guitar" : "tone + song") + (video != null ? " and video" : "") + " · press Stop & save to finish this take.", ChipTone.Info);
			}
			catch (Exception error) { SetRecordingError(error); video?.Dispose(); video = null; }
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
			catch (Exception error) { SetRecordingError(error); }
			finally { video?.Dispose(); video = null; isCommandRunning = false; UpdateState(); }
		}

		private void SetRecordingError(Exception error)
		{
			string retained = video?.RetainedVideoPath;
			SetStatus(error.Message + (string.IsNullOrEmpty(retained) ? "" : " Video retained at " + retained), ChipTone.Bad);
		}

		private async void ApplyBuffer(object sender, EventArgs args)
		{
			if (!masterEnabled) return;
			if (!applyBufferButton.Enabled || client == null || bufferSnapshot == null) return;
			var selectedBuffer = bufferSnapshot;
			bool custom = customBufferCheck.Checked;
			uint frames = custom ? (uint)customBuffer.Value : 0;
			if (custom && (frames < selectedBuffer.Minimum || frames > selectedBuffer.Maximum || frames % selectedBuffer.Fundamental != 0))
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
				var deadline = DateTime.UtcNow.AddSeconds(2);
				var status = await client.SendAsync(5);
				while (status.OutputError == unchecked((int)0x8000000A) && DateTime.UtcNow < deadline)
				{
					await Task.Delay(50);
					status = await client.SendAsync(5);
				}
				if (status.OutputError < 0) throw new IOException("Output buffer request is pending or the selected device is unavailable; it has not been saved.");
				bufferSnapshot = AudioBufferSnapshot.Parse(status);
				if ((custom && bufferSnapshot.Period != frames) || bufferSnapshot.Endpoint != selectedBuffer.Endpoint)
					throw new IOException("Rocksmith did not confirm the requested output buffer.");
				if (!WritePrivateProfileString("Output buffer " + selectedBuffer.Endpoint, "PeriodFrames", custom ? frames.ToString(System.Globalization.CultureInfo.InvariantCulture) : null, settingsPath)
					|| !WritePrivateProfileString("Output buffer " + selectedBuffer.Endpoint, "Mode", custom ? "Custom" : "Automatic", settingsPath))
					throw new IOException("The buffer is active, but could not be saved for the next launch.");
				SetStatus((custom ? "Custom" : "Automatic") + " buffer applied and saved: " + bufferSnapshot.Period + " frames (" + (bufferSnapshot.Period / 48.0).ToString("0.##") + " ms).", ChipTone.Good);
			}
			catch (Exception error) { SetStatus(error.Message, ChipTone.Bad); }
			finally { isCommandRunning = false; UpdateState(); }
		}

		private async void MeasureLatency(object sender, EventArgs args)
		{
			if (!measureLatencyButton.Enabled || client == null || latestStatus == null)
				return;
			if (latestStatus.OutputError < 0)
			{
				SetStatus("Latency measurement is unavailable while playback output is unavailable.", ChipTone.Bad);
				return;
			}

			var activeClient = client;
			isCommandRunning = true;
			latencyReadout.Text = "Round-trip latency: measuring...";
			UpdateState();
			try
			{
				while (isPolling) await Task.Delay(25);
				await activeClient.SendAsync(17);
				var deadline = DateTime.UtcNow.AddSeconds(5);
				AudioControlStatus result;
				do
				{
					await Task.Delay(100);
					result = await activeClient.SendAsync(18);
					if (client == null || client.ProcessId != activeClient.ProcessId || DateTime.UtcNow >= deadline)
						break;
				}
				while (string.Equals(result.FilePath, "measuring", StringComparison.OrdinalIgnoreCase));

				if (string.Equals(result.FilePath, "measuring", StringComparison.OrdinalIgnoreCase))
					throw new TimeoutException("The latency measurement did not finish. Check the physical output-to-input loopback and try again.");
				if (!TryParseLatencyResult(result.FilePath, out var milliseconds, out var confidence, out bool hasSignal))
					throw new IOException("The audio bridge returned an invalid latency result.");
				latencyReadout.Text = hasSignal
					? "Round-trip latency: " + milliseconds.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture) + " ms · confidence " + confidence.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture)
					: "Round-trip latency: no signal · confidence " + confidence.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture);
				SetStatus(hasSignal ? "Round-trip latency measured." : "Latency probe returned no signal. Check the physical loopback and input selection.", hasSignal ? ChipTone.Good : ChipTone.Warn);
			}
			catch (Exception error)
			{
				latencyReadout.Text = "Round-trip latency: measurement failed.";
				SetStatus("Could not measure round-trip latency: " + error.Message, ChipTone.Bad);
			}
			finally
			{
				isCommandRunning = false;
				UpdateState();
			}
		}

		private static bool TryParseLatencyResult(string value, out double milliseconds, out double confidence, out bool hasSignal)
		{
			milliseconds = 0;
			confidence = 0;
			hasSignal = false;
			if (string.IsNullOrWhiteSpace(value)) return false;
			string[] fields = value.Split(';');
			if (fields.Length != 2) return false;
			string[] measurement = fields[0].Split('=');
			string[] confidenceField = fields[1].Split('=');
			if (confidenceField.Length != 2 || !string.Equals(confidenceField[0], "conf", StringComparison.OrdinalIgnoreCase)
				|| !double.TryParse(confidenceField[1], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out confidence))
				return false;
			if (measurement.Length == 2 && string.Equals(measurement[0], "ms", StringComparison.OrdinalIgnoreCase))
			{
				if (!double.TryParse(measurement[1], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out milliseconds)) return false;
				hasSignal = true;
				return true;
			}
			return measurement.Length == 1 && string.Equals(measurement[0], "no_signal", StringComparison.OrdinalIgnoreCase);
		}

		private async void ApplyOutput(object sender, EventArgs args)
		{
			if (!masterEnabled) return;
			if (isCommandRunning) return;
			RefreshDevices();
			if (isCommandRunning || !(outputSelector.SelectedItem is AudioDeviceChoice output))
				return;
			isCommandRunning = true;
			RefreshEnvironment();   // an apply decides on fresh facts, not the housekeeping snapshot
			UpdateState();
			// The device is the choice; the transport is automatic. If the chosen device has an ASIO driver we
			// promote to ASIO through the Rocksmith Audio Bridge proxy (best latency, and it is the only path
			// that can record the game mix). Otherwise we fall back to the WASAPI bridge. This is why picking an
			// ASIO-capable device no longer forces the WASAPI downgrade the old "Apply output" always did.
			AudioInputMode mode = inputMode.ReadMode();
			string asioDriver = mode == AudioInputMode.Asio ? FindMatchingAsioDriver(output.Name) : null;

			// Live, no-restart moves in the proxy world: with the game up and the proxy owning the ASIO chain,
			// output can be redirected to another device (or handed back to the ASIO device) at runtime instead
			// of demanding a relaunch. Only these two cases are handled here; anything else falls through to the
			// arm/install flow below unchanged.
			// The op-21/22 route path only exists in ASIO mode, when the proxy is linked into the game's output
			// chain. Cable mode disables RS_ASIO and uses the permanent native output session below instead.
			bool proxyChainLive = AsioProxySetup.IsLinked(gameDirectory);
			var live = OutputApplyPlan.Plan(mode == AudioInputMode.Asio, inputMode.IsGameRunning(), client != null, proxyChainLive,
				isPassthrough, isRouting, asioDriver != null, output.Id == routedEndpointId,
				latestStatus != null && (latestStatus.EndpointId == PassthroughEndpoint || isSilentProxy || isStartingProxy || isVirtualRoute));
			if (live != OutputApplyAction.UseExistingFlow)
			{
				try
				{
					if (live == OutputApplyAction.PromoteToAsio)
					{
						AsioProxySetup.SelectTarget(asioDriver);
						latestStatus = await client.SendAsync(25, asioDriver);
						if (latestStatus.OutputError < 0) throw new IOException("The selected ASIO output could not be opened.");
						if (isRouting) latestStatus = await client.SendAsync(22);
						AsioProxySetup.SetPreferRealOutput(true);
						WriteSetting("OutputDevice", output.Id);
						SetStatus("Now playing through " + output.Name + " (ASIO). No restart needed.", ChipTone.Good);
					}
					else if (live == OutputApplyAction.RouteToDevice)
					{
						// Op 21 = route to a WASAPI device (op 19 is the input noise gate; 20 reserved).
						latestStatus = await client.SendAsync(21, output.Id);
						AsioProxySetup.SetPreferRealOutput(false);
						WriteSetting("OutputDevice", output.Id);
						SetStatus("Now playing through " + output.Name + " · no restart. Pick your ASIO device and press Apply output to switch back.", ChipTone.Good);
					}
					else if (live == OutputApplyAction.RebindAsio)
					{
						AsioProxySetup.SelectTarget(asioDriver);
						latestStatus = await client.SendAsync(25, asioDriver);
						if (latestStatus.OutputError < 0) throw new IOException("The ASIO device could not be re-opened.");
						AsioProxySetup.SetPreferRealOutput(true);
						WriteSetting("OutputDevice", output.Id);
						SetStatus("Re-opened " + output.Name + " (ASIO). No restart needed.", ChipTone.Good);
					}
					else // StopRoute
					{
						latestStatus = await client.SendAsync(22);
						AsioProxySetup.SetPreferRealOutput(true);
						WriteSetting("OutputDevice", output.Id);
						SetStatus("Back on " + output.Name + " (ASIO). No restart needed.", ChipTone.Good);
					}
					isShowingPlaybackStatus = false;
				}
				catch (Exception error) { SetStatus(error.Message, ChipTone.Bad); }
				finally { isCommandRunning = false; UpdateState(); }
				return;
			}

			if (asioDriver != null)
			{
				try
				{
					if (inputMode.IsGameRunning())
					{
						SetStatus("Close Rocksmith to change the ASIO output; RS_ASIO reads it at launch.", ChipTone.Warn);
						return;
					}
					if (!AsioProxySetup.IsProxyRegistered())
						AsioProxySetup.Register(Path.Combine(gameDirectory, "RocksmithAudioBridge.dll"));
					// Point RS_ASIO at the proxy for output and for every input on the same interface. The shared
					// host is what lets an interface that is absent at launch be adopted for input later: the
					// proxy boots virtually, RS_ASIO keeps its capture device, and the game binds the real input
					// when it arrives. Output-only linking leaves RS_ASIO's raw input lookup failing for the session.
					AsioProxySetup.Link(gameDirectory, asioDriver);
					WriteSetting("OutputDevice", output.Id);
					RefreshEnvironment();
					UpdateOutputHint();
					// The ASIO output is bound when Rocksmith starts (RS_ASIO reads its ini at launch), so this
					// one-time setup needs the game to (re)start. After that, recording and the mixer are runtime.
					SetStatus(output.Name + " set as your ASIO output (one-time setup). Start Rocksmith and the bridge is live; after this, recording, the mixer and output moves never need a restart.", ChipTone.Good);
				}
				catch (Exception error) { SetStatus("Could not set up the audio bridge driver: " + error.Message, ChipTone.Bad); }
				finally { isCommandRunning = false; UpdateState(); }
				return;
			}

			// Once RS_ASIO is linked to the permanent proxy, a plain Windows endpoint is its shared fallback,
			// not the legacy RS_ASIO-WASAPI configuration. Save the route intent without touching RS_ASIO.ini;
			// the proxy adopts it automatically on the next launch and the live op-21 path handles a running game.
			if (!inputMode.IsGameRunning() && AsioProxySetup.IsLinked(gameDirectory))
			{
				try
				{
					WriteSetting("OutputDevice", output.Id);
					WriteSetting("Enabled", "0");
					AsioProxySetup.SetPreferRealOutput(false);
					SetStatus(output.Name + " selected as the proxy's shared output. Start Rocksmith when ready.", ChipTone.Good);
				}
				catch (Exception error) { SetStatus("Could not save the proxy output: " + error.Message, ChipTone.Bad); }
				finally { isCommandRunning = false; UpdateState(); }
				return;
			}

			while (isPolling)
				await Task.Delay(15);
			try
			{
				if (client != null && !isPassthrough)
				{
					latestStatus = await client.SendAsync(4, output.Id);
					WriteSetting("OutputDevice", output.Id);
					isShowingPlaybackStatus = true;
					UpdatePlaybackStatus();
				}
				else
				{
					// The pipe is hosted even in passthrough, so a live connection does not mean the bridge is
					// routing. Enabling routing rewrites config that is read at launch, so it must be armed while
					// the game is closed; EnableBridge reports exactly that when Rocksmith is already running.
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
				RefreshDiskSpace();
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

		private void UpdateAsioBuffer(AudioControlStatus status)
		{
			uint frames = 0;
			uint sampleRate = 0;
			bool hasFrames = false;
			bool hasRate = false;
			foreach (string field in status.FilePath.Split(' '))
			{
				if (field.StartsWith("asioInputFrames=", StringComparison.Ordinal))
					hasFrames = uint.TryParse(field.Substring("asioInputFrames=".Length), out frames);
				if (field.StartsWith("asioInputRate=", StringComparison.Ordinal))
					hasRate = uint.TryParse(field.Substring("asioInputRate=".Length), out sampleRate);
			}
			if (!hasFrames || !hasRate)
			{
				asioBufferLabel.Text = "ASIO input buffer unavailable: use matching updated settings and game DLL builds.";
				return;
			}
			asioBufferLabel.Text = frames == 0 || sampleRate == 0
				? "ASIO input buffer: unavailable (no active Player 1 ASIO input)."
				: "ASIO input (Player 1): " + frames + " frames · " + (frames * 1000.0 / sampleRate).ToString("0.##")
					+ " ms at " + (sampleRate / 1000.0).ToString("0.##") + " kHz";
		}

		private void UpdateState()
		{
			if (nav != null) nav.Enabled = masterEnabled;
			foreach (var page in pages) if (page != null) page.Enabled = masterEnabled;
			masterPower.Enabled = !isCommandRunning && !isGameRunning;
			string masterPowerTip = isGameRunning
				? "Close Rocksmith before turning the audio bridge on or off."
				: "Turns the audio bridge on or off. Note by Note is unaffected.";
			SetTip(masterPower, masterPowerTip);
			SetTip(masterPowerHost, masterPowerTip);
			asioBufferLabel.Visible = inputSelector.SelectedIndex == 1;
			if (latestStatus == null)
				asioBufferLabel.Text = "ASIO input buffer: available once Rocksmith connects.";
			SynchronizePlaybackState();
			UpdateMixer();
			UpdatePlaybackStatus();
			bool recording = latestStatus?.IsRecording == true;
			customBuffer.Visible = customBufferCheck.Checked;
			customBufferCheck.Enabled = !isCommandRunning;
			bool canSetBuffer = client != null && bufferSnapshot != null && !isCommandRunning && !recording && video == null
				&& (outputSelector.SelectedItem as AudioDeviceChoice)?.Id == bufferSnapshot.Endpoint;
			customBuffer.Enabled = applyBufferButton.Enabled = canSetBuffer;
			bool canMeasureLatency = client != null && latestStatus != null && latestStatus.OutputError >= 0
				&& !isCommandRunning && !recording && video == null;
			measureLatencyButton.Enabled = canMeasureLatency;
			currentBufferLabel.Text = bufferSnapshot == null ? DescribeUnavailableBufferState()
				: "Windows playback buffer: " + bufferSnapshot.Period + " frames · " + (bufferSnapshot.Period / 48.0).ToString("0.##") + " ms at 48 kHz · " + DescribeBufferMode() + "\n"
				+ (bufferSnapshot.Minimum == bufferSnapshot.Maximum ? "The device supports only this period for the current format."
					: "The device supports " + bufferSnapshot.Minimum + " to " + bufferSnapshot.Maximum + " frames, in steps of " + bufferSnapshot.Fundamental + ".");
			bool wantsVideo = captureMode.SelectedIndex == 1;
			bool captureReady = WindowCaptureRecorder.Supported;
			string blocker = DescribeBlocker(recording, wantsVideo, captureReady);
			recordButton.Enabled = blocker == null && !isCommandRunning && !recording && video == null;
			stopButton.Enabled = !isCommandRunning && (recording || video != null || latestStatus?.RecordingError < 0);
			applyButton.Enabled = masterEnabled && !isCommandRunning && outputSelector.SelectedItem is AudioDeviceChoice;
			// Hide the "Remove audio bridge driver" back-out while the game is running (it can't take effect until
			// relaunch), and bring it back once the game closes. Uses the cached install state so the poll tick
			// stays off the registry.
			removeProxyButton.Visible = proxyInstalled && !isGameRunning;
			outputSelector.Enabled = !isCommandRunning;
			captureMode.Enabled = !recording && !isCommandRunning && video == null;
			recordingSource.Enabled = captureMode.Enabled;
			// Output logging is read when Rocksmith opens its output stream, so changing it while the
			// game is running cannot affect the current session. Keep the live overlay independent.
			monitorOutputCheck.Enabled = !isGameRunning && !isCommandRunning;
			recordingChip.Visible = recording || video != null;
			recordingChip.Tone = ChipTone.Bad;
			elapsedLabel.ForeColor = recording ? StudioTheme.Record : StudioTheme.Ink;
			deckHint.Text = recording
				? "Take running · " + (latestStatus.IsDryRecording ? "dry guitar" : "game mix") + (wantsVideo ? " and video" : "") + " · " + recordingDirectory.Text
				: blocker ?? "Ready. Press Record, or " + RecordingHotkey + " in the game, to start a take.";
			deckHint.ForeColor = blocker == null || recording ? StudioTheme.Muted : StudioTheme.Warning;
			captureChip.Visible = wantsVideo;
			captureChip.Set(captureReady ? "Window capture ready" : "Window capture unavailable", captureReady ? ChipTone.Good : ChipTone.Bad);
			UpdateDisk();
			UpdateInputMode();
			UpdateTakeButtons();
			libraryLabel.Text = takes.Summary;
			SetTip(recordingDirectory, recordingDirectory.Text);
		}

		private string DescribeUnavailableBufferState()
		{
			if (latestStatus == null) return "Windows playback buffer: available once Rocksmith connects.";
			if (isSilentProxy) return "Output is silent. Buffer tuning becomes available once a Windows device is routed.";
			if (isStartingProxy) return "The audio bridge driver is starting.";
			if (isDowngradingProxy) return "The ASIO device stopped responding; falling back to the Windows output.";
			if (isRouting) return "Windows output is live. Select the routed device above to inspect or change its buffer.";
			if (latestStatus.EndpointId == PassthroughEndpoint) return "ASIO output is live; the Windows playback buffer does not apply.";
			return "Playback device unavailable. Reconnect it, or select another output and press Apply output.";
		}

		private void UpdateBridgeChip(bool playbackBroken)
		{
			bridgeChip.Visible = true;
			if (isSilentProxy)
				bridgeChip.Set("Bridge ready · silent", ChipTone.Warn);
			else if (isStartingProxy)
				bridgeChip.Set("Bridge starting", ChipTone.Idle);
			else if (isDowngradingProxy || routeSourceStalled)
				bridgeChip.Set("Bridge recovering", ChipTone.Warn);
			else
			{
				bool bridgeOn = latestStatus != null && !playbackBroken && (isRouting || !isPassthrough);
				bridgeChip.Set(bridgeOn ? "Bridge on" : "Bridge off", bridgeOn ? ChipTone.Good : ChipTone.Idle);
			}
		}

		private string DescribeBlocker(bool recording, bool wantsVideo, bool captureReady)
		{
			if (client == null)
				return "Rocksmith is not connected. Pick an output on the Setup tab, press Apply output, then launch the game.";
			if (recording || video != null)
				return null;
			if (latestStatus?.RecordingError < 0)
				return "The last take ended with an error. Press Stop & save to close it before recording again.";
			if (!Path.IsPathRooted(recordingDirectory.Text))
				return "Choose a full folder path for takes before recording.";
			if (recordingSource.SelectedIndex == 1 && latestStatus?.IsDryInputReady != true)
				return "Dry recording needs an active Player 1 guitar input at 48 kHz.";
			// Game-mix (wet) recording on an ASIO output only captures through the Rocksmith Audio Bridge proxy.
			// A plain WASAPI output records the mix via the render tap and needs no driver, so gate only the
			// ASIO-without-driver case and point the user at the one action that installs it.
			if (recordingSource.SelectedIndex == 0
				&& cachedMode == AudioInputMode.Asio
				&& outputSelector.SelectedItem is AudioDeviceChoice wetOut
				&& FindMatchingAsioDriver(wetOut.Name) != null
				&& !cachedProxyRegistered)
				return "Game-mix recording on ASIO needs the audio bridge driver. On the Setup tab, pick your device and press Apply output to install it.";
			if (wantsVideo && !captureReady)
				return WindowCaptureRecorder.Requirement;
			return null;
		}

		private void UpdateDisk()
		{
			long free = cachedFreeSpace;
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
				if (cachedModeError != null) throw new InvalidOperationException(cachedModeError);
				var mode = cachedMode;
				syncingInputMode = true;
				inputSelector.SelectedIndex = mode == AudioInputMode.Asio ? 1 : 0;
				syncingInputMode = false;
				inputSelector.Enabled = !isGameRunning && !isCommandRunning && mode != AudioInputMode.Unavailable;
				cableForPlayerTwoCheck.Enabled = !isCommandRunning && (!isGameRunning || client != null);
				if (mode == AudioInputMode.Asio && latestStatus?.ProxyInputMode == 2)
					inputChip.Set("Tone Cable fallback", ChipTone.Good);
				else if (mode == AudioInputMode.Asio && latestStatus?.ProxyInputMode == 1)
					inputChip.Set("Waiting for Tone Cable", ChipTone.Warn);
				else
					inputChip.Set(mode == AudioInputMode.Asio ? "ASIO input" : mode == AudioInputMode.Cable ? "Cable input" : "RS_ASIO not installed",
						mode == AudioInputMode.Unavailable ? ChipTone.Warn : ChipTone.Info);
				// The event-driven option only affects the plain cable path, so hide it under ASIO
				// (RS_ASIO owns capture there) rather than showing an inert control.
				eventDrivenCaptureCheck.Visible = mode == AudioInputMode.Cable;
				eventDrivenCaptureHint.Visible = mode == AudioInputMode.Cable;
				cableForPlayerTwoCheck.Visible = mode == AudioInputMode.Asio;
				cableForPlayerTwoHint.Visible = mode == AudioInputMode.Asio;
				inputModeLabel.Text = mode == AudioInputMode.Unavailable
					? "Install RS_ASIO in the Rocksmith folder to unlock the ASIO option."
					: isGameRunning ? "Close Rocksmith to change the input mode." : "Takes effect the next time Rocksmith starts.";
				if (ReadSetting("Enabled", "0") == "1")
				{
					inputModeLabel.Text += mode == AudioInputMode.Asio
						? " ASIO supplies the guitar input; playback goes to the output device chosen on the left."
						: " The bridge supplies the cable input and plays through the output device chosen on the left.";
				}
			}
			catch (Exception error)
			{
				syncingInputMode = false;
				syncingCableForPlayerTwo = false;
				inputSelector.Enabled = false;
				eventDrivenCaptureCheck.Visible = false;
				eventDrivenCaptureHint.Visible = false;
				cableForPlayerTwoCheck.Visible = false;
				cableForPlayerTwoHint.Visible = false;
				inputChip.Set("Input files conflict", ChipTone.Bad);
				inputModeLabel.Text = error.Message;
			}
		}

		private void SaveEventDrivenCapture()
		{
			try
			{
				WriteModernCableInput(eventDrivenCaptureCheck.Checked);
				SetStatus("Event-driven cable capture " + (eventDrivenCaptureCheck.Checked ? "on" : "off") + ". Restart Rocksmith to apply.", ChipTone.Good);
			}
			catch (Exception error)
			{
				SetStatus("Could not save the cable capture setting: " + error.Message, ChipTone.Bad);
			}
		}

		private async void SaveCableForPlayerTwo()
		{
			bool enabled = cableForPlayerTwoCheck.Checked;
			bool previous = RSMods.ReadSettings.ProcessSettings(RSMods.ReadSettings.CableForPlayerTwoIdentifier) == "on";
			isCommandRunning = true;
			UpdateState();
			try
			{
				WriteModSetting(RSMods.ReadSettings.CableForPlayerTwoIdentifier, enabled);
				if (isGameRunning)
				{
					if (client == null) throw new InvalidOperationException("The running game is not connected to the Audio bridge.");
					await client.SendAsync(26, enabled ? "1" : "0");
				}
				SetStatus(enabled
					? isGameRunning ? "Player 2 now hears the Real Tone Cable." : "Player 2 will hear the Real Tone Cable when Rocksmith starts."
					: isGameRunning ? "Player 2 no longer hears the Real Tone Cable." : "Player 2 Real Tone Cable off.", ChipTone.Good);
			}
			catch (Exception error)
			{
				try { WriteModSetting(RSMods.ReadSettings.CableForPlayerTwoIdentifier, previous); }
				catch (Exception rollbackError) { error = new AggregateException(error, rollbackError); }
				syncingCableForPlayerTwo = true;
				cableForPlayerTwoCheck.Checked = previous;
				syncingCableForPlayerTwo = false;
				SetStatus("Could not save the Player 2 cable setting: " + error.Message, ChipTone.Bad);
			}
			finally
			{
				isCommandRunning = false;
				UpdateState();
			}
		}

		// The bridge writes only its own key and always keeps it in [Mod Settings], where the game DLL
		// reads it. This preserves every unrelated line without relying on the main window's settings table.
		private static void WriteModernCableInput(bool enabled)
		{
			WriteModSetting(RSMods.ReadSettings.ModernCableInputIdentifier, enabled);
		}

		private static void WriteModSetting(string identifier, bool enabled)
		{
			WriteModSetting(identifier, enabled ? "on" : "off");
		}

		private static void WriteModSetting(string identifier, string value)
		{
			// Every value the bridge writes must be on the shared bridge-owned list, or the main window's
			// next full save silently reverts it. Keep ReadSettings.BridgeOwnedIdentifiers in step.
			System.Diagnostics.Debug.Assert(Array.IndexOf(RSMods.ReadSettings.BridgeOwnedIdentifiers, identifier) >= 0,
				"Bridge wrote a setting that is not in ReadSettings.BridgeOwnedIdentifiers: " + identifier);
			WriteRsModsSetting(identifier, value, "[Mod Settings]");
		}

		private static void WriteRsModsSetting(string identifier, string value, string section)
		{
			string iniPath = Path.Combine(RSMods.Util.GenUtil.GetRSDirectory(), "RSMods.ini");
			string desired = identifier + value;
			var lines = File.Exists(iniPath) ? new List<string>(File.ReadAllLines(iniPath)) : new List<string>();
			lines.RemoveAll(line => line.StartsWith(identifier, StringComparison.Ordinal));
			int sectionStart = lines.FindIndex(line => string.Equals(line.Trim(), section, StringComparison.OrdinalIgnoreCase));
			if (sectionStart < 0)
			{
				if (lines.Count > 0 && lines[lines.Count - 1].Length > 0) lines.Add("");
				lines.Add(section);
				lines.Add(desired);
			}
			else
			{
				int sectionEnd = lines.FindIndex(sectionStart + 1, line => line.TrimStart().StartsWith("[", StringComparison.Ordinal));
				lines.Insert(sectionEnd < 0 ? lines.Count : sectionEnd, desired);
			}
			File.WriteAllLines(iniPath, lines);
		}

		// Slider and box both work in 0.1 dB (0..+20); RSMods.ini and the DLL speak tenths of a dB (0..200).
		private static int SliderToTenths(int value) => Math.Max(0, Math.Min(200, value));
		private static int TenthsToSlider(int tenths) => Math.Max(0, Math.Min(200, tenths));

		private static int ReadInputGainTenths()
		{
			string raw = RSMods.ReadSettings.ProcessSettings(RSMods.ReadSettings.AsioInputGainIdentifier);
			return int.TryParse(raw, out int tenths) ? Math.Max(0, Math.Min(200, tenths)) : 0;
		}

		// Set both controls from a value in tenths without them re-triggering each other (caller holds the sync flag).
		private void SetInputGain(int tenths)
		{
			int t = Math.Max(0, Math.Min(200, tenths));
			inputGainSlider.Value = t;
			gainInput.Value = Math.Max(gainInput.Minimum, Math.Min(gainInput.Maximum, t / 10m));
		}

		// Feature rows stay in place; switching a feature off greys its slider and box instead of hiding them.
		private void ApplyInputGainVisibility()
		{
			inputGainSlider.Enabled = gainInput.Enabled = inputGainEnableCheck.Checked;
		}

		private void OnInputGainChanged(object sender, EventArgs args)   // slider moved
		{
			if (syncingInputGain) return;
			syncingInputGain = true;
			gainInput.Value = Math.Max(gainInput.Minimum, Math.Min(gainInput.Maximum, inputGainSlider.Value / 10m));
			syncingInputGain = false;
			pendingInputGainTenths = inputGainSlider.Value;
			lastMixerEdit = DateTime.UtcNow;
		}

		private void OnGainInputChanged(object sender, EventArgs args)   // numeric typed/spun
		{
			if (syncingInputGain) return;
			syncingInputGain = true;
			inputGainSlider.Value = Math.Max(inputGainSlider.Minimum, Math.Min(inputGainSlider.Maximum, (int)Math.Round(gainInput.Value * 10m)));
			syncingInputGain = false;
			pendingInputGainTenths = inputGainSlider.Value;
			lastMixerEdit = DateTime.UtcNow;
			PersistInputGain();
		}

		private void PersistInputGain()
		{
			int tenths = SliderToTenths(inputGainSlider.Value);
			if (tenths == persistedInputGainTenths) return;
			try
			{
				WriteInputGain(tenths);
				persistedInputGainTenths = tenths;
				SetStatus("Input gain +" + (tenths / 10.0).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture) + " dB saved and applied. If you don't hear a change, restart Rocksmith (the input stage arms at launch).", ChipTone.Good);
			}
			catch (Exception error)
			{
				SetStatus("Could not save the input gain: " + error.Message, ChipTone.Bad);
			}
		}

		// Gate threshold is stored in tenths of a dBFS (0 = off, else negative). The slider works directly in tenths
		// (0.1 dB per step); the numeric box works in dB. Both are clamped to the slider/box ranges.
		private static int ReadGateThresholdTenths()
		{
			string raw = RSMods.ReadSettings.ProcessSettings(RSMods.ReadSettings.NoiseGateThresholdIdentifier);
			return int.TryParse(raw, out int tenths) ? Math.Max(-800, Math.Min(0, tenths)) : 0;
		}

		// The effective threshold to apply/persist: the box value in tenths when the gate is on, else 0 (off).
		private int GateEffectiveTenths() => gateEnableCheck.Checked ? (int)Math.Round(gateThresholdInput.Value * 10m) : 0;

		// Set both controls from a threshold in tenths without triggering each other (caller holds syncingNoiseGate).
		private void SetGateThreshold(int tenths)
		{
			int clamped = Math.Max(noiseGateSlider.Minimum, Math.Min(noiseGateSlider.Maximum, tenths));
			noiseGateSlider.Value = clamped;
			decimal db = Math.Max(gateThresholdInput.Minimum, Math.Min(gateThresholdInput.Maximum, clamped / 10m));
			gateThresholdInput.Value = db;
		}

		private void ApplyGateVisibility()
		{
			noiseGateSlider.Enabled = gateThresholdInput.Enabled = gateEnableCheck.Checked;
		}

		// Slider moved: mirror to the numeric box, then apply live. Persist happens on mouse/key release.
		private void OnNoiseGateChanged(object sender, EventArgs args)
		{
			if (syncingNoiseGate) return;
			syncingNoiseGate = true;
			gateThresholdInput.Value = Math.Max(gateThresholdInput.Minimum, Math.Min(gateThresholdInput.Maximum, noiseGateSlider.Value / 10m));
			syncingNoiseGate = false;
			pendingNoiseGateTenths = GateEffectiveTenths();
			lastMixerEdit = DateTime.UtcNow;
		}

		// Numeric box typed/spun (already clamped to its range): mirror to the slider, apply live, and persist -
		// numeric commits are discrete, unlike a drag.
		private void OnGateInputChanged(object sender, EventArgs args)
		{
			if (syncingNoiseGate) return;
			syncingNoiseGate = true;
			noiseGateSlider.Value = Math.Max(noiseGateSlider.Minimum, Math.Min(noiseGateSlider.Maximum, (int)Math.Round(gateThresholdInput.Value * 10m)));
			syncingNoiseGate = false;
			pendingNoiseGateTenths = GateEffectiveTenths();
			lastMixerEdit = DateTime.UtcNow;
			PersistNoiseGate();
		}

		private void PersistNoiseGate()
		{
			int tenths = GateEffectiveTenths();
			if (tenths == persistedNoiseGateTenths) return;
			try
			{
				WriteNoiseGate(tenths);
				persistedNoiseGateTenths = tenths;
				string where = tenths >= 0 ? "off" : (tenths / 10.0).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture) + " dB";
				SetStatus("Noise gate " + where + " saved and applied. If you don't hear a change, restart Rocksmith (the input stage arms at launch).", ChipTone.Good);
			}
			catch (Exception error)
			{
				SetStatus("Could not save the noise gate: " + error.Message, ChipTone.Bad);
			}
		}

		private static void WriteNoiseGate(int tenths)
		{
			string iniPath = Path.Combine(RSMods.Util.GenUtil.GetRSDirectory(), "RSMods.ini");
			string identifier = RSMods.ReadSettings.NoiseGateThresholdIdentifier;
			string desired = identifier + tenths.ToString(System.Globalization.CultureInfo.InvariantCulture);
			var lines = File.Exists(iniPath) ? new List<string>(File.ReadAllLines(iniPath)) : new List<string>();
			bool replaced = false;
			for (int i = 0; i < lines.Count; i++)
			{
				if (lines[i].StartsWith(identifier, StringComparison.Ordinal))
				{
					lines[i] = desired;
					replaced = true;
					break;
				}
			}
			if (!replaced) lines.Add(desired);
			File.WriteAllLines(iniPath, lines);
		}

		// Reads the saved Rocksmith gate override: whether it is on, and the forced P1_NoiseFloor in tenths of
		// a dB (default -593 = the game's own calibrated default, so enabling it is neutral until dragged).
		private static void ReadRocksmithGate(out bool on, out int tenths)
		{
			on = RSMods.ReadSettings.ProcessSettings(RSMods.ReadSettings.RocksmithGateOverrideIdentifier) == "1";
			string raw = RSMods.ReadSettings.ProcessSettings(RSMods.ReadSettings.RocksmithGateThresholdIdentifier);
			tenths = int.TryParse(raw, out int t) ? Math.Max(-1000, Math.Min(100, t)) : -593;
		}

		// The forced-threshold value in tenths, read from the numeric box (the box is the source of truth).
		private int RgThresholdTenths() => (int)Math.Round(rgGateInput.Value * 10m);

		// Set both controls from a threshold in tenths without triggering each other (caller holds syncingRgGate).
		private void SetRgThreshold(int tenths)
		{
			int clamped = Math.Max(rgGateSlider.Minimum, Math.Min(rgGateSlider.Maximum, tenths));
			rgGateSlider.Value = clamped;
			decimal db = Math.Max(rgGateInput.Minimum, Math.Min(rgGateInput.Maximum, clamped / 10m));
			rgGateInput.Value = db;
		}

		private void ApplyRgGateVisibility()
		{
			rgGateSlider.Enabled = rgGateInput.Enabled = rgEnableCheck.Checked;
		}

		// Slider moved: mirror to the numeric box, then apply live. Persist happens on mouse/key release.
		private void OnRgGateChanged(object sender, EventArgs args)
		{
			if (syncingRgGate) return;
			syncingRgGate = true;
			rgGateInput.Value = Math.Max(rgGateInput.Minimum, Math.Min(rgGateInput.Maximum, rgGateSlider.Value / 10m));
			syncingRgGate = false;
			pendingRocksmithGate = (rgEnableCheck.Checked, RgThresholdTenths());
			lastMixerEdit = DateTime.UtcNow;
		}

		// Numeric box typed/spun: mirror to the slider, apply live, and persist (a discrete commit, unlike a drag).
		private void OnRgInputChanged(object sender, EventArgs args)
		{
			if (syncingRgGate) return;
			syncingRgGate = true;
			rgGateSlider.Value = Math.Max(rgGateSlider.Minimum, Math.Min(rgGateSlider.Maximum, (int)Math.Round(rgGateInput.Value * 10m)));
			syncingRgGate = false;
			pendingRocksmithGate = (rgEnableCheck.Checked, RgThresholdTenths());
			lastMixerEdit = DateTime.UtcNow;
			PersistRocksmithGate();
		}

		private void PersistRocksmithGate()
		{
			bool on = rgEnableCheck.Checked;
			int tenths = RgThresholdTenths();
			if (on == persistedRgOverride && tenths == persistedRgTenths) return;
			try
			{
				WriteRocksmithGate(on, tenths);
				persistedRgOverride = on;
				persistedRgTenths = tenths;
				string where = on ? (tenths / 10.0).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture) + " dB" : "off (game keeps its calibrated gate)";
				SetStatus("Rocksmith gate " + where + " saved and applied.", ChipTone.Good);
			}
			catch (Exception error)
			{
				SetStatus("Could not save the Rocksmith gate: " + error.Message, ChipTone.Bad);
			}
		}

		private static void WriteRocksmithGate(bool on, int tenths)
		{
			string iniPath = Path.Combine(RSMods.Util.GenUtil.GetRSDirectory(), "RSMods.ini");
			var lines = File.Exists(iniPath) ? new List<string>(File.ReadAllLines(iniPath)) : new List<string>();
			ReplaceOrAppendIni(lines, RSMods.ReadSettings.RocksmithGateOverrideIdentifier, on ? "1" : "0");
			ReplaceOrAppendIni(lines, RSMods.ReadSettings.RocksmithGateThresholdIdentifier, tenths.ToString(System.Globalization.CultureInfo.InvariantCulture));
			File.WriteAllLines(iniPath, lines);
		}

		// Surgically replaces the first line beginning with identifier ("Key="), or appends it if absent.
		private static void ReplaceOrAppendIni(List<string> lines, string identifier, string value)
		{
			string desired = identifier + value;
			for (int i = 0; i < lines.Count; i++)
			{
				if (lines[i].StartsWith(identifier, StringComparison.Ordinal)) { lines[i] = desired; return; }
			}
			lines.Add(desired);
		}

		// Reads the saved mains-hum notch base frequency: 0 = off, else the fundamental in Hz. Any value in
		// the DLL's honored range (20..120) is accepted, not just 50/60, so a hand-picked notch survives a
		// reload. Out-of-range or unparseable = off.
		private static int ReadHumFilter()
		{
			string raw = RSMods.ReadSettings.ProcessSettings(RSMods.ReadSettings.HumFilterIdentifier);
			return int.TryParse(raw, out int hz) && hz >= 20 && hz <= 120 ? hz : 0;
		}

		// The effective value to apply/persist: the selected base Hz when enabled, else 0 (off).
		private int HumEffectiveHz() => humFilterCheck.Checked ? (int)humFilterInput.Value : 0;

		private void ApplyHumFilterVisibility()
		{
			humFilterSlider.Enabled = humFilterInput.Enabled = humFilterCheck.Checked;
		}

		// Set both hum controls from a frequency in Hz without them re-triggering each other (caller holds syncingHumFilter).
		private void SetHumFilter(int hz)
		{
			int clamped = Math.Max(20, Math.Min(120, hz));
			humFilterSlider.Value = clamped;
			humFilterInput.Value = clamped;
		}

		// Slider moved: mirror to the box, apply live. Persist happens on mouse/key release.
		private void OnHumSliderChanged(object sender, EventArgs args)
		{
			if (syncingHumFilter) return;
			syncingHumFilter = true;
			humFilterInput.Value = humFilterSlider.Value;
			syncingHumFilter = false;
			pendingHumFilter = HumEffectiveHz();
			lastMixerEdit = DateTime.UtcNow;
		}

		// Box typed/spun: mirror to the slider, apply live and persist (a discrete commit, unlike a drag).
		private void OnHumInputChanged(object sender, EventArgs args)
		{
			if (syncingHumFilter) return;
			syncingHumFilter = true;
			humFilterSlider.Value = (int)humFilterInput.Value;
			syncingHumFilter = false;
			pendingHumFilter = HumEffectiveHz();
			lastMixerEdit = DateTime.UtcNow;
			PersistHumFilter();
		}

		private void PersistHumFilter()
		{
			int hz = HumEffectiveHz();
			if (hz == persistedHumFilter) return;
			try
			{
				string iniPath = Path.Combine(RSMods.Util.GenUtil.GetRSDirectory(), "RSMods.ini");
				var lines = File.Exists(iniPath) ? new List<string>(File.ReadAllLines(iniPath)) : new List<string>();
				ReplaceOrAppendIni(lines, RSMods.ReadSettings.HumFilterIdentifier, hz.ToString(System.Globalization.CultureInfo.InvariantCulture));
				File.WriteAllLines(iniPath, lines);
				persistedHumFilter = hz;
				SetStatus("Hum filter " + (hz == 0 ? "off" : hz + " Hz") + " saved and applied.", ChipTone.Good);
			}
			catch (Exception error)
			{
				SetStatus("Could not save the hum filter: " + error.Message, ChipTone.Bad);
			}
		}

		// Compressor slider: 0-100 strength stored verbatim in RSMods.ini and the DLL (0 = off).
		private static int ReadCompressorStrength()
		{
			string raw = RSMods.ReadSettings.ProcessSettings(RSMods.ReadSettings.CompressorStrengthIdentifier);
			return int.TryParse(raw, out int value) ? Math.Max(0, Math.Min(100, value)) : 0;
		}

		// Set both controls from a strength (0-100) without them re-triggering each other (caller holds the sync flag).
		private void SetCompressor(int strength)
		{
			int s = Math.Max(0, Math.Min(100, strength));
			compressorSlider.Value = s;
			compInput.Value = s;
		}

		private void ApplyCompressorVisibility()
		{
			compressorSlider.Enabled = compInput.Enabled = compressorEnableCheck.Checked;
		}

		private void OnCompressorChanged(object sender, EventArgs args)   // slider moved
		{
			if (syncingCompressor) return;
			int strength = Math.Max(0, Math.Min(100, compressorSlider.Value));
			syncingCompressor = true;
			compInput.Value = strength;
			syncingCompressor = false;
			pendingCompressor = strength;
			lastMixerEdit = DateTime.UtcNow;
		}

		private void OnCompInputChanged(object sender, EventArgs args)   // numeric typed/spun
		{
			if (syncingCompressor) return;
			int strength = Math.Max(0, Math.Min(100, (int)compInput.Value));
			syncingCompressor = true;
			compressorSlider.Value = strength;
			syncingCompressor = false;
			pendingCompressor = strength;
			lastMixerEdit = DateTime.UtcNow;
			PersistCompressor();
		}

		private void PersistCompressor()
		{
			int strength = Math.Max(0, Math.Min(100, compressorSlider.Value));
			if (strength == persistedCompressor) return;
			try
			{
				WriteCompressor(strength);
				persistedCompressor = strength;
				string where = strength <= 0 ? "off" : strength.ToString(System.Globalization.CultureInfo.InvariantCulture) + "%";
				SetStatus("Compressor " + where + " saved and applied. If you don't hear a change, restart Rocksmith (the input stage arms at launch).", ChipTone.Good);
			}
			catch (Exception error)
			{
				SetStatus("Could not save the compressor: " + error.Message, ChipTone.Bad);
			}
		}

		private static void WriteCompressor(int strength)
		{
			string iniPath = Path.Combine(RSMods.Util.GenUtil.GetRSDirectory(), "RSMods.ini");
			string identifier = RSMods.ReadSettings.CompressorStrengthIdentifier;
			string desired = identifier + strength.ToString(System.Globalization.CultureInfo.InvariantCulture);
			var lines = File.Exists(iniPath) ? new List<string>(File.ReadAllLines(iniPath)) : new List<string>();
			bool replaced = false;
			for (int i = 0; i < lines.Count; i++)
			{
				if (lines[i].StartsWith(identifier, StringComparison.Ordinal))
				{
					lines[i] = desired;
					replaced = true;
					break;
				}
			}
			if (!replaced) lines.Add(desired);
			File.WriteAllLines(iniPath, lines);
		}

		// Surgical single-line write, like WriteModernCableInput: this runs in the bridge process, which
		// never loads the main window's settings table, so a full rewrite would wipe every other setting.
		private static void WriteInputGain(int tenths)
		{
			string iniPath = Path.Combine(RSMods.Util.GenUtil.GetRSDirectory(), "RSMods.ini");
			string identifier = RSMods.ReadSettings.AsioInputGainIdentifier;
			string desired = identifier + tenths.ToString(System.Globalization.CultureInfo.InvariantCulture);
			var lines = File.Exists(iniPath) ? new List<string>(File.ReadAllLines(iniPath)) : new List<string>();
			bool replaced = false;
			for (int i = 0; i < lines.Count; i++)
			{
				if (lines[i].StartsWith(identifier, StringComparison.Ordinal))
				{
					lines[i] = desired;
					replaced = true;
					break;
				}
			}
			if (!replaced) lines.Add(desired);
			File.WriteAllLines(iniPath, lines);
		}

		// Surgical single-line write for the limiter toggle (same reason as WriteInputGain: the bridge
		// process must not rewrite the whole ini). 1 = on, 0 = off.
		// The slider and box both work in 0.1 dB (-24..0); RSMods.ini and the DLL speak tenths of a dB (-240..0).
		private static int LimiterSliderToTenths(int value) => value;
		private static int LimiterTenthsToSlider(int tenths) => Math.Max(-240, Math.Min(0, tenths));

		private static int ReadLimiterLevelTenths()
		{
			string raw = RSMods.ReadSettings.ProcessSettings(RSMods.ReadSettings.AudioBridgeLimiterLevelIdentifier);
			return int.TryParse(raw, out int tenths) ? Math.Max(-240, Math.Min(0, tenths)) : -60;
		}

		// Mirror the ceiling slider onto the numeric box (guarded so the box's own handler does not re-fire).
		private void UpdateLimiterReadout()
		{
			syncingLimiter = true;
			ceilingInput.Value = Math.Max(ceilingInput.Minimum, Math.Min(ceilingInput.Maximum, limiterLevelSlider.Value / 10m));
			syncingLimiter = false;
		}

		private void OnCeilingInputChanged(object sender, EventArgs args)   // numeric typed/spun
		{
			if (syncingLimiter) return;
			syncingLimiter = true;
			limiterLevelSlider.Value = Math.Max(limiterLevelSlider.Minimum, Math.Min(limiterLevelSlider.Maximum, (int)Math.Round(ceilingInput.Value * 10m)));
			syncingLimiter = false;
			pendingLimiter = LimiterCommand();
			lastMixerEdit = DateTime.UtcNow;
			PersistLimiterLevel();
		}

		// The ceiling controls are live only while the limiter is on and the proxy driver is installed.
		private void ApplyOutputControlVisibility()
		{
			bool limOn = limiterCheck.Checked && limiterCheck.Enabled;
			limiterLevelSlider.Enabled = ceilingInput.Enabled = limOn;
		}

		// The proxy command (op 16): "<limiterOn>,<ceiling>,<agcOn>,<target>". The AGC stage is retired, so agcOn
		// is always 0 and target is a fixed placeholder; only the look-ahead brickwall limiter runs.
		private string LimiterCommand()
		{
			var ci = System.Globalization.CultureInfo.InvariantCulture;
			int limOn = limiterCheck.Checked ? 1 : 0;
			double ceiling = Math.Pow(10.0, (LimiterSliderToTenths(limiterLevelSlider.Value) / 10.0) / 20.0);
			return limOn.ToString(ci) + "," + ceiling.ToString("0.######", ci) + ",0,0.1";
		}

		private void PersistLimiterLevel()
		{
			try { WriteBridgeSetting(RSMods.ReadSettings.AudioBridgeLimiterLevelIdentifier, LimiterSliderToTenths(limiterLevelSlider.Value).ToString(System.Globalization.CultureInfo.InvariantCulture)); }
			catch (Exception error) { SetStatus("Could not save the ceiling: " + error.Message, ChipTone.Bad); }
		}

		// Surgical single-line write shared by the bridge-owned RSMods.ini settings (see WriteModernCableInput).
		private static void WriteBridgeSetting(string identifier, string value)
		{
			WriteModSetting(identifier, value);
		}

		private static void WriteLimiterSetting(bool on)
		{
			WriteModSetting(RSMods.ReadSettings.AudioBridgeLimiterIdentifier, on ? "1" : "0");
		}

		private void UpdateTakeButtons()
		{
			bool selected = takes.SelectedPath != null;
			playButton.Enabled = selected;
			revealButton.Enabled = selected;
		}

		private void SwitchInputMode(bool enableAsio)
		{
			if (!masterEnabled) return;
			try
			{
				inputMode.SetAsioEnabled(enableAsio);
				SetStatus(enableAsio ? "ASIO input selected. Launch Rocksmith when ready." : "Real Tone Cable input selected. Launch Rocksmith when ready.", ChipTone.Good);
			}
			catch (Exception error) { SetStatus(error.Message, ChipTone.Bad); }
			RefreshEnvironment();
			UpdateState();
		}

		private void SynchronizePlaybackState()
		{
			if (latestStatus == null)
			{
				if (hasPlaybackStatus) isShowingPlaybackStatus = true;
				hasPlaybackStatus = false;
				return;
			}

			// Use the routed device id (not the "(route)<id>" wire sentinel) so the selection follows the
			// device the mix is actually playing on while a route is live.
			string activeId = isRouting ? routedEndpointId : latestStatus.EndpointId;
			var selected = outputSelector.SelectedItem as AudioDeviceChoice;
			// On the first status after (re)connecting, adopt whatever device is actually live rather than the
			// persisted default. A route (op 21) lives in the game process and mutes the ASIO forward, and it
			// survives a GUI restart, so without this the selector claims the ASIO device while that device sits
			// silent and the mix plays on the routed device with no sign a route is active. hasPlaybackStatus is
			// still false here on the first call, so this only relaxes the guard for that initial adoption.
			bool followsPlayback = selected == null || !hasPlaybackStatus || selected.Id == displayedPlaybackEndpoint;
			bool changed = !hasPlaybackStatus || displayedPlaybackEndpoint != activeId
				|| displayedPlaybackError != latestStatus.OutputError
				|| displayedRouteStalled != routeSourceStalled;
			hasPlaybackStatus = true;
			displayedPlaybackEndpoint = activeId;
			displayedPlaybackError = latestStatus.OutputError;
			displayedRouteStalled = routeSourceStalled;
			if (changed) isShowingPlaybackStatus = true;

			if (!followsPlayback || latestStatus.OutputError < 0) return;
			for (int index = 0; index < outputSelector.Items.Count; index++)
			{
				var device = (AudioDeviceChoice)outputSelector.Items[index];
				if (device.Id == activeId)
				{
					if (outputSelector.SelectedIndex != index) outputSelector.SelectedIndex = index;
					return;
				}
			}
		}

		private void UpdatePlaybackStatus()
		{
			if (!isShowingPlaybackStatus) return;
			string message;
			var tone = ChipTone.Good;
			bool perfActive = false;   // lit the header bolt only when the game plays straight out the fast ASIO device
			if (latestStatus == null)
			{
				message = "Rocksmith disconnected · playback device unavailable.";
				tone = ChipTone.Idle;
			}
			else if (latestStatus.OutputError == unchecked((int)0x8000000A))
			{
				message = "Opening selected playback device...";
				tone = ChipTone.Warn;
			}
			else if (isSilentProxy)
			{
				message = "No playback device is available · Rocksmith is still running silently and will route automatically when one appears.";
				tone = ChipTone.Warn;
			}
			else if (isStartingProxy)
			{
				message = "Audio proxy is starting · Rocksmith remains available while playback is prepared.";
				tone = ChipTone.Idle;
			}
			else if (isDowngradingProxy)
			{
				message = "ASIO output was lost · downgrading to the selected shared output.";
				tone = ChipTone.Warn;
			}
			else if (latestStatus.OutputError < 0 || string.IsNullOrEmpty(latestStatus.EndpointId))
			{
				message = "Playback output unavailable · choose another device.";
				tone = ChipTone.Bad;
			}
			else if (routeSourceStalled)
			{
				// The Windows route is open but the real ASIO callback source stopped. The proxy watchdog will
				// replace that source with its virtual callback stream, so this is a temporary recovery state.
				string name = null;
				foreach (AudioDeviceChoice device in outputSelector.Items)
				{
					if (device.Id == routedEndpointId) { name = device.Name; break; }
				}
				message = "ASIO output was lost · recovering playback through "
					+ (name ?? "the selected shared device") + ". Rocksmith remains running.";
				tone = ChipTone.Warn;
			}
			else
			{
				// While routing, the wire endpoint is "(route)<id>"; resolve to the real device id so the name
				// lookup and selection-follow work, and say "Routing to" to make the redirect visible.
				string activeId = isRouting ? routedEndpointId : latestStatus.EndpointId;
				string name = null;
				foreach (AudioDeviceChoice device in outputSelector.Items)
				{
					if (device.Id == activeId)
					{
						name = device.Name;
						break;
					}
				}
				message = name != null ? (isRouting ? "Routing to " : "Now playing through ") + name
					: activeId == PassthroughEndpoint ? "Playing straight through the ASIO device" : "Active playback device: " + activeId;
				// Name the live OUTPUT transport, reflecting the state it is actually in right now. Fast only
				// when the game plays straight out the low-latency ASIO device: the bridge is passing through
				// and the proxy forwards to the real ASIO driver with no extra hop. The moment the output is
				// redirected elsewhere (routed to laptop speakers, or a bridge-owned WASAPI endpoint) it renders
				// over shared WASAPI and is downgraded, even though the guitar input is still ASIO, so drop the
				// bolt. Recomputed every poll, so it follows live hot-swaps between the two paths. Tagged only in
				// the proxy world, so a plain setup is not mislabelled.
				if (proxyInstalled)
				{
					bool fastAsioOutput = latestStatus.EndpointId == PassthroughEndpoint;
					perfActive = fastAsioOutput;
					message += fastAsioOutput ? " · high-performance" : " · shared output";
				}
				var selected = outputSelector.SelectedItem as AudioDeviceChoice;
				if (selected != null && selected.Id != activeId)
				{
					message += " · Selected " + selected.Name + "; press Apply output to switch.";
				}
			}
			SetStatus(message, tone);
			SetPerformanceActive(perfActive);
			isShowingPlaybackStatus = true;
		}

		private void UpdateRouteState(string endpointId)
		{
			endpointId = endpointId ?? "";
			routeSourceStalled = endpointId.StartsWith(RouteStalledPrefix, StringComparison.Ordinal);
			isVirtualRoute = endpointId.StartsWith(RouteVirtualPrefix, StringComparison.Ordinal);
			isRouting = routeSourceStalled || isVirtualRoute || endpointId.StartsWith(RoutePrefix, StringComparison.Ordinal);
			routedEndpointId = routeSourceStalled ? endpointId.Substring(RouteStalledPrefix.Length)
				: isVirtualRoute ? endpointId.Substring(RouteVirtualPrefix.Length)
				: isRouting ? endpointId.Substring(RoutePrefix.Length) : "";
		}

		// Light (show) the header bolt only when high-performance ASIO output is live; hide it otherwise, so its
		// presence at the top right is the at-a-glance "fast output is active" signal.
		private void SetPerformanceActive(bool active)
		{
			perfBolt.Visible = active;
		}

		private void SetStatus(string message, ChipTone tone)
		{
			isShowingPlaybackStatus = false;
			statusLabel.Text = message;
			SetTip(statusLabel, message);
			statusLabel.ForeColor = tone == ChipTone.Bad ? StudioTheme.Record
				: tone == ChipTone.Warn ? StudioTheme.Warning
				: tone == ChipTone.Good ? StudioTheme.Positive
				: StudioTheme.Muted;
		}

		private void SavePreferences()
		{
			WriteSetting("RecordingDirectory", Path.GetFullPath(recordingDirectory.Text));
			WriteSetting("RecordingSource", recordingSource.SelectedIndex == 1 ? "Dry" : "Tone");
		}

		private void EnableBridge(string outputId)
		{
			if (!masterEnabled) throw new InvalidOperationException("Turn the audio bridge on before changing the audio setup.");
			if (inputMode.IsGameRunning()) throw new InvalidOperationException("Close Rocksmith before enabling the bridge. If it is already enabled, wait for its connection before switching output.");
			string temporary = settingsPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
			try
			{
				if (File.Exists(settingsPath))
					File.Copy(settingsPath, temporary);
				else
					File.WriteAllText(temporary, "[Audio]\r\n", Encoding.Unicode);
				if (!WritePrivateProfileString("Audio", "OutputDevice", outputId, temporary)
					|| !WritePrivateProfileString("Audio", "Enabled", "1", temporary)
					|| !WritePrivateProfileString("Audio", "MasterEnabled", "1", temporary))
					throw new IOException("Could not prepare audio settings.");
				if (File.Exists(settingsPath))
					File.Replace(temporary, settingsPath, null);
				else
					File.Move(temporary, settingsPath);
			}
			catch
			{
				throw;
			}
			finally { if (File.Exists(temporary)) File.Delete(temporary); }
		}

		private void SetMasterPower(bool enabled)
		{
			if (inputMode.IsGameRunning())
			{
				syncingMasterPower = true;
				masterPower.IsOn = masterEnabled;
				syncingMasterPower = false;
				SetStatus("Close Rocksmith before turning the audio bridge on or off.", ChipTone.Warn);
				UpdateState();
				return;
			}

			try
			{
				if (enabled)
				{
					WriteSetting("MasterEnabled", "1");
					masterEnabled = true;
					masterOffRequiresClose = false;
					SetStatus("Audio bridge on. Choose an output on the Setup tab and press Apply output. Note by Note is unaffected.", ChipTone.Good);
				}
				else
				{
					AsioProxySetup.Unlink(gameDirectory);
					WriteSetting("Enabled", "0");
					WriteSetting("MasterEnabled", "0");
					masterEnabled = false;
					SetStatus("Audio bridge off. The audio setup will not be touched; Note by Note still works.", ChipTone.Idle);
				}
			}
			catch (Exception error)
			{
				syncingMasterPower = true;
				masterPower.IsOn = masterEnabled;
				syncingMasterPower = false;
				SetStatus("Could not change the audio bridge power: " + error.Message, ChipTone.Bad);
			}
			syncingMasterPower = true;
			masterPower.IsOn = masterEnabled;
			syncingMasterPower = false;
			RefreshEnvironment();
			UpdateState();
		}

		private void ReconcileDisabledBridge()
		{
			if (inputMode.IsGameRunning())
			{
				masterOffRequiresClose = true;
				return;
			}
			try
			{
				AsioProxySetup.Unlink(gameDirectory);
				WriteSetting("Enabled", "0");
				WriteSetting("MasterEnabled", "0");
				masterOffRequiresClose = false;
			}
			catch (Exception error)
			{
				masterOffRequiresClose = true;
				SetStatus("The audio bridge is off, but restoring the saved audio setup needs attention: " + error.Message, ChipTone.Warn);
			}
		}

		private void ReconcileEnabledBridge()
		{
			try
			{
				AsioProxySetup.SynchronizeLinkedInputs(gameDirectory);
			}
			catch (Exception error)
			{
				SetStatus("The audio bridge is on, but its shared ASIO setup needs attention: " + error.Message, ChipTone.Warn);
			}
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
				return "Automatic (device default)";
			if (saved != bufferSnapshot.Period.ToString(System.Globalization.CultureInfo.InvariantCulture)) return "Runtime differs from saved buffer";
			return "Custom";
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
				if (deviceEnumerator != null)
				{
					int result = deviceEnumerator.UnregisterEndpointNotificationCallback(this);
					if (result < 0) Trace.TraceError("Could not unregister playback device notifications: 0x{0:X8}", result);
					deviceEnumerator.Dispose();
					deviceEnumerator = null;
				}
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
