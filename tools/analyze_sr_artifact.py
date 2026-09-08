#!/usr/bin/env python3
"""Diagnose the 2x super-resolution output for periodic (period-2 / period-4 /
Bayer-cell) artifacts. Run on the SR output PNG:

    python analyze_sr_artifact.py path/to/sr_output.png [row]

Reports the FFT peak structure of a scanline. A dominant peak at 0.25 cycles/px
= period-4 (Bayer-cell) artifact; at 0.5 = period-2 checkerboard. A clean image
has its energy concentrated at low frequencies (smooth rolloff, no strong
mid/high peaks).
"""
import sys
import numpy as np
from PIL import Image

path = sys.argv[1]
row = int(sys.argv[2]) if len(sys.argv) > 2 else None
a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
print("size:", a.shape)
g = a[:, :, 1]
y = row if row is not None else g.shape[0] // 2
line = g[y].astype(np.float64)
line -= line.mean()
x = np.arange(len(line))
line -= np.polyval(np.polyfit(x, line, 1), x)
spec = np.abs(np.fft.rfft(line))
spec[0] = 0
freqs = np.fft.rfftfreq(len(line), d=1.0)
order = np.argsort(spec)[::-1]
print("top FFT peaks (cycles/px, power):")
for p in order[:8]:
    print("  %6.3f  %10.1f" % (freqs[p], spec[p]))
for target in (0.5, 0.25, 0.125):
    band = spec[(freqs >= target - 0.02) & (freqs <= target + 0.02)]
    print("  band@%.3f mean power: %.1f" % (target, band.mean() if len(band) else 0.0))
# adjacent-pixel difference (edge/grain energy)
d1 = np.abs(np.diff(g, axis=1)).mean()
print("  adjacent-pixel mean abs diff: %.2f" % d1)