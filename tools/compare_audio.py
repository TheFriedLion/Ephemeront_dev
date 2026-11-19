"""Checks whether a recording is an exact copy of an original WAV.

    python tools/compare_audio.py info FILE.wav
    python tools/compare_audio.py compare ORIGINAL.wav RECORDED.wav
    python tools/compare_audio.py resample IN.wav OUT.wav RATE

`compare` lines the recording up with the original (the recording usually has
extra silence before and after), then reports how closely every sample matches:
how many are bit-identical, the largest difference, and how deep the two cancel
when one is subtracted from the other.
"""
import struct
import sys

import numpy as np


def read_wav(path):
    data = open(path, 'rb').read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise SystemExit('%s: not a WAV file' % path)
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            tag, channels, rate, _, _, bits = struct.unpack('<HHIIHH', body[:16])
            if tag == 0xFFFE and len(body) >= 26:  # WAVE_FORMAT_EXTENSIBLE: real tag is in the subformat
                tag = struct.unpack('<H', body[24:26])[0]
            fmt = (tag, channels, rate, bits)
        elif cid == b'data':
            pcm = body
        pos += 8 + size + (size & 1)
    tag, channels, rate, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(pcm, '<f4').astype(np.float64)
        kind = '32-bit float'
    elif tag == 3 and bits == 64:
        x = np.frombuffer(pcm, '<f8')
        kind = '64-bit float'
    elif tag == 1 and bits == 16:
        x = np.frombuffer(pcm, '<i2') / 32768.0
        kind = '16-bit PCM'
    elif tag == 1 and bits == 24:
        b = np.frombuffer(pcm[:len(pcm) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v / 8388608.0
        kind = '24-bit PCM'
    elif tag == 1 and bits == 32:
        x = np.frombuffer(pcm, '<i4') / 2147483648.0
        kind = '32-bit PCM'
    else:
        raise SystemExit('%s: unsupported format (tag %d, %d bits)' % (path, tag, bits))
    x = x[:len(x) // channels * channels].reshape(-1, channels)
    return x, rate, kind


def db(v):
    return 20 * np.log10(v) if v > 0 else float('-inf')


def info(path):
    x, rate, kind = read_wav(path)
    peak = np.abs(x).max()
    rms = np.sqrt(np.mean(x ** 2))
    loud = np.nonzero(np.abs(x).max(axis=1) > 1e-4)[0]
    print('%s' % path)
    print('  %s, %d Hz, %d channel(s), %.3f s (%d frames)' % (kind, rate, x.shape[1], len(x) / rate, len(x)))
    print('  peak %.4f (%.2f dBFS), RMS %.2f dBFS' % (peak, db(peak), db(rms)))
    if len(loud):
        print('  sound from %.3f s to %.3f s' % (loud[0] / rate, (loud[-1] + 1) / rate))
    if x.shape[1] == 2:
        print('  channels identical: %s' % np.array_equal(x[:, 0], x[:, 1]))
    return x, rate


def best_offset(orig, rec):
    """Where in `rec` the original starts, by cross-correlation (via FFT)."""
    n = 1
    while n < len(orig) + len(rec):
        n *= 2
    corr = np.fft.irfft(np.fft.rfft(rec, n) * np.conj(np.fft.rfft(orig, n)), n)
    lag = int(np.argmax(corr[:len(rec)]))
    return lag


def compare(orig_path, rec_path):
    o, orate = info(orig_path)
    print()
    r, rrate = info(rec_path)
    print()
    if orate != rrate:
        print('SAMPLE RATES DIFFER (%d vs %d): Windows had to resample, so an exact match is impossible.'
              % (orate, rrate))
        return
    if o.shape[1] == 1 and r.shape[1] == 2:
        o = np.repeat(o, 2, axis=1)  # a mono file plays identically on both sides
    lag = best_offset(o[:, 0], r[:, 0])
    if lag + len(o) > len(r):
        print('The recording is too short to hold the whole original (it starts %.3f s in).' % (lag / rrate))
        o = o[:len(r) - lag]
    seg = r[lag:lag + len(o)]
    print('Original found %.3f s into the recording (frame %d).' % (lag / rrate, lag))

    diff = seg - o
    same = np.count_nonzero(np.float32(seg) == np.float32(o))
    total = o.size
    sig = np.sqrt(np.mean(o ** 2))
    res = np.sqrt(np.mean(diff ** 2))
    gain = float(np.sum(seg * o) / np.sum(o * o))
    fitted = np.sqrt(np.mean((seg - gain * o) ** 2))
    print('Bit-identical samples: %d of %d (%.4f%%)' % (same, total, 100.0 * same / total))
    bad = np.nonzero(np.any(np.float32(seg) != np.float32(o), axis=1))[0]
    if len(bad):
        # Group mismatched frames into stretches, so a few bad moments stand out from scattered noise.
        breaks = np.nonzero(np.diff(bad) > 1)[0]
        starts = np.concatenate([[bad[0]], bad[breaks + 1]])
        ends = np.concatenate([bad[breaks], [bad[-1]]]) + 1
        print('Mismatched frames:     %d, in %d stretch(es)' % (len(bad), len(starts)))
        for a, b in list(zip(starts, ends))[:8]:
            print('    %.4f s to %.4f s (%d frames), largest difference %.3g' %
                  (a / orate, b / orate, b - a, np.abs(diff[a:b]).max()))
        clean = np.ones(len(o), bool)
        clean[bad] = False
        print('Everything else:       bit-identical (%d frames)' % clean.sum())
    print('Largest difference:    %.3g (%.1f dBFS)' % (np.abs(diff).max(), db(np.abs(diff).max())))
    print('Null depth:            %.1f dB (original minus recording, relative to the signal)' % db(res / sig))
    print('Level of recording:    %+.4f dB versus the original' % db(gain))
    print('Null after level match: %.1f dB' % db(fitted / sig))
    outside = np.concatenate([r[:lag].ravel(), r[lag + len(o):].ravel()])
    if outside.size:
        print('Outside the original:  peak %.3g (should be silence)' % np.abs(outside).max())
    if same == total:
        print('\nRESULT: EXACT. Every sample matches the original bit for bit.')
    elif db(res / sig) < -90:
        print('\nRESULT: Transparent. Differences are below -90 dB (rounding level), not audible.')
    else:
        print('\nRESULT: NOT exact. See the numbers above.')


def resample(src, dst, rate):
    """Writes a 32-bit float copy of src at another sample rate (for testing at the device's rate)."""
    from fractions import Fraction
    from scipy.signal import resample_poly
    x, srate, _ = read_wav(src)
    f = Fraction(rate, srate)
    y = resample_poly(x, f.numerator, f.denominator, axis=0).astype('<f4')
    ch = y.shape[1]
    body = y.tobytes()
    fmt = struct.pack('<HHIIHH', 3, ch, rate, rate * ch * 4, ch * 4, 32)
    with open(dst, 'wb') as out:
        out.write(b'RIFF' + struct.pack('<I', 4 + 8 + len(fmt) + 8 + len(body)) + b'WAVE')
        out.write(b'fmt ' + struct.pack('<I', len(fmt)) + fmt)
        out.write(b'data' + struct.pack('<I', len(body)) + body)
    print('wrote %s (%d Hz)' % (dst, rate))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'info':
        info(sys.argv[2])
    elif len(sys.argv) == 5 and sys.argv[1] == 'resample':
        resample(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    elif len(sys.argv) == 4 and sys.argv[1] == 'compare':
        compare(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
