# Split a single WAV render of tools/shreddage-chromatic.mid into per-note samples the
# injector loads: note_<midi>.wav for midi LOW..HIGH. Preserves the source WAV format
# (just copies PCM byte ranges + writes a header), so 16/24/32-bit or float all pass through.
#
# Usage: python split-guitar-samples.py <render.wav> [out_folder]
#   default out_folder = the game's RSModsResearch\GuitarSamples (auto-loaded on launch)
import sys, os, struct

LOW, HIGH = 40, 88
STRIDE_SEC = 2.6      # per-note stride in the MIDI (2.0 note + 0.6 gap) at 120 BPM
TAKE_SEC   = 1.8      # how much of each note to keep (attack + sustain)

def read_wav(path):
    with open(path, 'rb') as f: buf = f.read()
    assert buf[:4] == b'RIFF' and buf[8:12] == b'WAVE', 'not a WAV'
    fmt = None; data = None; pos = 12
    while pos + 8 <= len(buf):
        cid = buf[pos:pos+4]; clen = struct.unpack('<I', buf[pos+8-4:pos+8])[0]
        body = buf[pos+8:pos+8+clen]
        if cid == b'fmt ': fmt = body
        elif cid == b'data': data = body
        pos += 8 + clen + (clen & 1)
    tag, ch, rate, byterate, block, bits = struct.unpack('<HHIIHH', fmt[:16])
    return dict(fmt=fmt, tag=tag, ch=ch, rate=rate, bits=bits, block=block, data=data)

def write_wav(path, w, pcm):
    data_len = len(pcm)
    with open(path, 'wb') as f:
        f.write(b'RIFF'); f.write(struct.pack('<I', 36 + data_len)); f.write(b'WAVE')
        f.write(b'fmt '); f.write(struct.pack('<I', 16))
        f.write(struct.pack('<HHIIHH', w['tag'], w['ch'], w['rate'],
                            w['rate']*w['block'], w['block'], w['bits']))
        f.write(b'data'); f.write(struct.pack('<I', data_len)); f.write(pcm)

def main():
    if len(sys.argv) < 2:
        print("usage: python split-guitar-samples.py <render.wav> [out_folder]"); return 1
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else \
        r"C:\Program Files (x86)\Steam\steamapps\common\Rocksmith2014\RSModsResearch\GuitarSamples"
    os.makedirs(out, exist_ok=True)
    w = read_wav(src)
    block = w['block']; rate = w['rate']
    stride = int(STRIDE_SEC * rate) * block
    take   = int(TAKE_SEC   * rate) * block
    n = 0
    for k, midi in enumerate(range(LOW, HIGH+1)):
        start = k * stride
        seg = w['data'][start:start+take]
        if len(seg) < block * rate // 4:   # <0.25s -> render too short, stop
            print(f"  (render ends at note {midi}; got {n} samples)"); break
        write_wav(os.path.join(out, f"note_{midi}.wav"), w, seg)
        n += 1
    print(f"wrote {n} samples to {out}  ({rate} Hz, {w['bits']}-bit, {w['ch']}ch)")
    print("Then in-game: load them without a restart via the bridge (load_guitar_samples).")

if __name__ == '__main__':
    sys.exit(main())
