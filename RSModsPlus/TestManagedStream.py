"""Exercise the production managed service on isolated Windows audio mailboxes."""
import ctypes
import json
import mmap
import os
import pathlib
import struct
import subprocess
import sys
import time
import uuid
import numpy as np
import soundfile as sf

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/ml-string-fret-service'))
import frontend as fe
from model_evidence import find_pitch_source

OUT = ROOT / 'artifacts/managed-ml-20260907'
suffix = '.Test' + uuid.uuid4().hex
ring_count = 1 << 17
audio = mmap.mmap(-1, 64 + ring_count*4, tagname='Local\\RSModsPlus.MlAudio.v2' + suffix)
struct.pack_into('<II', audio, 0, 0x4c4d5352, 2)
ring = np.ndarray((ring_count,), dtype='<f4', buffer=audio, offset=64)
exe = pathlib.Path(os.environ.get('MANAGED_ML_TEST_DIR', ROOT / 'Installer/Resources/RSModsGUI')) / 'ManagedMlHarness.exe'
log = (OUT / 'stream.log').open('w')
process = subprocess.Popen([str(exe), str(os.getpid()), suffix], stdout=log, stderr=log,
                           creationflags=subprocess.CREATE_NO_WINDOW,
                           cwd=exe.parent, env={**os.environ, 'PATH': os.path.join(os.environ['SystemRoot'],'System32')})
mailbox = mmap.mmap(-1, 112, tagname='Local\\RSModsPlus.MlStringFret.v2' + suffix)
session = fe.load_model(str(ROOT / 'tools/ml-training/models/fretnet_guitarset.onnx'))
results = []
try:
    time.sleep(2)
    assert process.poll() is None, 'Managed service failed at startup'
    for rate, shift, name in [(48000,0,'b19'), (48000,-1,'b19'), (44100,-1,'low'), (48000,0,'silence')]:
        # Rewind as well as change the format to exercise reset invalidation.
        struct.pack_into('<Q', audio, 16, 0)
        struct.pack_into('<Ii', audio, 8, rate, shift)
        ring[:] = 0
        time.sleep(.2)
        count = int(rate*2.4)
        t = np.arange(count)/rate
        if name == 'silence': y = np.zeros(count, dtype=np.float32)
        elif name == 'low': y = (.2*np.sin(2*np.pi*82.4069*t)).astype(np.float32)
        else:
            original, source_rate = sf.read(ROOT / 'tools/fretboard/note_bank/note_78.wav', dtype='float32')
            if original.ndim > 1: original = original.mean(axis=1)
            import librosa
            if source_rate != rate: original = librosa.resample(original, orig_sr=source_rate, target_sr=rate)
            if shift: original = librosa.effects.pitch_shift(original, sr=rate, n_steps=shift)
            y = np.resize(original, count).astype(np.float32)
        for start in range(0, count, rate//100):
            struct.pack_into('<IiiI',audio,48,2,4,77,ctypes.windll.kernel32.GetTickCount() & 0xffffffff)
            end = min(count, start + rate//100)
            ring[np.arange(start,end)&(ring_count-1)] = y[start:end]
            struct.pack_into('<Q', audio, 16, end)
            time.sleep(.01)
        time.sleep(.3)
        snapshot = mailbox[:]
        magic, version, seq, actual_shift = struct.unpack_from('<IIIi', snapshot)
        frets = np.array(struct.unpack_from('<6i', snapshot,16))
        physical = np.array(struct.unpack_from('<6i', snapshot,40))
        confidence = np.array(struct.unpack_from('<6f',snapshot,64))
        sample, actual_rate = struct.unpack_from('<QI',snapshot,96)
        assert magic == 0x46535352 and version == 2 and seq % 2 == 0
        assert actual_rate == rate and actual_shift == shift
        assert 0 < sample <= count
        if name == 'silence':
            assert np.all(frets == -1) and np.all(confidence == 0)
        else:
            window = int(1.2*rate)
            # Recover the inference window endpoint from its exact center timestamp.
            probe = np.zeros(window, dtype=np.float32)
            cqt = fe.compute_cqt(probe, rate)
            center = int(round((len(cqt)-5)*512*rate/22050))
            endpoint = sample + window-center
            outputs, _ = fe.infer_latest_frame(session,y[endpoint-window:endpoint],rate)
            probs = fe.probs_from_logits(outputs[0])
            _, expected_conf = find_pitch_source(probs,77)
            detected = [(4,18,expected_conf)] if expected_conf >= .35 else fe.decode_probs(probs)
            expected_frets = np.full(6,-1)
            expected_confidence = np.zeros(6)
            for string, fret, conf in detected:
                expected_frets[string] = fret
                expected_confidence[string] = round(conf,3)
            np.testing.assert_array_equal(frets,expected_frets)
            np.testing.assert_allclose(confidence,expected_confidence,atol=.001)
        np.testing.assert_array_equal(physical, np.where(frets<0,-1,frets-shift))
        results.append(dict(rate=rate,shift=shift,fixture=name,frets=frets.tolist(),sample=int(sample)))
        print(results[-1],flush=True)
    # The mutex must reject a second writer instead of allowing mailbox corruption.
    duplicate = subprocess.run([str(exe),str(os.getpid()),suffix],capture_output=True,
                               creationflags=subprocess.CREATE_NO_WINDOW,timeout=10)
    assert duplicate.returncode == 1 and b'already running' in duplicate.stderr
    assert process.poll() is None
    module_list = subprocess.check_output(['powershell.exe','-NoProfile','-Command',
        f'(Get-Process -Id {process.pid}).Modules | Select-Object -ExpandProperty FileName'],text=True)
    (OUT / 'managed-modules.txt').write_text(module_list)
    for module in module_list.splitlines():
        normalized = str(pathlib.Path(module)).lower()
        assert normalized.startswith(str(exe.parent).lower()) or normalized.startswith(os.environ['SystemRoot'].lower()), module
    (OUT / 'stream-results.json').write_text(json.dumps(results,indent=2))
finally:
    process.terminate()
    process.wait(timeout=10)
    log.close()
    del ring
    audio.close()
    mailbox.close()
