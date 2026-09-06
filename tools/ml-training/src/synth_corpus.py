"""Adapter: the waveguide synthetic corpus (tools/waveguide_guitar.py) -> FretNet
training/eval arrays, using the SAME CQT front end and label conventions as the
GuitarSet path so synth and real data are interchangeable to the model.

Why this exists
---------------
The headline weakness (issue #66) is same-pitch-different-position disambiguation.
GuitarSet has little controlled coverage of it; the waveguide generator makes it on
demand, calibrated to the real rig's per-string inharmonicity (wg_calibration.json),
with exact string/fret/onset labels. This module turns a generated corpus directory
into the (tiles, tab, dev, mask, onset) tuples featurize_track_fretnet() produces, so
it can be concatenated into a FretNet training set or held out as a pure
disambiguation benchmark.

Design choices
--------------
* CQT front end reused verbatim (compute_cqt / tile_context from features.py) so the
  synth clips land in the identical feature space as GuitarSet -- no domain seam in
  the input.
* Label rasterization is reimplemented here in pure numpy (frame_labels/onset_labels)
  rather than imported from guitarset.py. It is numerically identical to that path
  (cqt_frame_times == arange(n)*HOP/SR), but keeping it librosa-free lets the label
  logic be unit-tested anywhere, and avoids importing the JAMS-bound GuitarSet loader
  for data that has no JAMS.
* dev (pitch deviation) is all-zero and mask = (note active): the synth set has no
  bends, so the pitch head simply gets valid zero-deviation targets on sounded
  frames. Shape-compatible with the FretNet loss; it neither teaches nor fights bends.

Run `python synth_corpus.py <corpus_dir>` for a dependency-light self-check of the
label path (no librosa needed); add `--featurize` to also build the full arrays
(needs the training env's librosa/soundfile).
"""

import os
import glob
import json

import numpy as np

from constants import SR, HOP_LENGTH, NUM_STRINGS


def frame_times(n_frames):
    """Center time (s) of each CQT frame. Identical to features.cqt_frame_times but
    librosa-free (librosa.frames_to_time is exactly frames*hop/sr)."""
    return np.arange(n_frames) * HOP_LENGTH / SR


def notes_to_per_string(notes):
    """[{string,fret,onset,offset}] -> list of 6 lists of (onset, offset, fret),
    the shape guitarset.load_string_notes returns."""
    per = [[] for _ in range(NUM_STRINGS)]
    for nd in notes:
        per[int(nd["string"])].append(
            (float(nd["onset"]), float(nd["offset"]), int(nd["fret"])))
    return per


def frame_labels(per_string, n_frames):
    """(n_frames, 6) int class ids: 0 silent, fret f -> f+1. Mirrors
    guitarset.frame_labels exactly."""
    t = frame_times(n_frames)
    labels = np.zeros((n_frames, NUM_STRINGS), dtype=np.int64)
    for s in range(NUM_STRINGS):
        for onset, offset, fret in per_string[s]:
            if fret < 0 or fret > 19:
                continue
            lo = int(np.searchsorted(t, onset, side="left"))
            hi = int(np.searchsorted(t, offset, side="right"))
            labels[lo:hi, s] = fret + 1
    return labels


def onset_labels(per_string, n_frames, spread=1):
    """(n_frames, 6) binary onset target, +/-spread frames around each note start.
    Mirrors fretnet_data.rasterize_onsets."""
    t = frame_times(n_frames)
    onset = np.zeros((n_frames, NUM_STRINGS), dtype=np.float32)
    for s in range(NUM_STRINGS):
        for onset_t, _off, fret in per_string[s]:
            if fret < 0 or fret > 19:
                continue
            fi = int(np.searchsorted(t, onset_t, side="left"))
            onset[max(0, fi - spread):min(n_frames, fi + spread + 1), s] = 1.0
    return onset


def featurize_clip(wav_path, notes_json):
    """One synth clip -> (tiles, tab, dev, mask, onset), matching
    fretnet_data.featurize_track_fretnet's shapes. Needs librosa+soundfile."""
    import soundfile as sf
    from features import compute_cqt, tile_context

    y, sr = sf.read(wav_path)
    if y.ndim > 1:
        y = y.mean(axis=1)
    cqt = compute_cqt(y, sr)
    tiles = tile_context(cqt)
    n = cqt.shape[0]

    per = notes_to_per_string(json.load(open(notes_json))["notes"])
    tab = frame_labels(per, n)
    onset = onset_labels(per, n)
    dev = np.zeros((n, NUM_STRINGS), dtype=np.float32)   # no bends in synth set
    mask = (tab > 0).astype(np.float32)
    return tiles, tab, dev, mask, onset


def iter_corpus(corpus_dir):
    """Yield (clip_meta, wav_path, notes_json_path) for every clip in a corpus."""
    man = json.load(open(os.path.join(corpus_dir, "manifest.json")))
    for c in man["clips"]:
        cid = c["id"]
        yield c, os.path.join(corpus_dir, cid + ".wav"), \
            os.path.join(corpus_dir, cid + ".notes.json")


def build_arrays(corpus_dir):
    """Featurize a whole corpus and concatenate along the frame axis ->
    (tiles, tab, dev, mask, onset). Same layout the FretNet trainer consumes."""
    T, TAB, DEV, M, ON = [], [], [], [], []
    for _c, wav, nj in iter_corpus(corpus_dir):
        t, tab, dev, mask, on = featurize_clip(wav, nj)
        T.append(t); TAB.append(tab); DEV.append(dev); M.append(mask); ON.append(on)
    return (np.concatenate(T), np.concatenate(TAB), np.concatenate(DEV),
            np.concatenate(M), np.concatenate(ON))


def build_npy_cache(corpus_dir, cache_dir):
    """Featurize the corpus into per-clip .npy quintuples, matching the exact naming
    and return contract of kaggle_fretnet.build_cache so the synth clips slot into
    FrameDataset alongside the GuitarSet cache. On Kaggle:

        from synth_corpus import build_npy_cache
        synth_files = build_npy_cache(SYNTH_DIR, CACHE_DIR)
        files = gs_files + synth_files      # feed FrameDataset

    Returns [(clip_id, tiles_path, tab_path, dev_path, mask_path, onset_path)]."""
    os.makedirs(cache_dir, exist_ok=True)
    out = []
    for i, (c, wav, nj) in enumerate(iter_corpus(corpus_dir)):
        cid = c["id"]
        paths = {k: os.path.join(cache_dir, f"{cid}_{k}.npy")
                 for k in ("tiles", "tab", "dev", "mask", "onset")}
        if not all(os.path.exists(p) for p in paths.values()):
            tiles, tab, dev, mask, onset = featurize_clip(wav, nj)
            np.save(paths["tiles"], tiles)
            np.save(paths["tab"], tab)
            np.save(paths["dev"], dev)
            np.save(paths["mask"], mask)
            np.save(paths["onset"], onset)
        out.append((cid, paths["tiles"], paths["tab"], paths["dev"],
                    paths["mask"], paths["onset"]))
        print(f"[synth-cache] {i + 1} {cid}", flush=True)
    return out


def same_pitch_groups(corpus_dir):
    """{midi: [(id, string, fret), ...]} for pitches with >=2 positions -- the
    disambiguation benchmark subset."""
    groups = {}
    for c, _w, _n in iter_corpus(corpus_dir):
        groups.setdefault(c["midi"], []).append((c["id"], c["string"], c["fret"]))
    return {m: v for m, v in groups.items()
            if len({(s, f) for _i, s, f in v}) >= 2}


def _self_check(corpus_dir):
    """librosa-free validation of the label path over the whole corpus."""
    n_clips = 0
    bad = 0
    for c, _wav, nj in iter_corpus(corpus_dir):
        per = notes_to_per_string(json.load(open(nj))["notes"])
        # a synth clip is a single note; derive a frame count from its offset
        end = max((off for s in per for (_on, off, _f) in s), default=0.0)
        n = int(np.ceil((end + 0.4) * SR / HOP_LENGTH))
        tab = frame_labels(per, n)
        onset = onset_labels(per, n)
        # the one labelled note must appear on exactly its (string, fret)
        s, f = c["string"], c["fret"]
        active = (tab[:, s] == f + 1).sum()
        others = (tab[:, [i for i in range(NUM_STRINGS) if i != s]] > 0).sum()
        if active == 0 or others != 0 or onset[:, s].sum() == 0:
            bad += 1
        n_clips += 1
    groups = same_pitch_groups(corpus_dir)
    print(f"self-check: {n_clips} clips, {bad} label mismatches")
    print(f"same-pitch benchmark: {len(groups)} pitches with >=2 positions "
          f"(e.g. { {m: len(v) for m, v in list(groups.items())[:5]} })")
    return bad == 0


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus_dir")
    ap.add_argument("--featurize", action="store_true",
                    help="also build full arrays (needs librosa/soundfile)")
    args = ap.parse_args()

    ok = _self_check(args.corpus_dir)
    if args.featurize:
        tiles, tab, dev, mask, onset = build_arrays(args.corpus_dir)
        print(f"arrays: tiles{tiles.shape} tab{tab.shape} dev{dev.shape} "
              f"mask{mask.shape} onset{onset.shape}")
    raise SystemExit(0 if ok else 1)
