# Speaker Mode investigation

This is the reverse-engineering record behind Speaker Mode: how the insertion
point was chosen, what each verification step showed, and the findings that did
not end up in the shipped implementation. The shipped design itself is described
in [the Speaker Mode design](../designs/speaker-mode.md).

**Status:** Historical record. Speaker Mode has been implemented and released.

## Insertion-layer selection

Decoded PCM at the codec-source boundary, referred to during the investigation
as Layer C, was selected as the insertion layer.

The Wwise Vorbis decoder output at vtable slot 10 exposes decoded PCM in
128-frame buffers. At this boundary the stereo sample data is interleaved
signed 16-bit PCM rather than the deinterleaved float format used later in
Wwise's native effect pipeline. File and bank codecs have distinct factories
and decoder vtables, so their identity stays explicit throughout the hook
lifecycle. Decoder objects are reused at the same addresses, which is why the
shipped hook tracks each reuse with a generation number assigned by the file
source factory.

Observed file streams were stereo 44.1 kHz and 48 kHz music. Song previews ran
approximately 28-30 seconds, and starting a full song produced two stereo
48 kHz streams of 11,486,976 frames (approximately 239.3 seconds). Bank streams
were predominantly short 32 kHz sound effects. Those observations set the
admission rule the shipped hook still uses: file-decoded stereo 44.1/48 kHz
audio at least 20 seconds long.

## Isolation verification

Verification ran as a ladder of increasingly audible probes, each removed once
its question was answered.

A silence probe muted only the admitted streams. It silenced song previews and
the full song while menu sound effects stayed audible, proving the boundary
isolates streamed music from bank-decoded effects. The probe did not test live
guitar tone or note detection.

A -6 dB gain probe then confirmed the samples could be modified cleanly. It
reduced preview and full-song volume without static while menu effects stayed
untouched.

The gain probe also surfaced the correct sample format. An earlier
implementation interpreted the buffers as deinterleaved float and produced
static, because it processed twice the valid buffer size; the data is
interleaved signed 16-bit PCM.

## Pitch verification

The final probe replaced the gain change with Signalsmith streaming pitch
processing, logging for each source generation whether it was admitted as a
streamed-music target and processing admitted audio only while Speaker Mode was
selected. Bank-decoder output was observed but never modified.

The first in-game pitch test passed on 1 August 2026: preview and full-song
music shifted correctly and without static. Longer playing exposed latency that
the initial test missed. Signalsmith's default 48 kHz configuration reports
2,880 frames of input latency and 2,880 frames of output latency, or 120 ms
total. Wwise's decode queue can absorb computation time, but it cannot remove
that content delay.

A focused 32-bit optimized benchmark compared 120, 80, 60, and 40 ms windows at
the observed 128-frame callback size. Processing cost stayed near 2% of one CPU
core for every configuration, proving the trade-off is latency against spectral
resolution rather than latency against CPU load. The streaming implementation
now uses a 60 ms window and 15 ms interval: 1,440 input plus 1,440 output frames
at 48 kHz.

## Stream identity findings

One explored direction was an independent look-ahead decoder, which would have
needed a stable stream or media identity per source. A passthrough probe
snapshotted the first 128 bytes of each file-factory context before the
registered Vorbis factory consumed it, without altering the context.

The captures showed codec ID `4` at context word 29 for every file source.
Word 18 changed per voice, a candidate instance or playing identifier.
Preview-like sources carried flags `0x1` at word 1; the observed full-song
source carried `0x10008`. Context words 5, 24, 25, and 26 consistently held
readable pointers, and a follow-up probe snapshotted only those four evidenced
targets rather than walking arbitrary pointers.

That follow-up resolved the stable Wwise bank names for both tested assets:

- `Song_Goldfinger99RedBalloons_Preview.bnk` for menu preview playback;
- `Song_Goldfinger99RedBalloons.bnk` for in-game playback.

The names resolved to the custom-song archive
`Goldfinger - 99 Red Balloons - v1.0 - DDC v2.2_p.psarc`. Its audio entries map
the preview bank to `1308704337.wem` and the full-song bank to `544120541.wem`.
The corresponding little-endian media ID occurs in each bank at offsets 44,
51,293, and 51,297, proving the mapping independently of PSARC entry order.
Independent extraction and `ww2ogg` inspection confirmed the live observations:

- `1308704337.wem`: stereo 48 kHz, 1,440,000 samples (30 seconds);
- `544120541.wem`: stereo 48 kHz, 11,486,976 samples (approximately 239.3 seconds).

The unfinished cache build proved that these pointers are not a safe runtime
identity contract. When a later factory context did not expose the bank string,
the source was classified as a full song with a null cache. The output hook then
retained the original decoder audio. The same classification explains the
reported previews that no longer shifted. The replacement uses Rocksmith's
existing selected song key and treats every non-matching source as a live stream.

The failed-build screenshot was a second independent state bug. `Speaker: F# ->
E (+2)` was the session's stale manually selected target; the code only logged
Rocksmith's arrangement tuning after gameplay started and never applied it. The
replacement reads the tuning label at the pre-song tuner, requires a uniform
six-string offset, and atomically sets the Speaker target before requesting the
cache. For the reported Eb Standard chart and an E guitar, the resulting audio
interval is +1 semitone and the overlay is `Speaker: Eb -> E (+1)`.

An offline follow-up extracted and decoded the full-song WEM successfully, so
preprocessing an entire song is technically possible. It is not available at
the current hook boundary: that callback receives only the next decoded
128-frame block and cannot see future PCM. A preload implementation would need
a separate WEM decoder, generic archive/media resolution, and a processed-audio
cache of roughly 46 MB for this four-minute stereo 16-bit song. Calling the live
Wwise decoder ahead would also require replacing its buffer and position
contract, which is substantially riskier than the current in-place hook.

## Measured architecture comparison

The 4:35 Thin Boys WEM was used as the actual-song benchmark. The measured stages
were 59 ms for PSARC discovery/bank resolution, 5 ms for WEM inflation, 780 ms
for `ww2ogg`, 93 ms for `revorb`, and 936 ms for `oggdec`: 1.875 seconds total.
In the explicit x86 benchmark, Signalsmith's monolithic `exact()` render took
9.222 seconds, PCM conversion and disk write took 73 ms, and map/page touch took
21 ms. The continuous progressive render took 9.664 seconds.

Independent 7.68-second segments with 960 ms of context scaled from 12.711
seconds on one x86 worker to 1.906 seconds on eight. They were rejected: resetting
Signalsmith changes phase-vocoder state, and hard boundaries differed from the
continuous reference by up to 1.82 full-scale. Overlap/crossfade would hide a
click by mixing unrelated phase states, not provide sample-exact stitching.

A single progressive x86 state produced the first ten seconds in 12 ms after
decode. Its complete output was deterministic, contained exactly the Wwise
declared 12,129,454 positions, and showed no call-boundary spike: RMS adjacent
sample delta was 0.0367 at 8,192-frame process boundaries versus 0.0376 across
the song. It preserves state and position mapping, so it was selected. The file factory
requires ten prepared seconds before constructing a new source; the worker then
renders ahead. Backward seeks use already published frames. A forward seek beyond
the frontier waits on the Wwise audio worker until the requested range exists.

Signalsmith's one-call `exact()` waveform is not a bit-reference for streaming.
The library evaluates digital silence per `process()` submission, whereas the
one-call helper evaluates the whole song as one submission. The benchmark
therefore treats exact output length, deterministic continuous-state output, and
boundary continuity as the progressive acceptance criteria; it does not claim
the two submission policies produce identical samples.

The current RSMods and upstream `develop` trees contain no CPU-affinity limiter.
The >32-thread workaround belongs to other Rocksmith/CDLC tooling. The benchmark
process had all 16 logical processors in its affinity mask. Windows schedules
threads across the process affinity mask by default; no assumption is made that
Rocksmith itself is single-core.

## Zero-playback-latency design

A causal streaming pitch shifter cannot produce the current output sample from
future audio it has not received. Shortening its analysis window reduces that
delay but also reduces low-frequency and transient resolution. Because
Rocksmith's song audio is prerecorded, the delay can instead be removed from the
playback path entirely by preparing position-aligned output before Wwise consumes
it.

The implemented design is progressive session-temporary processed PCM:

1. Resolve `GameState::GetSongKey()` to the owning bank, PSARC, and WEM.
2. Decode outside Wwise and normalize decoder tail padding to the sample count
   declared in the WEM. A vgmstream cross-check reported the same count.
3. Process the fixed-length stereo buffer progressively. A current Signalsmith Stretch
   release provides exact fixed-buffer processing with start/end latency
   compensation, allowing the higher-quality default window to be restored.
4. Render signed 16-bit stereo PCM directly into a delete-on-close mapped
   temporary file. The observed four-minute song occupies approximately 46 MB,
   but it is retained only while active or during a two-minute cooldown. Retired
   audio has a 256 MiB ceiling and is never reused across Rocksmith launches.
5. Keep calling the original Wwise decoder to preserve all of its source state,
   but replace each returned block from the mapped temporary PCM at the decoder's
   reported `positionStart`. The audio callback then performs only bounds checks and a
   small `memcpy`; it runs no pitch DSP and adds no content delay.

The temporary full-song audio starts rendering when the pre-song tuner confirms the
selection. Choosing a different song cancels unfinished work. Full playback is
held only until the exact opening is ready; silently falling back to streaming
would reintroduce the delay this design exists to remove. A target tuning change
requests different prepared audio, so a change during playback would have to
explicitly pause until that render is ready. The implemented design disallows
Speaker Mode pitch and mode changes for the active song.

This is preferable to driving the live Wwise decoder ahead. Decoder-ahead would
need to reproduce the private output-state contract, keep the decoder object's
hidden position ahead of the pipeline, handle seeks and end-of-stream state, and
serve buffers with Wwise-compatible lifetimes. An independent decoder plus
position-indexed replacement leaves the original decoder state untouched.

Primary references:

- [Signalsmith Stretch processing, latency, seeking, and flush contract](https://github.com/Signalsmith-Audio/signalsmith-stretch)
- [vgmstream Wwise support and accurate sample-count goals](https://github.com/vgmstream/vgmstream)
- [Microsoft `SetProcessAffinityMask` contract](https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-setprocessaffinitymask)
