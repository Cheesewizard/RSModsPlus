using System;
using System.IO;
using System.Security.Cryptography;

namespace RSModsPlus.MachineLearning
{
	public static class EmbeddedModel
	{
		private const string RESOURCE_NAME = "RSModsPlus.MachineLearning.FretNet.onnx";

		public static byte[] ReadBytes()
		{
			using (var stream = typeof(EmbeddedModel).Assembly.GetManifestResourceStream(RESOURCE_NAME))
			{
				if (stream == null) throw new InvalidOperationException("The RSModsPlus assembly does not contain its FretNet model.");
				if (stream.Length == 0) throw new InvalidOperationException("The embedded FretNet model is empty.");

				using (var buffer = new MemoryStream())
				{
					stream.CopyTo(buffer);
					return buffer.ToArray();
				}
			}
		}

		public static string GetSha256()
		{
			using (var algorithm = SHA256.Create())
			{
				return BitConverter.ToString(algorithm.ComputeHash(ReadBytes())).Replace("-", "").ToLowerInvariant();
			}
		}
	}
}
