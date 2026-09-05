#pragma once

#include <string>

// Modern WASAPI capture for the Real Tone Cable (and any other PortAudio WASAPI input).
//
// Rocksmith drives its guitar input through a 2012-era PortAudio whose WASAPI backend only
// knows two shapes: legacy shared mode (polled, ~22 ms) and exclusive mode. The Real Tone
// Cable's driver has NO exclusive input format, so ExclusiveMode=1 fails the open with
// -9996 ("cable not detected") and ExclusiveMode=0 leaves a 22 ms polled stream. Both are
// decided by ini state the player can't reliably edit (issue #76).
//
// This module removes the ini from the equation. It wraps the IAudioClient PortAudio
// activates for a capture endpoint in a mod-owned client that always opens a modern
// shared-mode, event-driven stream (IAudioClient3 low-latency period when Windows 10+
// offers it, legacy shared with format auto-conversion otherwise) while presenting to
// PortAudio exactly the exclusive, event-driven stream it asked for. PortAudio's own
// stream layout, the GetBuffer tap in AsioHook, the Drop Pedal shifter and Note by Note
// all keep working unchanged: they see the same PaWasapiStream and the same
// IAudioCaptureClient interface, just fed by a better engine.
//
// Output (the Wwise sink) is never touched.
namespace Audio::CableInput
{
	// Detours Pa_OpenStream and prepares the IMMDevice::Activate wrapper. Call once at
	// startup, before the game opens its input stream. Safe to call with RS_ASIO present
	// (it then does nothing: RS_ASIO owns the input).
	void Install();

	// Game-loop liveness reporting: logs whether the wrapped input is delivering audio,
	// and when it stalls or resumes. Never runs on the audio thread.
	void Poll();

	// One-line human summary of the active input path for overlays and diagnostics.
	std::string DescribeStatus();

	// Snapshot for the in-game audio overlay (latency, signal, health). Cheap to copy; safe
	// from the render thread.
	struct Diagnostics
	{
		bool installed = false;        // the cable client is in place (RS_ASIO absent, detour ok)
		bool rsAsio = false;           // RS_ASIO owns the input; nothing wrapped
		bool streamActive = false;     // an input stream has been started
		std::string inputPath;         // "modern raw", "modern", "engine convert", "legacy shared"
		std::string inputFormat;       // what the game receives
		double inputLatencyMs = 0.0;   // one served chunk (the period) in ms; 0 = unknown
		double inputLatencyGameMs = 0.0; // PortAudio's own figure from audiodump.txt; 0 = unknown
		double outputLatencyMs = 0.0;  // the game's figure from audiodump.txt; 0 = unknown
		bool outputExclusive = false;
		bool outputKnown = false;
		double packetsPerSecond = 0.0;
		bool stalled = false;
		float meterPeak = 0.0f;        // decaying linear peak (0..1) for the signal bar
		std::string deviceFormat;      // the Windows format the cable came up with ("16 kHz")
		uint64_t dropouts = 0;         // packets discarded because the game's audio thread fell behind
		double measuredInputMs = 0.0;  // MEASURED: mean sample age when the game takes a packet (see ReportMeasuredInputAge)
		bool measuredValid = false;    // at least one measurement has been taken this stream
	};
	Diagnostics GetDiagnostics();

	// Feeds the measured input latency. Called on the audio thread with the MEAN age, in ms,
	// of the samples in a packet at the moment the game's capture call received it (device
	// timestamp of the first sample vs. now, minus half the packet). A pick lands anywhere in
	// the packet, so the mean is the delay a player experiences. Works on the stock path
	// too, via the AsioHook tap.
	void ReportMeasuredInputAge(double ageMs);
	// Raw inputs of the tap's measurement (clock delta in 100 ns units, packet frames), for
	// the evidence line in Poll.
	void ReportMeasuredInputRaw(int64_t delta100ns, uint32_t frames);

	// RSMods.ini [Mod Settings] AudioDiagnosticsOverlay (off by default).
	bool IsOverlayEnabled();
}
