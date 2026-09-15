#pragma once

// Output loudness guard: a slow loudness AGC (equalises perceived loudness between songs) followed by
// a fast true-peak look-ahead brickwall limiter (a real ceiling that pins transients, for hearing
// safety). Shared by the proxy (which runs it on the game output before the real driver plays it) and
// an offline test. Pure and header-only so it stays unit-testable with synthetic signals, no game or
// hardware. See docs/designs/audio-bridge-output-loudness-guard.md.
//
// Layout matches the proxy: audio is planar (one buffer per channel). Per output buffer the caller:
//   1) computes each channel's mean-square and calls BeginBlock(...) once  -> advances the shared AGC gain
//   2) calls ProcessChannel(ch, x, frames) for each channel               -> applies AGC gain then limits
// Both stages are individually toggleable. With both off the signal is bit-for-bit unchanged and no
// latency is added. With the limiter on, output is delayed by the look-ahead length (a few ms); the
// AGC alone adds no latency.
#include <vector>
#include <cmath>

namespace OutputGuardDsp
{
	struct Config
	{
		float sampleRate = 48000.0f;

		// Limiter (safety ceiling). Off by default = unity passthrough, no latency.
		bool  limiterOn = false;
		float ceilingLin = 1.0f;    // linear peak ceiling in [0,1]; nothing leaves above this
		float lookaheadMs = 3.0f;   // look-ahead so the gain is down before the peak arrives
		float releaseMs = 120.0f;   // how fast gain recovers after a transient

		// Loudness AGC (song-to-song equalisation). Off by default = no gain change, no latency.
		bool  agcOn = false;
		float targetRms = 0.10f;    // aim perceived loudness (RMS) here; keep a few dB below the ceiling
		float agcMaxBoostDb = 6.0f; // never boost more than this (keeps quiet passages from getting loud)
		float agcMaxCutDb = 18.0f;  // may cut this much to tame a hot song
		float agcTimeMs = 1500.0f;  // slow: equalise between songs, not within one
		float agcFloorRms = 0.005f; // below this the signal is treated as silence/noise: hold gain, no boost
	};

	class OutputGuard
	{
	public:
		void Configure(const Config& c)
		{
			m_cfg = c;
			const float sr = c.sampleRate > 0.0f ? c.sampleRate : 48000.0f;

			m_look = static_cast<int>(c.lookaheadMs * 0.001f * sr + 0.5f);
			if (m_look < 1) m_look = 1;
			// Attack reaches the target within the look-ahead window (~e^-4 residual over m_look samples).
			m_attack = 1.0f - std::exp(-4.0f / static_cast<float>(m_look));
			const float relSamples = c.releaseMs * 0.001f * sr;
			m_release = relSamples > 0.0f ? (1.0f - std::exp(-1.0f / relSamples)) : 1.0f;

			const float agcSamples = c.agcTimeMs * 0.001f * sr;
			m_agcBlockDecayPerSample = agcSamples > 0.0f ? (1.0f / agcSamples) : 1.0f;
			m_agcMinGain = std::pow(10.0f, -c.agcMaxCutDb / 20.0f);
			m_agcMaxGain = std::pow(10.0f, c.agcMaxBoostDb / 20.0f);

			ResizeChannels(static_cast<int>(m_chans.size()));
			ResetState();
		}

		// Drop all history: ring buffers to silence, gains to unity. Call on (re)start.
		void Reset(int channels)
		{
			ResizeChannels(channels);
			ResetState();
		}

		// Advance the shared AGC gain from this block's loudness. channelMeanSquare[i] is the mean of the
		// squared samples for channel i over `frames`; the block loudness is their average. Cheap: once per
		// buffer, not per sample.
		void BeginBlock(const double* channelMeanSquare, int channels, int frames)
		{
			if (channels > static_cast<int>(m_chans.size())) ResizeChannels(channels);
			if (!m_cfg.agcOn || channels <= 0 || frames <= 0) return;

			double agg = 0.0;
			for (int i = 0; i < channels; ++i) agg += channelMeanSquare[i];
			agg /= channels;

			// Smooth the mean-square toward this block over the AGC time constant.
			const double a = 1.0 - std::pow(1.0 - static_cast<double>(m_agcBlockDecayPerSample), frames);
			m_envMs += (agg - m_envMs) * a;

			const double rms = std::sqrt(m_envMs > 0.0 ? m_envMs : 0.0);
			double desired = m_agcGain;
			if (rms > m_cfg.agcFloorRms)
			{
				desired = m_cfg.targetRms / rms;
				if (desired < m_agcMinGain) desired = m_agcMinGain;
				if (desired > m_agcMaxGain) desired = m_agcMaxGain;
			}
			// Move the applied gain slowly toward the target (second smoothing = no zipper on the gain).
			m_agcGain += (desired - m_agcGain) * a;
		}

		// Live scalar updates safe to call from the audio thread: they only flip a flag or move a level,
		// never resize a buffer, so the user can drag the slider / toggle a stage with no allocation or race.
		// Timing params (look-ahead, release, AGC time constant) and channel count are fixed by Configure.
		void SetLimiterLive(bool on, float ceilingLin) { m_cfg.limiterOn = on; m_cfg.ceilingLin = ceilingLin; }
		void SetAgcLive(bool on, float targetRms) { m_cfg.agcOn = on; m_cfg.targetRms = targetRms; }

		float AgcGain() const { return static_cast<float>(m_agcGain); }
		int   LookaheadSamples() const { return m_look; }

		// Process one channel's planar buffer in place: apply the shared AGC gain, then the look-ahead
		// brickwall limiter. When the limiter is off there is no delay (zero added latency).
		void ProcessChannel(int ch, float* x, int frames)
		{
			if (ch < 0 || !x || frames <= 0) return;
			if (ch >= static_cast<int>(m_chans.size())) ResizeChannels(ch + 1);
			Chan& c = m_chans[ch];
			const bool agc = m_cfg.agcOn;
			const bool lim = m_cfg.limiterOn && m_cfg.ceilingLin > 0.0f;
			const float g = static_cast<float>(m_agcGain);
			const float ceil = m_cfg.ceilingLin;

			for (int i = 0; i < frames; ++i)
			{
				float s = x[i];
				if (agc) s *= g;

				if (!lim) { x[i] = s; continue; }

				// Delay line: emit the sample from m_look ago, store the incoming one.
				const float delayed = c.delay[c.widx];
				c.delay[c.widx] = s;
				c.widx = (c.widx + 1 == m_look) ? 0 : c.widx + 1;

				// Gain reacts to the incoming (future) sample so it is already down when that sample emerges.
				const float mag = s < 0.0f ? -s : s;
				const float target = (mag > ceil) ? (ceil / mag) : 1.0f;
				c.gain += (target < c.gain ? m_attack : m_release) * (target - c.gain);

				float out = delayed * c.gain;
				if (out > ceil) out = ceil; else if (out < -ceil) out = -ceil; // hard backstop: guarantees the ceiling
				x[i] = out;
			}
		}

	private:
		struct Chan
		{
			std::vector<float> delay; // look-ahead ring, m_look samples
			int widx = 0;
			float gain = 1.0f;        // current limiter gain, <= 1
		};

		void ResizeChannels(int channels)
		{
			if (channels < 0) channels = 0;
			m_chans.resize(channels);
			for (auto& c : m_chans) c.delay.assign(m_look > 0 ? m_look : 1, 0.0f);
		}

		void ResetState()
		{
			m_agcGain = 1.0;
			m_envMs = 0.0;
			for (auto& c : m_chans)
			{
				std::fill(c.delay.begin(), c.delay.end(), 0.0f);
				c.widx = 0;
				c.gain = 1.0f;
			}
		}

		Config m_cfg;
		int   m_look = 144;
		float m_attack = 0.03f;
		float m_release = 0.0002f;
		float m_agcBlockDecayPerSample = 1.0f / 72000.0f;
		float m_agcMinGain = 0.125f;
		float m_agcMaxGain = 2.0f;

		double m_agcGain = 1.0;
		double m_envMs = 0.0;
		std::vector<Chan> m_chans;
	};
}
