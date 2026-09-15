#pragma once

#include "CaptureFormat.h"
#include "IInputProcessor.hpp"

#include <cstddef>

namespace Audio::AsioHook
{
	constexpr size_t INPUT_ROUTE_COUNT = 2;

	// Validates and chains RS_ASIO's existing PortAudio unmarshal patch. This observes the
	// IAudioCaptureClient RS_ASIO already created instead of creating another ASIO host.
	void Install();

	// Called from the game loop so buffers and processor state are prepared off the audio thread.
	void Poll();

	// Ownership stays with the caller. Active endpoints are assigned in RS_ASIO.ini order.
	void SetProcessor(size_t routeIndex, IInputProcessor* inputProcessor);

	// A source stage that runs BEFORE the route's processor, overwriting the captured input
	// when armed. Lets a synthetic-input harness feed the real processor chain (e.g. the Drop
	// Pedal shifter). Ownership stays with the caller; a disarmed source is inert.
	void SetInputSource(size_t routeIndex, IInputProcessor* inputSource);
	void SetInputSourceActive(size_t routeIndex, bool active);

	// Make-up gain (dB) applied to the real guitar input before the game's amp and note gate.
	// Compensates for RS_ASIO/interface inputs arriving quieter than a hot Real Tone Cable, which
	// otherwise makes the game's level-sensitive gate mute sustains and bends early. 0 dB = off.
	void SetInputGainDb(float decibels);

	// Soft noise gate (downward expander) threshold in dBFS, keyed to the raw pre-gain input. It ducks
	// the between-note hiss the make-up gain would amplify, without chopping sustain. >= 0 dB = off.
	void SetNoiseGateThresholdDb(float decibels);
	float GetNoiseGateThresholdDb();

	// Input compressor strength (0..1, 0 = off). Flattens the natural string-beat wobble before the game
	// amp so a quiet interface input doesn't warble the way a hot cable's compressed signal doesn't.
	void SetCompressorStrength(float strength);
	float GetCompressorStrength();

	// Mains-hum notch base frequency in Hz (0 = off, else 50 or 60). Notches out the 50/60 Hz ground-loop
	// hum comb a grounded interface injects and a single-USB Real Tone Cable does not; front of chain, so
	// it cleans the raw input before the gate/gain and the game see it.
	void SetHumFilterBaseHz(float baseHz);
	float GetHumFilterBaseHz();

	// Round-trip latency measurement (paired with the proxy's probe injection): arm a one-shot capture of
	// `frames` route-0 input samples (the looped-back probe), then the host correlates them against the probe.
	void StartLatencyCapture(int frames);
	bool IsLatencyCaptureDone();
	int GetLatencyCapture(const float** out);   // returns frames captured so far; *out = buffer
	float GetInputGainDb();

	void SetProcessingEnabled(bool enabled);
	bool IsProcessingEnabled();
	bool IsInputConfigured(size_t routeIndex);
	bool IsInputReady(size_t routeIndex);
}
