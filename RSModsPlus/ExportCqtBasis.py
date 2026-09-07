"""Developer build tool: freeze the trained CQT filters into an embedded resource."""
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/ml-string-fret-service'))
import frontend

transform = frontend.prepare_transform()
destination = ROOT / 'RSModsPlus/MachineLearning/CqtBasis.bin'
with destination.open('wb') as output:
    output.write(struct.pack('<i', 1))
    output.write(transform.length_scale.astype('<f8').tobytes())
    for basis, fft_size, hop in transform.octaves:
        output.write(struct.pack('<ii', fft_size, hop))
        for row in range(24):
            values = basis.getrow(row)
            output.write(struct.pack('<i', values.nnz))
            for index, value in zip(values.indices, values.data):
                output.write(struct.pack('<iff', int(index), float(value.real), float(value.imag)))
print(destination)
