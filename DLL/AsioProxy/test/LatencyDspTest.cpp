// Offline unit test for the round-trip latency DSP. No game, no hardware: synthesize a "loopback"
// (the probe delayed by a known lag, attenuated, with added noise) and assert FindLag recovers the lag.
// Exit 0 = pass. Built + run by run-tests.sh.
#include "../LatencyDsp.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static unsigned g_rng = 0x1234567u;
static float Noise(float amp) { g_rng = g_rng * 1664525u + 1013904223u; return amp * ((g_rng >> 8) / 8388608.0f - 1.0f); }

// One case: place the probe at trueLag in a recording, scaled by `level`, with `noise` added everywhere.
static bool Case(int trueLag, float level, float noise, int maxLag)
{
	const int refLen = 256;
	std::vector<float> probe(refLen);
	LatencyDsp::GenerateProbe(probe.data(), refLen);

	const int recLen = maxLag + refLen + 64;
	std::vector<float> recorded(recLen, 0.0f);
	for (int i = 0; i < recLen; ++i) recorded[i] = Noise(noise);
	for (int i = 0; i < refLen; ++i) recorded[trueLag + i] += level * probe[i];

	LatencyDsp::Match m = LatencyDsp::FindLag(probe.data(), refLen, recorded.data(), recLen, maxLag);
	const bool ok = (m.lag >= trueLag - 1 && m.lag <= trueLag + 1) && m.confidence > 0.5f;
	printf("  lag=%d (true %d) conf=%.3f level=%.2f noise=%.3f -> %s\n",
		m.lag, trueLag, m.confidence, level, noise, ok ? "ok" : "FAIL");
	return ok;
}

int main()
{
	bool pass = true;
	pass &= Case(137, 1.0f, 0.00f, 2400);   // clean electrical loopback
	pass &= Case(512, 0.5f, 0.02f, 2400);   // quieter return, some noise
	pass &= Case(48,  0.7f, 0.05f, 2400);   // short latency, more noise
	pass &= Case(2005, 0.6f, 0.03f, 2400);  // near the far end of the search window

	// Negative control: no probe present at all -> must NOT report a confident lag.
	{
		const int refLen = 256, recLen = 4000;
		std::vector<float> probe(refLen); LatencyDsp::GenerateProbe(probe.data(), refLen);
		std::vector<float> noiseOnly(recLen);
		for (int i = 0; i < recLen; ++i) noiseOnly[i] = Noise(0.1f);
		LatencyDsp::Match m = LatencyDsp::FindLag(probe.data(), refLen, noiseOnly.data(), recLen, 2400);
		const bool ok = m.confidence < 0.5f;
		printf("  no-probe control: conf=%.3f -> %s\n", m.confidence, ok ? "ok" : "FAIL");
		pass &= ok;
	}

	printf(pass ? "PASS: latency DSP recovers known delays and rejects noise.\n" : "FAIL: latency DSP.\n");
	return pass ? 0 : 1;
}
