# Offline Note-by-Note recording harness

Run from the repository root:

```powershell
& .\Tests\NoteByNoteRecordings\run.ps1
```

The runner reads every bundle in `artifacts/nbn-hardness`. It requires each
bundle to contain `README.md` and an `RSMods_debug` snapshot, verifies the
SHA-256 values documented by the README, and checks output-tap start/stop
mapping where the bundle documents a recording interval. It also verifies
Speaker Mode +1 route synchronization (`Eb -> E (+1)`, `inputShift=0`) from
the README and runtime snapshot.

The two known rollback-baseline findings are read directly from each capture's
bounded runtime-log interval and reported as regression failures: sub-250 ms
dense successor commits in Rock and Roll All Nite, and fret-hand-muted
attack-only acceptance without pitched matching in Pride and Joy. Use
`-AllowExpectedRegressions` only when inspecting bundle integrity while those
baseline failures are intentionally present.

This is an evidence harness, not an audio detector replay. WAV files are
verified by identity and referenced as captured evidence; no pitch ground
truth is inferred from them, and Rocksmith is never launched or contacted.
