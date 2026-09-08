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

	void SetProcessingEnabled(bool enabled);
	bool IsProcessingEnabled();
	bool IsInputConfigured(size_t routeIndex);
	bool IsInputReady(size_t routeIndex);
}
