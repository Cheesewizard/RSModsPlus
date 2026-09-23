// Offline unit test for the output loudness guard (AGC + look-ahead brickwall limiter). No game, no
// hardware: feed synthetic planar blocks and assert the ceiling is genuinely held, that sub-ceiling
// signal passes unattenuated, and that the AGC pulls loudness toward the target without boosting
// near-silence. Exit 0 = pass. Mirrors the proxy's planar, per-channel-per-block call pattern.
#include "../OutputGuardDsp.h"
#include <cstdio>
#include <cmath>
#include <vector>

static const int FRAMES = 4800;   // 100 ms at 48 kHz
static const int CH = 2;
static const float SR = 48000.0f;

// One planar channel buffer of a sine at the given linear amplitude.
static std::vector<float> Sine(float amp, float freq = 220.0f, int frames = FRAMES)
{
	std::vector<float> x(frames);
	for (int f = 0; f < frames; ++f) x[f] = amp * std::sin(2.0f * 3.14159265f * freq * f / SR);
	return x;
}

static float Peak(const std::vector<float>& x)
{
	float p = 0.0f;
	for (float v : x) { const float a = std::fabs(v); if (a > p) p = a; }
	return p;
}

static float Rms(const std::vector<float>& x)
{
	double s = 0.0;
	for (float v : x) s += static_cast<double>(v) * v;
	return static_cast<float>(std::sqrt(s / x.size()));
}

static double MeanSquare(const std::vector<float>& x)
{
	double s = 0.0;
	for (float v : x) s += static_cast<double>(v) * v;
	return s / x.size();
}

// Run `blocks` identical stereo blocks of amplitude `amp` through the guard, returning channel 0 of the
// last block (so AGC has converged and the limiter has passed its warm-up).
static std::vector<float> RunSteady(OutputGuardDsp::OutputGuard& g, float amp, int blocks, float freq = 220.0f)
{
	std::vector<float> ch0;
	for (int b = 0; b < blocks; ++b)
	{
		std::vector<float> c0 = Sine(amp, freq), c1 = Sine(amp, freq);
		const double ms[CH] = { MeanSquare(c0), MeanSquare(c1) };
		g.BeginBlock(ms, CH, FRAMES);
		g.ProcessChannel(0, c0.data(), FRAMES);
		g.ProcessChannel(1, c1.data(), FRAMES);
		ch0 = c0;
	}
	return ch0;
}

static bool Check(const char* name, bool cond, float got, float expect)
{
	printf("  %-38s got=%.4f expect~%.4f -> %s\n", name, got, expect, cond ? "ok" : "FAIL");
	return cond;
}

int main()
{
	bool pass = true;

	// 1. Both stages off: bit-for-bit passthrough, no delay.
	{
		OutputGuardDsp::Config cfg; cfg.sampleRate = SR; // limiterOn/agcOn default false
		OutputGuardDsp::OutputGuard g; g.Configure(cfg); g.Reset(CH);
		auto x = Sine(0.3f), ref = x;
		double ms[CH] = { MeanSquare(x), MeanSquare(x) };
		g.BeginBlock(ms, CH, FRAMES);
		g.ProcessChannel(0, x.data(), FRAMES);
		bool same = true; for (size_t i = 0; i < x.size(); ++i) if (x[i] != ref[i]) { same = false; break; }
		pass &= Check("both off: passthrough unchanged", same, Peak(x), 0.3f);
	}

	// 2. Ceiling is genuinely held: a hot 0.9 sine never leaves above a 0.5 ceiling.
	{
		OutputGuardDsp::Config cfg; cfg.sampleRate = SR; cfg.limiterOn = true; cfg.ceilingLin = 0.5f;
		OutputGuardDsp::OutputGuard g; g.Configure(cfg); g.Reset(CH);
		auto out = RunSteady(g, 0.9f, 4);
		float over = 0.0f; for (float v : out) { float a = std::fabs(v); if (a > over) over = a; }
		pass &= Check("limiter: 0.9 into 0.5 ceiling held", over <= 0.5f + 1e-4f, over, 0.5f);
	}

	// 3. Sub-ceiling signal passes essentially unattenuated (limiter idles): 0.3 into a 0.5 ceiling stays ~0.3.
	{
		OutputGuardDsp::Config cfg; cfg.sampleRate = SR; cfg.limiterOn = true; cfg.ceilingLin = 0.5f;
		OutputGuardDsp::OutputGuard g; g.Configure(cfg); g.Reset(CH);
		auto out = RunSteady(g, 0.3f, 4);
		pass &= Check("limiter: 0.3 under 0.5 ceiling untouched", std::fabs(Peak(out) - 0.3f) < 0.01f, Peak(out), 0.3f);
	}

	// 4. A single loud spike inside a quiet block is still capped at the ceiling (look-ahead catches it).
	{
		OutputGuardDsp::Config cfg; cfg.sampleRate = SR; cfg.limiterOn = true; cfg.ceilingLin = 0.5f;
		OutputGuardDsp::OutputGuard g; g.Configure(cfg); g.Reset(CH);
		// Warm up quiet, then a block with a burst to 0.95 in the middle.
		RunSteady(g, 0.1f, 2);
		std::vector<float> c0 = Sine(0.1f), c1 = Sine(0.1f);
		for (int f = 2000; f < 2100; ++f) { c0[f] = 0.95f; c1[f] = 0.95f; }
		double ms[CH] = { MeanSquare(c0), MeanSquare(c1) };
		g.BeginBlock(ms, CH, FRAMES);
		g.ProcessChannel(0, c0.data(), FRAMES);
		pass &= Check("limiter: 0.95 spike capped", Peak(c0) <= 0.5f + 1e-4f, Peak(c0), 0.5f);
	}

	// 5. AGC boosts a quiet song toward the target (within the boost cap).
	{
		OutputGuardDsp::Config cfg; cfg.sampleRate = SR; cfg.agcOn = true; cfg.targetRms = 0.05f;
		OutputGuardDsp::OutputGuard g; g.Configure(cfg); g.Reset(CH);
		auto out = RunSteady(g, 0.03f, 200);  // input RMS ~0.021; target 0.05 needs ~+7.5 dB, capped near +6
		pass &= Check("agc: quiet lifted toward target", Rms(out) > 0.035f, Rms(out), 0.05f);
	}

	// 6. AGC cuts a hot song toward the target.
	{
		OutputGuardDsp::Config cfg; cfg.sampleRate = SR; cfg.agcOn = true; cfg.targetRms = 0.10f;
		OutputGuardDsp::OutputGuard g; g.Configure(cfg); g.Reset(CH);
		auto out = RunSteady(g, 0.6f, 200);   // input RMS ~0.42; should be pulled down near 0.10
		pass &= Check("agc: hot pulled toward target", std::fabs(Rms(out) - 0.10f) < 0.03f, Rms(out), 0.10f);
	}

	// 7. AGC does NOT boost near-silence: sub-floor signal keeps ~unity gain (hearing-safety guard).
	{
		OutputGuardDsp::Config cfg; cfg.sampleRate = SR; cfg.agcOn = true; cfg.targetRms = 0.10f; cfg.agcFloorRms = 0.005f;
		OutputGuardDsp::OutputGuard g; g.Configure(cfg); g.Reset(CH);
		RunSteady(g, 0.003f, 100);            // RMS ~0.0021, below the 0.005 floor
		pass &= Check("agc: near-silence not boosted", std::fabs(g.AgcGain() - 1.0f) < 0.05f, g.AgcGain(), 1.0f);
	}

	// 8. Combined: a hot song is equalised toward target AND never exceeds the ceiling.
	{
		OutputGuardDsp::Config cfg; cfg.sampleRate = SR;
		cfg.agcOn = true; cfg.targetRms = 0.10f; cfg.limiterOn = true; cfg.ceilingLin = 0.5f;
		OutputGuardDsp::OutputGuard g; g.Configure(cfg); g.Reset(CH);
		auto out = RunSteady(g, 0.9f, 200);
		bool held = Peak(out) <= 0.5f + 1e-4f;
		bool near = std::fabs(Rms(out) - 0.10f) < 0.03f;
		pass &= Check("combined: equalised", near, Rms(out), 0.10f);
		pass &= Check("combined: ceiling held", held, Peak(out), 0.5f);
	}

	// 9. True-peak: a signal whose SAMPLES all sit under the ceiling but whose reconstructed waveform
	//    overshoots it (an inter-sample peak) must still be pulled down. A 12 kHz tone at 48 kHz with a
	//    45 deg phase lands every sample at +-0.707*A while the true peak is A. With A=0.6 and a 0.5 ceiling
	//    the sample peak (~0.424) never trips a sample-only limiter, but the true peak (0.6) exceeds it.
	{
		const float A = 0.6f, f = 12000.0f, ph = 0.7853982f; // 45 deg
		auto isp = [&](void) {
			std::vector<float> x(FRAMES);
			for (int n = 0; n < FRAMES; ++n) x[n] = A * std::sin(2.0f * 3.14159265f * f * n / SR + ph);
			return x;
		};

		// Sample-peak only: the limiter idles because no sample exceeds the ceiling.
		OutputGuardDsp::Config off; off.sampleRate = SR; off.limiterOn = true; off.ceilingLin = 0.5f; off.truePeak = false;
		OutputGuardDsp::OutputGuard goff; goff.Configure(off); goff.Reset(CH);
		std::vector<float> a = isp(), b = isp();
		double msoff[CH] = { MeanSquare(a), MeanSquare(b) };
		goff.BeginBlock(msoff, CH, FRAMES); goff.ProcessChannel(0, a.data(), FRAMES);
		const float peakOff = Peak(a);

		// True-peak on: sees the ~0.6 inter-sample peak and eases the level down, leaving headroom.
		OutputGuardDsp::Config on = off; on.truePeak = true;
		OutputGuardDsp::OutputGuard gon; gon.Configure(on); gon.Reset(CH);
		std::vector<float> c = isp(), d = isp();
		double mson[CH] = { MeanSquare(c), MeanSquare(d) };
		gon.BeginBlock(mson, CH, FRAMES); gon.ProcessChannel(0, c.data(), FRAMES);
		const float peakOn = Peak(c);

		pass &= Check("true-peak: sample-only idles (no reduction)", std::fabs(peakOff - 0.4243f) < 0.02f, peakOff, 0.4243f);
		pass &= Check("true-peak: ISP detected, level eased down", peakOn < peakOff * 0.95f && peakOn > 0.2f, peakOn, 0.354f);
	}

	printf(pass ? "PASS: output guard holds the ceiling and equalises loudness.\n" : "FAIL: output guard.\n");
	return pass ? 0 : 1;
}
