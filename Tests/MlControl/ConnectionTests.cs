using System;
using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Threading;
using RSModsPlus.MachineLearning;

namespace RSModsPlus.Tests
{
	internal static class ConnectionTests
	{
		[DllImport("kernel32.dll")]
		private static extern ulong GetTickCount64();

		private static void Require(bool condition, string message)
		{
			if (!condition) throw new InvalidOperationException(message);
			Console.WriteLine("PASS " + message);
		}

		private static void WaitUntil(Func<bool> predicate)
		{
			var clock = Stopwatch.StartNew();
			while (!predicate())
			{
				if (clock.ElapsedMilliseconds > 6000) throw new TimeoutException("Expected service transition did not occur.");
				Thread.Sleep(30);
			}
		}

		private static void Main(string[] arguments)
		{
			var directory = Path.GetDirectoryName(Path.GetFullPath(arguments[0]));
			var serviceDirectory = Path.Combine(directory, "RSMods");
			Directory.CreateDirectory(serviceDirectory);
			File.Copy(arguments[0], Path.Combine(serviceDirectory, "RSMods.exe"), true);
			File.WriteAllText(Path.Combine(directory, "rsmodsplus.dll"), "isolated launcher fixture");
			using (var host = Process.Start(new ProcessStartInfo(arguments[0]) { UseShellExecute = false, CreateNoWindow = true }))
			{
				try
				{
					WaitUntil(() => MlServiceConnection.Read(host.Id, ".TestControl").CanRestart);
					Require(!MlServiceConnection.Read(host.Id, ".TestControl").IsConnected, "Running process without results is not connected");
					var starts = Path.Combine(serviceDirectory, "starts.txt");
					WaitUntil(() => File.Exists(starts));
					var first = int.Parse(File.ReadAllLines(starts)[0]);
					MlServiceConnection.RequestRestart(host.Id);
					WaitUntil(() => File.ReadAllLines(starts).Length == 2);
					Require(!IsRunning(first), "Restart stops the old child before launching replacement");
					File.WriteAllText(Path.Combine(serviceDirectory, "exit"), "");
					WaitUntil(() => MlServiceConnection.Read(host.Id).Message.Contains("exit code 17"));
					Thread.Sleep(200);
					Require(File.ReadAllLines(starts).Length == 2, "Exited service stays stopped without automatic retry");
					File.Delete(Path.Combine(serviceDirectory, "exit"));
					MlServiceConnection.RequestRestart(host.Id);
					WaitUntil(() => File.ReadAllLines(starts).Length == 3);
					Require(true, "Manual restart recovers an exited service");
					WaitUntil(() => MlServiceConnection.Read(host.Id, ".TestControl").CanRestart);
					using (var audio = MemoryMappedFile.CreateNew("Local\\RSModsPlus.MlAudio.v2.TestControl", 64))
					using (var result = MemoryMappedFile.CreateNew("Local\\RSModsPlus.MlStringFret.v2.TestControl", 112))
					using (var input = audio.CreateViewAccessor())
					using (var output = result.CreateViewAccessor())
					{
						input.Write(0, 0x4C4D5352u); input.Write(4, 2u); input.Write(8, 48000u); input.Write(16, 96000UL);
						output.Write(0, 0x46535352u); output.Write(4, 2u); output.Write(88, GetTickCount64()); output.Write(96, 95000UL); output.Write(104, 48000u);
						Require(MlServiceConnection.Read(host.Id, ".TestControl").IsConnected, "Fresh results establish connection");
						output.Write(88, GetTickCount64() - 1000);
						Require(!MlServiceConnection.Read(host.Id, ".TestControl").IsConnected, "Stale publication loses connection");
						output.Write(88, GetTickCount64()); output.Write(96, 1UL);
						Require(!MlServiceConnection.Read(host.Id, ".TestControl").IsConnected, "Stale audio loses connection even with fresh publication");
						output.Write(4, 99u);
						Require(MlServiceConnection.Read(host.Id, ".TestControl").Message.Contains("mismatch"), "Protocol mismatch is explicit");
					}
					File.Delete(Path.Combine(directory, "rsmodsplus.dll"));
					MlServiceConnection.RequestRestart(host.Id);
					WaitUntil(() => MlServiceConnection.Read(host.Id).Message.Contains("files missing"));
					Require(File.ReadAllLines(starts).Length == 3, "Missing package reports failure without starting a child");
					File.WriteAllText(Path.Combine(directory, "rsmodsplus.dll"), "isolated launcher fixture");
					MlServiceConnection.RequestRestart(host.Id);
					WaitUntil(() => File.ReadAllLines(starts).Length == 4);
					Require(true, "Manual restart recovers after missing package is restored");
					var last = int.Parse(File.ReadAllLines(starts)[3]);
					File.WriteAllText(Path.Combine(directory, "stop"), "");
					Require(host.WaitForExit(5000), "Host shuts down");
					WaitUntil(() => !IsRunning(last));
					Require(true, "Host shutdown closes its ML child");
				}
				finally
				{
					if (!host.HasExited) { host.Kill(); host.WaitForExit(); }
				}
			}
		}

		private static bool IsRunning(int processId)
		{
			try { using (var process = Process.GetProcessById(processId)) return !process.HasExited; }
			catch (ArgumentException) { return false; }
		}
	}
}
