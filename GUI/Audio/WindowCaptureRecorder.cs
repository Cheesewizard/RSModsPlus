using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace RSMods.Audio
{
	internal sealed class WindowCaptureRecorder : IDisposable
	{
		private const uint FramesPerSecond = 30;
		private const uint BitsPerSecond = 12000000;
		private const long MaximumDriftTicks = 30 * TimeSpan.TicksPerSecond;
		private string videoPath;
		private bool capturing;

		public static bool Supported
		{
			get
			{
				try { return RsCaptureSupported() != 0; }
				catch (DllNotFoundException) { return false; }
				catch (EntryPointNotFoundException) { return false; }
			}
		}

		public static string Requirement
		{
			get { return "Video capture needs Windows 10 1903 or newer with the recorder library installed."; }
		}

		public bool IsCapturing
		{
			get { return capturing; }
		}

		public Task StartAsync(int gameProcessId, string directory)
		{
			if (capturing)
				throw new InvalidOperationException("A video take is already open.");
			using (var game = Process.GetProcessById(gameProcessId))
			{
				if (game.MainWindowHandle == IntPtr.Zero)
					throw new InvalidOperationException("Rocksmith has no capture window.");
				Directory.CreateDirectory(directory);
				videoPath = Path.Combine(directory, "Rocksmith-video-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N") + ".mp4");
				Check(RsCaptureStart(game.MainWindowHandle, videoPath, BitsPerSecond, FramesPerSecond), "Video capture could not start");
				capturing = true;
			}
			return Task.FromResult(true);
		}

		public async Task<string> StopAsync(AudioControlStatus audio)
		{
			if (!capturing)
				throw new InvalidOperationException("No video take is open.");
			capturing = false;
			ulong startFileTime;
			ulong frames;
			int stopped = RsCaptureStop(out startFileTime, out frames);
			if (stopped == ErrorEmpty)
				throw new IOException("No frames were captured. Play in a visible window and try the take again.");
			Check(stopped, "Video capture failed");
			if (audio == null || audio.RecordingStarted == 0 || audio.Frames == 0 || !File.Exists(audio.FilePath))
				throw new IOException("No game audio was captured. The video file was retained.");
			long offset = (long)audio.RecordingStarted - (long)startFileTime;
			if (Math.Abs(offset) > MaximumDriftTicks)
				throw new IOException("Video and audio clocks did not agree. Separate take files were retained.");
			string output = Path.ChangeExtension(audio.FilePath, ".mp4");
			string video = videoPath;
			await Task.Run(() => Combine(video, audio.FilePath, output, offset));
			Delete(video);
			videoPath = null;
			return output;
		}

		public static string Combine(string videoFile, string audioFile, string outputFile, long audioOffset)
		{
			Check(RsCaptureMux(videoFile, audioFile, outputFile, audioOffset), "Video and audio could not be combined");
			return outputFile;
		}

		public void Dispose()
		{
			if (capturing)
			{
				capturing = false;
				ulong startFileTime;
				ulong frames;
				RsCaptureStop(out startFileTime, out frames);
			}
			Delete(videoPath);
			videoPath = null;
		}

		private static void Delete(string path)
		{
			if (string.IsNullOrEmpty(path))
				return;
			try { if (File.Exists(path)) File.Delete(path); }
			catch (IOException error) { Trace.TraceError("Capture cleanup failed: " + error); }
			catch (UnauthorizedAccessException error) { Trace.TraceError("Capture cleanup failed: " + error); }
		}

		private static void Check(int result, string message)
		{
			if (result >= 0)
				return;
			if (result == ErrorNotSupported)
				throw new NotSupportedException(Requirement);
			throw new IOException(message + " (0x" + result.ToString("X8", CultureInfo.InvariantCulture) + ").");
		}

		private static readonly int ErrorEmpty = unchecked((int)0x800700FE);
		private static readonly int ErrorNotSupported = unchecked((int)0x80070032);

		[DllImport("rswindowcapture.dll")]
		private static extern int RsCaptureSupported();

		[DllImport("rswindowcapture.dll", CharSet = CharSet.Unicode)]
		private static extern int RsCaptureStart(IntPtr window, string path, uint bitsPerSecond, uint framesPerSecond);

		[DllImport("rswindowcapture.dll")]
		private static extern int RsCaptureStop(out ulong startFileTime, out ulong frames);

		[DllImport("rswindowcapture.dll", CharSet = CharSet.Unicode)]
		private static extern int RsCaptureMux(string videoPath, string audioPath, string outputPath, long audioOffset);
	}
}
