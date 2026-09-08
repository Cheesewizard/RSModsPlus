using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace RSMods.Audio
{
	internal sealed class BufferTuningSnapshot
	{
		public uint Minimum { get; private set; }
		public uint Fundamental { get; private set; }
		public uint Maximum { get; private set; }
		public uint Period { get; private set; }
		public ulong EmptyObservations { get; private set; }
		public ulong RepeatedBlocks { get; private set; }
		public ulong Frames { get; private set; }
		public string Endpoint { get; private set; }

		public static BufferTuningSnapshot Parse(AudioControlStatus status, bool requireRunning = true)
		{
			if (status.OutputError < 0 || (requireRunning && !status.IsRecording))
				throw new InvalidOperationException("The output stream must be running to test its buffer.");
			var fields = new Dictionary<string, ulong>();
			foreach (string item in status.FilePath.Split(' '))
			{
				string[] pair = item.Split('=');
				if (pair.Length == 2 && ulong.TryParse(pair[1], NumberStyles.None, CultureInfo.InvariantCulture, out ulong value))
					fields.Add(pair[0], value);
			}
			foreach (string key in new[] { "engineMinimum", "engineFundamental", "engineMaximum", "enginePeriod", "emptyOutputObservations", "repeatedGameBlocks" })
			{
				if (!fields.ContainsKey(key)) throw new IOException("This bridge does not provide buffer tuning diagnostics. Update the game DLL.");
			}
			var result = new BufferTuningSnapshot
			{
				Minimum = checked((uint)fields["engineMinimum"]), Fundamental = checked((uint)fields["engineFundamental"]),
				Maximum = checked((uint)fields["engineMaximum"]), Period = checked((uint)fields["enginePeriod"]),
				EmptyObservations = fields["emptyOutputObservations"], RepeatedBlocks = fields["repeatedGameBlocks"],
				Frames = status.Frames, Endpoint = status.EndpointId
			};
			if (result.Minimum == 0 || result.Fundamental == 0 || result.Maximum < result.Minimum || result.Maximum > 48000)
				throw new IOException("The device returned invalid buffer limits.");
			return result;
		}

		public IEnumerable<uint> GetCandidates()
		{
			uint limit = Math.Min(Maximum, Math.Max(Minimum, 1920u));
			uint current = Minimum;
			for (int count = 0; count < 8 && current <= limit; ++count)
			{
				if (current % Fundamental != 0) throw new IOException("The device returned an unaligned minimum period.");
				yield return current;
				if (current == limit) yield break;
				uint next = ((current + current / 2 + Fundamental - 1) / Fundamental) * Fundamental;
				next = Math.Min(limit / Fundamental * Fundamental, Math.Max(current + Fundamental, next));
				if (next <= current) yield break;
				current = next;
			}
		}
	}

	internal enum BufferTrialResult { WarmingUp, Measuring, Unstable, RepeatedAudio, Stable }

	internal sealed class BufferStabilityWindow
	{
		private const int WARMUP_MILLISECONDS = 2000;
		public string ProgressText { get; private set; } = "Ready";
		private readonly int measurementMilliseconds;
		private long started = -1;
		private long baselineTime = -1;
		private BufferTuningSnapshot baseline;
		private ulong previousFrames;

		public BufferStabilityWindow(bool longerCheck = false)
		{
			measurementMilliseconds = longerCheck ? 33000 : 8000;
		}

		public BufferTrialResult Observe(long milliseconds, bool focused, BufferTuningSnapshot sample)
		{
			if (!focused || sample.Frames == previousFrames)
			{
				ProgressText = !focused ? "Paused: return to Rocksmith; the countdown restarts when focused." : "Paused: no new audio; the countdown restarts when audio resumes.";
				started = -1; baselineTime = -1; baseline = null;
				previousFrames = sample.Frames;
				return BufferTrialResult.WarmingUp;
			}
			previousFrames = sample.Frames;
			if (started < 0) started = milliseconds;
			if (milliseconds - started < WARMUP_MILLISECONDS)
			{
				ProgressText = "Warming up · " + Math.Ceiling((WARMUP_MILLISECONDS - (milliseconds - started)) / 1000.0) + " s remaining";
				return BufferTrialResult.WarmingUp;
			}
			if (baseline == null) { baseline = sample; baselineTime = milliseconds; }
			if (sample.RepeatedBlocks > baseline.RepeatedBlocks) return BufferTrialResult.RepeatedAudio;
			if (sample.EmptyObservations < baseline.EmptyObservations || sample.Frames < baseline.Frames)
				throw new IOException("Audio counters reset during the test.");
			if (sample.EmptyObservations > baseline.EmptyObservations) return BufferTrialResult.Unstable;
			long elapsed = milliseconds - baselineTime;
			ProgressText = "Checking audio · " + Math.Min(measurementMilliseconds / 1000, elapsed / 1000) + "/" + measurementMilliseconds / 1000 + " s · "
				+ Math.Max(0, Math.Ceiling((measurementMilliseconds - elapsed) / 1000.0)) + " s remaining";
			return elapsed >= measurementMilliseconds ? BufferTrialResult.Stable : BufferTrialResult.Measuring;
		}
	}

	internal sealed class AudioBufferTuner
	{
		private readonly Func<uint, string, Task<AudioControlStatus>> send;
		private readonly Func<bool> isGameFocused;
		private readonly Action<string> report;
		private readonly Action<string, uint> save;

		public AudioBufferTuner(Func<uint, string, Task<AudioControlStatus>> send, Func<bool> isGameFocused, Action<string> report, Action<string, uint> save)
		{
			this.send = send ?? throw new ArgumentNullException(nameof(send));
			this.isGameFocused = isGameFocused ?? throw new ArgumentNullException(nameof(isGameFocused));
			this.report = report ?? throw new ArgumentNullException(nameof(report));
			this.save = save ?? throw new ArgumentNullException(nameof(save));
		}

		public async Task<uint> RunAsync(CancellationToken cancellation, bool longerCheck = false)
		{
			if ((await send(1, "")).IsRecording) throw new InvalidOperationException("Finish the recording before tuning.");
			var initial = BufferTuningSnapshot.Parse(await send(5, ""));
			bool committed = false;
			uint activePeriod = initial.Period;
			var clock = Stopwatch.StartNew();
			try
			{
				foreach (uint period in initial.GetCandidates())
				{
					cancellation.ThrowIfCancellationRequested();
					if (activePeriod != period) await send(6, period.ToString(CultureInfo.InvariantCulture) + "\n" + initial.Endpoint);
					activePeriod = period;
					var trial = new BufferStabilityWindow(longerCheck);
					while (clock.Elapsed < TimeSpan.FromMinutes(6))
					{
						cancellation.ThrowIfCancellationRequested();
						if ((await send(1, "")).IsRecording)
							throw new InvalidOperationException("Recording started during tuning; the test was interrupted.");
						var sample = BufferTuningSnapshot.Parse(await send(5, ""));
						if (sample.Endpoint != initial.Endpoint || sample.Period != period)
							throw new InvalidOperationException("The output changed during tuning; the result was discarded.");
						var result = trial.Observe(clock.ElapsedMilliseconds, isGameFocused(), sample);
						report("Testing " + (period / 48.0).ToString("0.##", CultureInfo.InvariantCulture) + " ms · "
							+ trial.ProgressText);
						if (result == BufferTrialResult.RepeatedAudio)
							throw new InvalidOperationException("Repeated game audio detected. Buffer tuning cannot establish a clean result; the previous setting was restored.");
						if (result == BufferTrialResult.Unstable) break;
						if (result == BufferTrialResult.Stable)
						{
							save(initial.Endpoint, period);
							committed = true;
							return period;
						}
						await Task.Delay(1000, cancellation);
					}
					if (clock.Elapsed >= TimeSpan.FromMinutes(6)) throw new TimeoutException("Tuning timed out. Keep the game focused during the test.");
				}
				throw new InvalidOperationException("No tested period stayed clear of empty-output observations. The previous setting was restored.");
			}
			finally
			{
				if (!committed && activePeriod != initial.Period)
				{
					var current = await send(5, "");
					if (current.EndpointId == initial.Endpoint)
						await send(6, initial.Period.ToString(CultureInfo.InvariantCulture) + "\n" + initial.Endpoint);
				}
			}
		}
	}
}
