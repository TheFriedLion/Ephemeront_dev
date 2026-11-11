"""Glitch checker for Ephemeront recordings.

Make a steady test tone, play it while Ephemeront is running, save a clip, then
check the clip. Any dropout, skip, or click shows up as a break in the sine wave.

    python glitch_check.py tone tone.wav [seconds]
    python glitch_check.py check clip.wav [more.wav ...]
"""

import math
import os
import struct
import sys

FREQ = 440.0


def write_tone(path, seconds=12, rate=48000):
    frames = int(rate * seconds)
    data = bytearray()
    for i in range(frames):
        v = int(8000 * math.sin(2 * math.pi * FREQ * i / rate))
        data += struct.pack('<hh', v, v)
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ')
        f.write(struct.pack('<IHHIIHH', 16, 1, 2, rate, rate * 4, 4, 16))
        f.write(b'data' + struct.pack('<I', len(data)) + data)


def read_left(path):
    """Returns (sample rate, bits, left channel as floats in -1..1). Reads PCM and float WAVs."""
    with open(path, 'rb') as f:
        blob = f.read()
    pos, fmt, rate, channels, bits, data = 12, None, 0, 0, 0, b''
    while pos + 8 <= len(blob):
        tag, size = blob[pos:pos + 4], struct.unpack('<I', blob[pos + 4:pos + 8])[0]
        body = blob[pos + 8:pos + 8 + size]
        if tag == b'fmt ':
            fmt, channels, rate = struct.unpack('<HHI', body[:8])
            bits = struct.unpack('<H', body[14:16])[0]
        elif tag == b'data':
            data = body
        pos += 8 + size + (size & 1)
    if fmt == 3:
        samples = struct.unpack('<%df' % (len(data) // 4), data)
    else:
        samples = [s / 32768 for s in struct.unpack('<%dh' % (len(data) // 2), data)]
    return rate, bits, samples[0::channels]


def check(path):
    rate, bits, left = read_left(path)
    name = os.path.basename(path)
    loud = [i for i, x in enumerate(left) if abs(x) > 0.03]
    if not loud:
        print('%s: %d Hz %d-bit, no tone found' % (name, rate, bits))
        return
    a, b = loud[0] + 100, loud[-1] - 100
    # A pure sine obeys s[n] = 2cos(w)s[n-1] - s[n-2]; anything else is a glitch.
    k = 2 * math.cos(2 * math.pi * FREQ / rate)
    events = []
    for i in range(a + 2, b):
        if abs(left[i] - (k * left[i - 1] - left[i - 2])) > 0.012:
            if not events or i - events[-1] > rate // 1000:
                events.append(i)
    peak = max(abs(x) for x in left)
    print('%s: %d Hz %d-bit, tone %.2f s, peak %.3f, %d glitches' % (name, rate, bits, (b - a) / rate, peak, len(events)))
    for i in events[:10]:
        print('   at %.3f s' % ((i - a) / rate))


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'tone':
        write_tone(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 12)
    elif len(sys.argv) >= 3 and sys.argv[1] == 'check':
        for p in sys.argv[2:]:
            check(p)
    else:
        print(__doc__)
