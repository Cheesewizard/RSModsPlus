# Render a full-range guitar sample bank straight from a SoundFont via FluidSynth (ctypes),
# no DAW and no splitting. Deterministic: each note is rendered on its own, exact timing.
# Uses TuxGuitar's bundled libfluidsynth-3.dll + MagicSFver2.sf2 (already installed).
import os, sys, ctypes, struct, math

TUX = r"C:\Program Files\tuxguitar"
SF2 = os.path.join(TUX, r"share\soundfont\MagicSFver2.sf2")
OUT = sys.argv[1] if len(sys.argv) > 1 else r"C:\Programming\RSModsPlus\tools\SF2Bank"
PROGRAM = int(sys.argv[2]) if len(sys.argv) > 2 else 25   # GM 25 = steel acoustic; 27 = clean electric
LOW, HIGH = 40, 88
RATE = 48000
HOLD = 1.8      # seconds rendered per note
VEL  = 110

os.add_dll_directory(TUX)
fl = ctypes.CDLL(os.path.join(TUX, "libfluidsynth-3.dll"))
# signatures
fl.new_fluid_settings.restype = ctypes.c_void_p
fl.new_fluid_synth.restype = ctypes.c_void_p
fl.new_fluid_synth.argtypes = [ctypes.c_void_p]
fl.fluid_settings_setnum.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]
fl.fluid_settings_setint.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
fl.fluid_synth_sfload.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
fl.fluid_synth_sfload.restype = ctypes.c_int
fl.fluid_synth_program_select.argtypes = [ctypes.c_void_p]*1 + [ctypes.c_int]*4
fl.fluid_synth_noteon.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
fl.fluid_synth_noteoff.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
fl.fluid_synth_write_float.argtypes = [ctypes.c_void_p, ctypes.c_int,
    ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
fl.fluid_synth_all_notes_off.argtypes = [ctypes.c_void_p, ctypes.c_int]

settings = fl.new_fluid_settings()
fl.fluid_settings_setnum(settings, b"synth.sample-rate", float(RATE))
fl.fluid_settings_setint(settings, b"synth.reverb.active", 0)
fl.fluid_settings_setint(settings, b"synth.chorus.active", 0)
fl.fluid_settings_setint(settings, b"synth.polyphony", 64)
synth = fl.new_fluid_synth(settings)
sfid = fl.fluid_synth_sfload(synth, SF2.encode(), 1)
if sfid < 0: print("SF2 load FAILED"); sys.exit(1)
fl.fluid_synth_program_select(synth, 0, sfid, 0, PROGRAM)
print(f"loaded {os.path.basename(SF2)} sfid={sfid} program={PROGRAM}")

os.makedirs(OUT, exist_ok=True)
n = int(HOLD*RATE)
def render_note(key):
    L = (ctypes.c_float*n)(); R = (ctypes.c_float*n)()
    fl.fluid_synth_all_notes_off(synth, 0)
    # flush a hair of silence to clear tails
    s = (ctypes.c_float*256)(); fl.fluid_synth_write_float(synth,256, s,0,1, s,0,1)
    fl.fluid_synth_noteon(synth, 0, key, VEL)
    fl.fluid_synth_write_float(synth, n, L,0,1, R,0,1)
    return L
def write_wav(path, buf):
    # 16-bit mono
    data = bytearray()
    for v in buf:
        x = max(-1.0, min(1.0, v))
        data += struct.pack('<h', int(x*32767))
    dl = len(data)
    with open(path,'wb') as f:
        f.write(b'RIFF'+struct.pack('<I',36+dl)+b'WAVE')
        f.write(b'fmt '+struct.pack('<I',16)+struct.pack('<HHIIHH',1,1,RATE,RATE*2,2,16))
        f.write(b'data'+struct.pack('<I',dl)+bytes(data))

count=0; silent=[]
for midi in range(LOW, HIGH+1):
    buf = render_note(midi)
    # RMS check
    acc=0.0
    for i in range(0,n,7): acc += buf[i]*buf[i]
    rms=math.sqrt(acc/(n//7))
    write_wav(os.path.join(OUT, f"note_{midi}.wav"), buf)
    if rms < 0.002: silent.append(midi)
    count+=1
print(f"rendered {count} notes -> {OUT}")
print(f"silent/low notes: {silent if silent else 'none'}")
