// Offline unit test for the output meter: peak-hold snaps up to a transient then decays, and RMS smooths
// toward the steady level. No game, no hardware. Exit 0 = pass.
#include "../MeterDsp.h"
#include <cstdio>
#include <cmath>

static bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

int main()
{
	bool pass = true;
	MeterDsp::ChannelMeter m;

	// A loud block snaps peak-hold up immediately.
	m.Update(0.9f, 0.7f);
	bool c1 = Near(m.peakHold, 0.9f, 1e-6f);
	printf("  peak snaps to transient: %.3f -> %s\n", m.peakHold, c1 ? "ok" : "FAIL"); pass &= c1;

	// Silence after: peak-hold decays but does not vanish immediately; RMS falls toward 0.
	for (int i = 0; i < 10; ++i) m.Update(0.0f, 0.0f);
	bool c2 = m.peakHold < 0.9f && m.peakHold > 0.2f;
	printf("  peak-hold decays gradually: %.3f -> %s\n", m.peakHold, c2 ? "ok" : "FAIL"); pass &= c2;

	// Sustained level: RMS converges toward it.
	MeterDsp::ChannelMeter s;
	for (int i = 0; i < 100; ++i) s.Update(0.5f, 0.35f);
	bool c3 = Near(s.rms, 0.35f, 0.01f) && Near(s.peakHold, 0.5f, 0.01f);
	printf("  RMS converges to steady 0.35: rms=%.3f peak=%.3f -> %s\n", s.rms, s.peakHold, c3 ? "ok" : "FAIL"); pass &= c3;

	printf(pass ? "PASS: meter holds peaks and smooths RMS.\n" : "FAIL: meter.\n");
	return pass ? 0 : 1;
}
