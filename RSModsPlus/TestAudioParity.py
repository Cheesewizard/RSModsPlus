"""Compare the managed audio frontend and inference with the trained Python path."""
import pathlib
import subprocess
import os
import sys
import json
import numpy as np
import soundfile as sf

ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS = pathlib.Path(os.environ.get('RSMODSPLUS_TOOLS_DIR', r'C:\Programming\RocksmithNativeAtlas\rsmodsplus-private\tools'))
sys.path.insert(0, str(TOOLS / 'ml-string-fret-service'))
import frontend as fe

OUT = ROOT / 'artifacts/managed-ml-20260907/parity'
OUT.mkdir(parents=True, exist_ok=True)
exe = ROOT / 'Installer/Resources/RSModsGUI/RSMods.exe'
session = fe.load_model(str(TOOLS / 'ml-training/models/fretnet_guitarset.onnx'))
rng = np.random.default_rng(20260907)
results = []
for rate in (22050, 44100, 48000):
    count = int(1.2 * rate)
    t = np.arange(count) / rate
    fixtures = {'silence': np.zeros(count), 'noise': rng.normal(0, .02, count),
                'low_e': .2 * np.sin(2*np.pi*82.4069*t),
                'b19': .2*np.sin(2*np.pi*739.989*t) * np.exp(-2*t)}
    for name, audio in fixtures.items():
        audio = audio.astype('float32')
        prefix = OUT / f'{rate}-{name}'
        source, target = prefix.with_suffix('.f32'), prefix.with_suffix('.result')
        audio.tofile(source)
        if target.exists(): target.unlink()
        subprocess.run([str(exe), '--ml-verify-audio', str(source), str(target), str(rate)], check=True)
        data = target.read_bytes()
        center = int.from_bytes(data[:4], 'little', signed=True)
        values = np.frombuffer(data[4:], dtype='<f4')
        cqt = fe.compute_cqt(audio, rate)
        tile = np.ascontiguousarray(cqt[-9:].T)
        outputs, reference_center = fe.infer_latest_frame(session, audio, rate)
        reference = np.concatenate([x.ravel() for x in outputs])
        tile_error = float(np.max(np.abs(values[:1728] - tile.ravel())))
        output_error = float(np.max(np.abs(values[1728:] - reference)))
        np.testing.assert_allclose(values[:1728], tile.ravel(), atol=2e-5, rtol=2e-4)
        np.testing.assert_allclose(values[1728:], reference, atol=.002, rtol=.002)
        assert center == reference_center, (center, reference_center)
        results.append(dict(rate=rate, fixture=name, tileError=tile_error, outputError=output_error))
        print(results[-1], flush=True)
(OUT / 'results.json').write_text(json.dumps(results, indent=2))
