using System;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Threading;

namespace RSModsPlus.MachineLearning
{
	internal static class ChartExpectationReader
	{
		public static int[] Read(MemoryMappedViewAccessor input)
		{
			for (var attempt = 0; attempt < 4; attempt++)
			{
				var sequence = input.ReadUInt32(48);
				if ((sequence & 1) != 0) continue;
				Thread.MemoryBarrier();
				var expectedString = input.ReadInt32(52);
				var expectedMidi = input.ReadInt32(56);
				var tick = input.ReadUInt32(60);
				Thread.MemoryBarrier();
				if (sequence != input.ReadUInt32(48)) continue;
				if (sequence == 0 || unchecked((uint)Environment.TickCount - tick) > 1000) throw new IOException("Expected-note context is unavailable or stale.");
				return new[] { expectedString, expectedMidi };
			}
			throw new IOException("Expected-note context changed during the read.");
		}
	}
}
