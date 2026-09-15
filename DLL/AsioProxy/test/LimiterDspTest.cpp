// Offline unit test for the static output trim. No game, no hardware: feed synthetic blocks and check
// that the gain scales the signal linearly, that unity is untouched, and that the shape is preserved
// (no clipping or compression). Exit 0 = pass.
#include "../LimiterDsp.h"
#include <cstdio>
#include <cmath>
#include <vector>

static const int FRAMES = 4800;   // 100 ms at 48 kHz
static const int CH = 2;

// Fill an interleaved stereo block with a sine of the given linear amplitude.
static std::vector<float> Sine(float amp, float freq = 220.0f)
{
	std::vector<float> x(FRAMES * CH);
	for (int f = 0; f < FRAMES; ++f)
	{
		const float v = amp * std::sin(2.0f * 3.14159265f * freq * f / 48000.0f);
		x[f * CH] = v; x[f * CH + 1] = v;
	}
	return x;
}

static float Peak(const std::vector<float>& x)
{
	float peak = 0.0f;
	for (float v : x) { const float a = std::fabs(v); if (a > peak) peak = a; }
	return peak;
}

static bool Check(const char* name, bool cond, float got, float expect)
{
	printf("  %-30s got=%.4f expect~%.4f -> %s\n", name, got, expect, cond ? "ok" : "FAIL");
	return cond;
}

int main()
{
	bool pass = true;

	// 1. Passthrough: gain 1 -> Active() false, signal bit-for-bit unchanged.
	{
		LimiterDsp::Trim trim; trim.Configure(1.0f);
		auto x = Sine(0.3f); auto ref = x;
		trim.Process(x.data(), FRAMES, CH);
		bool same = true; for (size_t i = 0; i < x.size(); ++i) if (x[i] != ref[i]) { same = false; break; }
		pass &= Check("passthrough unchanged", same && !trim.Active(), Peak(x), 0.3f);
	}

	// 2. A -6 dB trim (gain ~0.5) halves the amplitude, exactly and everywhere.
	{
		LimiterDsp::Trim trim; trim.Configure(0.5f);
		auto x = Sine(0.8f);
		trim.Process(x.data(), FRAMES, CH);
		pass &= Check("gain 0.5 (0.8 -> 0.4)", std::fabs(Peak(x) - 0.4f) < 0.001f, Peak(x), 0.4f);
	}

	// 3. Linearity / no clipping: scaling a hot signal keeps a clean sine (peak scales, shape intact).
	{
		LimiterDsp::Trim trim; trim.Configure(0.25f);
		auto x = Sine(1.0f); auto ref = Sine(1.0f);
		trim.Process(x.data(), FRAMES, CH);
		bool exact = true; for (size_t i = 0; i < x.size(); ++i) if (std::fabs(x[i] - ref[i] * 0.25f) > 1e-6f) { exact = false; break; }
		pass &= Check("gain 0.25 scales, no clip", exact, Peak(x), 0.25f);
	}

	printf(pass ? "PASS: trim scales linearly, preserves shape, and passes through when off.\n" : "FAIL: trim.\n");
	return pass ? 0 : 1;
}
