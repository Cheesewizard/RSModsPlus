# Persistent input contract tests

Run `Tests\PersistentInput\run.cmd` from the repository root on Windows with the
Visual Studio x86 C++ build tools installed. Results and compiler output are under
`build/Tests/PersistentInput`.

The harness includes the production capture and device implementations. It drives
explicit packet/timeline transitions, including 100 connect/disconnect cycles,
and verifies the COM contracts. A separate threaded check uses the real event clock
and COM marshaling with an intentionally nonexistent selected endpoint. It opens
no hardware driver, launches no game, and changes no installed configuration.

Capture-driven delivery checks that partial packets wait for completion, complete
packets signal without a timer tick, timer ticks do not duplicate active capture,
two queued packets drain immediately with their original samples/timestamps, and
stalled/disconnected capture returns to timed silence.

The AMD offload regression checks USB identity in property 39 when property 2
names the controller, and rejects a different USB product with a matching prefix.
After building, `build\Tests\PersistentInput\persistent_tests.exe --inspect-devices`
checks real device identification and wrapped enumeration without opening capture
clients. On 8 September it recognized the connected cable and presented exactly
one persistent input. This is discovery evidence, not live capture acceptance.

The shared pitch-processing integration is covered separately by the
`persistent-readiness` scenario in `Tests/AudioCapture/run.cmd`.

Physical USB hotplug and native Rocksmith startup still require live acceptance.
