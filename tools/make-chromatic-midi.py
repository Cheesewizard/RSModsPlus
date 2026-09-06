# Generate a chromatic MIDI file: one sustained note per semitone across the guitar range,
# each note held NOTE_SEC with GAP_SEC of silence between, so the rendered WAV can be split
# into per-note samples by fixed timing. Note N is the (N-LOW)th note in the file.
import struct, sys

LOW, HIGH = 40, 88          # E2 .. E6
NOTE_SEC, GAP_SEC = 2.0, 0.6
TEMPO_BPM = 120
PPQ = 480                   # ticks per quarter note
VEL = 110

ticks_per_sec = PPQ * (TEMPO_BPM / 60.0)
note_ticks = int(NOTE_SEC * ticks_per_sec)
gap_ticks  = int(GAP_SEC  * ticks_per_sec)

def vlq(n):
    b = [n & 0x7F]; n >>= 7
    while n: b.insert(0, (n & 0x7F) | 0x80); n >>= 7
    return bytes(b)

events = bytearray()
# tempo meta
events += vlq(0) + bytes([0xFF,0x51,0x03]) + struct.pack(">I", int(60_000_000/TEMPO_BPM))[1:]
first = True
for midi in range(LOW, HIGH+1):
    delta = 0 if first else gap_ticks
    first = False
    events += vlq(delta) + bytes([0x90, midi, VEL])          # note on
    events += vlq(note_ticks) + bytes([0x80, midi, 0])        # note off
events += vlq(0) + bytes([0xFF,0x2F,0x00])                    # end of track

track = bytes([0x4D,0x54,0x72,0x6B]) + struct.pack(">I", len(events)) + bytes(events)
header = bytes([0x4D,0x54,0x68,0x64]) + struct.pack(">IHHH", 6, 0, 1, PPQ)

out = r"C:\Programming\RSModsPlus\tools\shreddage-chromatic.mid"
with open(out, "wb") as f: f.write(header + track)
print(f"wrote {out}: notes {LOW}..{HIGH} ({HIGH-LOW+1} notes), {NOTE_SEC}s each + {GAP_SEC}s gap")
print(f"note K (0-indexed) starts at t = K*{NOTE_SEC+GAP_SEC}s ; midi = {LOW}+K")
