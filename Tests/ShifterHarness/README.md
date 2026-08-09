# Audio shifter harnesses

Standalone tools for measuring the production Drop Pedal shifter and the
Signalsmith processing paths used by Speaker Mode. They are diagnostics and
benchmarks; they do not establish in-game hook, routing, prepared-audio
alignment, seek, or end-of-song correctness.

## Build and run

From a Developer Command Prompt in `Tests\ShifterHarness`:

```bat
build.bat
drop_pedal_latency.exe
harness.exe 128
speaker_latency.exe
speaker_pipeline_benchmark.exe song.wav
```

`drop_pedal_latency.exe` measures the observable content delay added by the
production ASIO Drop Pedal shifter. It aligns deterministic input and output
amplitude-envelope transitions across three marker sequences for guitar and
bass notes at 48 kHz with 128-frame callbacks, then reports median,
95th-percentile and maximum delay. The zero-semitone cases validate the
measurement baseline. It also reports average, 95th-percentile,
99th-percentile, and maximum wall-clock processing time per callback against
the 2.67 ms real-time deadline. Maximum wall-clock time includes operating
system scheduling interruptions. When launched by double-clicking, it waits
for Enter before closing and also writes `drop_pedal_latency_results.txt`
beside the executable.

`harness.exe 128` runs the production Drop Pedal shifter with 128-frame ASIO
callbacks over held notes, plucks, weak signals, string changes, bends, double
stops, staccato notes, and live retuning. It reports introduced pop events,
pitch error, and harmonic purity. WAV mode accepts a recorded mono DI take:
`harness.exe input.wav semitones output.wav 128`.

`speaker_latency.exe` compares algorithmic latency, processing cost, and pitch
accuracy at the game's observed 48 kHz sample rate and 128-frame decoder
callbacks. It also exercises the fixed-length `outputSeek` and flush sequence
used to remove full-song playback delay.

`speaker_pipeline_benchmark.exe` times continuous progressive rendering and the
direct render into delete-on-close mapped PCM used during a Speaker Mode session.
It verifies frame counts, deterministic output, mapped-output accuracy, and
continuity across streaming call boundaries.

`speaker_extractor_benchmark.cs` isolates PSARC discovery, WEM extraction, and
the external decode stages. Compile it with `GUI\PsarcEntryPath.cs` beside
`GUI\Lib\Rocksmith2014PsarcLib.dll`.
`speaker_extractor_integration_test.cs` drives the same command-line extraction
entry point exposed by `RSMods.exe`. Its `--test-psarc-entry-paths` mode verifies
that PSARC entry names are parsed as archive identifiers, including names with
characters Windows rejects in filesystem paths and pathless TOC entries.
