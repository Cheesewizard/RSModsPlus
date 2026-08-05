# Speaker Mode harnesses

Standalone tools for measuring the Signalsmith processing paths used by Speaker
Mode. They are diagnostics and benchmarks; they do not establish in-game hook,
routing, prepared-audio alignment, seek, or end-of-song correctness.

## Build and run

From a Developer Command Prompt in `Tests\ShifterHarness`:

```bat
build.bat
speaker_latency.exe
speaker_pipeline_benchmark.exe song.wav
```

`speaker_latency.exe` compares algorithmic latency, processing cost, and pitch
accuracy at the game's observed 48 kHz sample rate and 128-frame decoder
callbacks. It also exercises the fixed-length `outputSeek` and flush sequence
used to remove full-song playback delay.

`speaker_pipeline_benchmark.exe` times continuous progressive rendering and the
direct render into delete-on-close mapped PCM used during a Speaker Mode session.
It verifies frame counts, deterministic output, mapped-output accuracy, and
continuity across streaming call boundaries.

`speaker_extractor_benchmark.cs` isolates PSARC discovery, WEM extraction, and
the external decode stages. Build it beside `GUI\Lib\Rocksmith2014PsarcLib.dll`.
`speaker_extractor_integration_test.cs` drives the same command-line extraction
entry point exposed by `RSMods.exe`.
