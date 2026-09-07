# Shared output buffer tuning

Implemented locally on feature/modern-cable-input. Not deployed or accepted in
live gameplay. This is an explicit calibration run, not continuous automatic
adjustment during recording, and does not claim to measure round-trip latency.

## Existing Modern Cable code

CableInput.cpp already queries minimum/default/fundamental/maximum engine periods,
requests the minimum, and reports capture dropouts and packet age. It did not
contain a stability search or persisted best-buffer result. The new output tuner
uses the bridge's existing output diagnostics; input faults remain separate.

## Workflow

Find stable buffer tests the current active output, starting at its minimum.
Keep Rocksmith focused and play normally. Five seconds of warmup precede each
30-second observation window; focus loss or a stalled stream resets the window.
An increase in empty-output observations rejects that candidate. Repeated game
blocks abort calibration rather than being treated as a reason to add buffering.
An unchanged cumulative fault count from startup does not fail a new trial.

Candidate periods are supported fundamental multiples, ordered upward with roughly
1.5x increases. At most eight candidates are considered, capped at 40 ms unless
the device minimum itself is larger. This finds the first passing tested value;
it is not an exhaustive proof of the absolute lowest stable value. Devices with
one supported period can only be validated at that value. Total run time is capped
at six minutes, including focus changes.

A passing period is saved in AudioRouting.ini under [Output buffer <endpoint ID>],
PeriodFrames. Startup and live output changes read and validate it against current
device limits. Unsupported saved periods cause an explicit error; Use device
minimum clears that device's saved value. Retest after driver/hardware changes.

Increasing the engine period primes only the additional buffering with silence,
outside the recorded game mix, before the new stream starts. The game-facing
buffer size and the validated callback pacing remain unchanged. A device/Windows
engine that refuses a live period change returns an error; success is not assumed.

Cancel or a failed trial restores the previous runtime period if it was changed
and the same endpoint is still active. Saved settings are written only on success.
Recording is disallowed through the tuning panel; period changes are also rejected
natively during recording. A changed endpoint aborts the test, and each period
command verifies the expected endpoint atomically on the control path.

## Protocol and validation

Operation 6: value is periodFrames, newline, expected endpoint ID. Operation 5
adds engineMinimum, engineFundamental, engineMaximum and enginePeriod fields to
its diagnostic text. The independent mixer task owns the separate protocol-v2
response extension and operations 7/8; buffer tuning works through its existing
AudioControlClient abstraction.

Tests cover supported/unaligned periods, wrong-device refusal, recording protection,
unchanged game buffer size, warmup and stable-window duration, stale counters,
focus loss, stalled streams, repeated-sample rejection, candidate bounds,
missing diagnostics, and cancellation with restoration and no persistence.
Existing pacing, queue, recording, live-switch and ASIO-mode tests also pass on
the isolated combined build. The GUI builds and an offscreen render was inspected;
layout now scrolls instead of overlapping the input controls.

Limitations: empty-output observations are a conservative risk signal, not a
hardware glitch counter; the test is limited to its observed load. It cannot
remove noise/clipping from the guitar or establish guitar-to-speaker latency.
Live driver-period negotiation and audible tuning outcomes remain unverified.

## Quick check duration

The default check now uses 2 seconds of warmup plus 8 seconds of measurement
(about 10 seconds per candidate). An optional longer check uses 2 + 33 seconds.
The UI reports stage, elapsed/remaining measurement time and explicit focus-loss
or stalled-audio restart reasons. A device exposing a single period is labelled
Check audio stability. Results describe observations during the check rather
than promising lasting stability. Timing tests cover both durations, restart
reasons, new faults and cancellation restoration.
