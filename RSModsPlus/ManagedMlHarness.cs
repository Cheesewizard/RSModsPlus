using System;
using RSModsPlus.MachineLearning;

namespace RSModsPlus.Verification
{
	internal static class ManagedMlHarness
	{
		private static int Main(string[] arguments)
		{
				try
				{
					if (arguments.Length != 2) throw new ArgumentException("Expected parent PID and isolated channel suffix.");
					var suffix = arguments[1];
					MlService.Run(int.Parse(arguments[0]), Console.WriteLine, suffix);
					return 0;
				}
				catch (Exception exception)
				{
					Console.Error.WriteLine(exception);
					return 1;
				}
		}

	}
}
