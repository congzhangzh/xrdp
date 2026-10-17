#!/usr/bin/env python3
"""Measures a tone in a raw recording: s16le, 2 channels, 48000 Hz.

Prints the RMS level (0..1) of its loud part, the part between the first
and the last sample above a tenth of the peak, and the frequency from the
median time between upward zero crossings, interpolated between samples.
The median ignores gaps and clicks, so a tone played too fast (44.1 kHz
audio played as 48 kHz) shows, even if silence pads it to the right
length. Prints "0 0" for silence.

Usage: tone.py <recording.raw>
"""
import statistics
import struct
import sys

RATE = 48000

data = open(sys.argv[1], 'rb').read()
data = data[:len(data) // 4 * 4]
left = struct.unpack('<%dh' % (len(data) // 2), data)[0::2]
peak = max((abs(x) for x in left), default=0)
loud = [i for i, x in enumerate(left) if abs(x) > peak / 10] if peak else []
if len(loud) < 2:
    print('0 0')
    sys.exit()
part = left[loud[0]:loud[-1] + 1]
rms = (sum(x * x for x in part) / len(part)) ** 0.5 / 32768
ups = [i + a / (a - b) for i, (a, b) in enumerate(zip(part, part[1:]))
       if a < 0 <= b]
periods = [b - a for a, b in zip(ups, ups[1:])]
freq = RATE / statistics.median(periods) if periods else 0
print('%.3f %.0f' % (rms, freq))
