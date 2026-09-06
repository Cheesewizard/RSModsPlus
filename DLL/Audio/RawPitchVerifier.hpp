#pragma once

#include <cstdint>

// Tier 0 of the modern detection stack (issue #29; enabled by the #18 tap finding): a
// Goertzel measurement bank over the RAW input route samples, observed at the same
// GetBuffer choke point the input processors run at - the exact audio the game's
// detector receives. The game's own spectral features are semitone-quantized with a
// measured, inconsistent +/-1 read bias, which makes adjacent-fret discrimination
// impossible downstream of them (proven live 2026-08-31); this module measures energy
// at EXACT frequencies (cent resolution) upstream of that quantization, answering the
// one question Note by Note needs: "is the expected note's fundamental actually
// sounding, versus its one-fret neighbours?" Deterministic DSP, no model, and the NBN
// freeze makes it latency-tolerant.
namespace RawPitchVerifier
{
	// Goertzel powers around a target fundamental. Powers are normalized by
	// (windowSampleCount/2)^2 so a full-scale sine at the bin reads ~1.0 regardless of
	// window length; compare them against each other, not against absolute thresholds.
	struct ToneEvidence
	{
		uint32_t sampleRate = 0;
		uint32_t windowSampleCount = 0;
		float totalRms = 0.0f;
		float targetPower = 0.0f;
		float minusOnePower = 0.0f;   // one semitone below the target
		float plusOnePower = 0.0f;    // one semitone above
		float minusTwoPower = 0.0f;
		float plusTwoPower = 0.0f;
	};

	// Audio-thread feed: append post-processing route samples (mono float) for Player 1
	// (routeIndex 0). Other routes are ignored for now. Silent buffers should be fed as
	// zeros so the ring's timeline stays continuous.
	void Observe(uint32_t routeIndex, const float* samples, uint32_t count, uint32_t sampleRate);

	// True once the ring holds at least one full window at a known sample rate.
	bool IsReady();

	// Measure the last windowSeconds of route audio at frequencyHz and its +/-1 and
	// +/-2 semitone neighbours. Safe from any thread; returns false when not enough
	// audio has been observed yet. windowSeconds is clamped to [0.05, 0.3].
	bool QueryToneEvidence(double frequencyHz, float windowSeconds, ToneEvidence& out);
}
