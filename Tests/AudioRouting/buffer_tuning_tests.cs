using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using RSMods.Audio;

namespace AudioRoutingTests
{
	internal static class BufferTuningTests
	{
		private static void Require(bool value, string message)
		{
			if (!value) throw new Exception(message);
		}

		private static BufferTuningSnapshot Sample(ulong frames, ulong empty = 0, ulong repeats = 0, uint minimum = 128, uint maximum = 960)
		{
			return BufferTuningSnapshot.Parse(new AudioControlStatus
			{
				IsRecording = true, Frames = frames, EndpointId = "test",
				FilePath = "engineMinimum=" + minimum + " engineFundamental=16 engineMaximum=" + maximum
					+ " enginePeriod=" + minimum + " emptyOutputObservations=" + empty + " repeatedGameBlocks=" + repeats
			});
		}

		private static void Main()
		{
			var window = new BufferStabilityWindow();
			for (int second = 0; second < 10; ++second)
				Require(window.Observe(second * 1000, true, Sample((ulong)(second + 1) * 48000, 100)) != BufferTrialResult.Stable, "Premature pass");
			Require(window.Observe(10000, true, Sample(36 * 48000, 100)) == BufferTrialResult.Stable, "Clean run did not pass");
			Require(window.Observe(36000, true, Sample(37 * 48000, 101)) == BufferTrialResult.Unstable, "New empty output ignored");
			Require(window.Observe(37000, true, Sample(38 * 48000, 100, 1)) == BufferTrialResult.RepeatedAudio, "Repeated samples hidden as buffering");
			Require(window.Observe(38000, false, Sample(39 * 48000, 100)) == BufferTrialResult.WarmingUp, "Focus loss did not reset test");
			Require(window.Observe(39000, true, Sample(40 * 48000, 100)) == BufferTrialResult.WarmingUp, "Focus return skipped warmup");
			Require(window.Observe(90000, true, Sample(40 * 48000, 100)) == BufferTrialResult.WarmingUp, "Stopped stream passed test");
						Require(window.ProgressText.Contains("no new audio"), "Stalled-stream restart reason is missing");
			var longerWindow = new BufferStabilityWindow(true);
			for (int second = 0; second < 35; second++)
				Require(longerWindow.Observe(second * 1000, true, Sample((ulong)(second + 1) * 48000)) != BufferTrialResult.Stable, "Long check passed early");
			Require(longerWindow.Observe(35000, true, Sample(36 * 48000)) == BufferTrialResult.Stable, "Long check did not finish");
			var fixedPeriods = new List<uint>(Sample(1, minimum: 480, maximum: 480).GetCandidates());
			Require(fixedPeriods.Count == 1 && fixedPeriods[0] == 480, "Invented unsupported periods");
			var periods = new List<uint>(Sample(1, maximum: 950).GetCandidates());
			for (int i = 0; i < periods.Count; ++i)
				Require(periods[i] % 16 == 0 && periods[i] <= 950 && (i == 0 || periods[i] > periods[i - 1]), "Invalid candidate sequence");
			bool rejected = false;
			try { BufferTuningSnapshot.Parse(new AudioControlStatus { IsRecording = true, FilePath = "" }); }
			catch (System.IO.IOException) { rejected = true; }
			Require(rejected, "Missing diagnostics interpreted as a clean run");
			var cancellation = new CancellationTokenSource();
			var applied = new List<uint>();
			uint active = 256;
			bool saved = false;
			var tuner = new AudioBufferTuner((operation, value) =>
			{
				if (operation == 1) return Task.FromResult(new AudioControlStatus());
				if (operation == 6)
				{
					active = uint.Parse(value.Split('\n')[0]); applied.Add(active);
					if (active == 128) cancellation.Cancel();
				}
				return Task.FromResult(new AudioControlStatus
				{
					IsRecording = true, Frames = 1, EndpointId = "test",
					FilePath = "engineMinimum=128 engineFundamental=16 engineMaximum=960 enginePeriod=" + active
						+ " emptyOutputObservations=0 repeatedGameBlocks=0"
				});
			}, () => true, message => { }, (endpoint, period) => saved = true);
			bool cancelled = false;
			try { tuner.RunAsync(cancellation.Token).GetAwaiter().GetResult(); }
			catch (OperationCanceledException) { cancelled = true; }
			Require(cancelled && !saved && applied.Count == 2 && applied[0] == 128 && applied[1] == 256,
				"Cancellation failed to restore the original output period without saving");
			cancellation.Dispose();
			Console.WriteLine("PASS: buffer tuning warmup, observation interval, fault discrimination, focus loss, stalled streams and supported-period candidates");
		}
	}
}
