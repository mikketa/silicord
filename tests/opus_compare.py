"""Compares opus_vectors output with a reference decoding of an Opus test vector.

    python tests/opus_compare.py out01.pcm testvector01.dec

Both files are 48 kHz stereo 16-bit little-endian PCM. Prints the signal to
error ratio of the whole file and of its worst 20 ms frame. Conformant
decoders stay far above 60 dB on CELT and hybrid material.
"""
import array
import math
import sys


def load(path):
    samples = array.array('h')
    with open(path, 'rb') as f:
        samples.frombytes(f.read())
    if sys.byteorder == 'big':
        samples.byteswap()
    return samples


def snr(ref, got):
    signal = sum(x * x for x in ref) or 1
    error = sum((a - b) ** 2 for a, b in zip(ref, got)) or 1
    return 10 * math.log10(signal / error)


def main():
    got, ref = load(sys.argv[1]), load(sys.argv[2])
    if len(got) != len(ref):
        print('length differs: %d vs %d samples' % (len(got), len(ref)))
    n = min(len(got), len(ref))
    frame = 960 * 2
    worst = min(range(0, n, frame), key=lambda s: snr(ref[s:s + frame], got[s:s + frame]))
    print('SNR %.1f dB, worst frame %d at %.1f dB' % (snr(ref[:n], got[:n]), worst // frame,
                                                    snr(ref[worst:worst + frame], got[worst:worst + frame])))


if __name__ == '__main__':
    main()
