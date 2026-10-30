#!/usr/bin/env python3
"""Regenerate the README figures from the constants in the firmware sources.

Every number plotted here is parsed from spectrum/src/main.cpp or
spotify/src/main.cpp and pushed through a line-by-line Python port of the
firmware arithmetic (same integer truncation). Nothing is hand-drawn.

    python3 docs/make_figures.py        # needs numpy + matplotlib
"""
import colorsys
import math
import os
import re

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "img")


def defines(path):
    src = open(os.path.join(ROOT, path)).read()
    out = {}
    for name, val in re.findall(r"^#define\s+(\w+)\s+([0-9.]+)\b", src, re.M):
        out[name] = float(val) if "." in val else int(val)
    return out


# --------------------------------------------------------------------------
# spectrum: FFT bin -> bar mapping and magnitude -> height transfer
# --------------------------------------------------------------------------
def spectrum_figures():
    d = defines("spectrum/src/main.cpp")
    N, fs, bars = d["SAMPLES"], d["SAMPLING_FREQ"], d["NUM_BARS"]
    area_h = d["AREA_BOT"] - d["AREA_TOP"]
    lo, hi = 2.0, N // 2 - 1

    # Port of setup(): log-spaced edges, forced strictly increasing.
    edge = []
    for i in range(bars + 1):
        e = int(lo * math.pow(hi / lo, i / bars))
        if i > 0 and e <= edge[i - 1]:
            e = edge[i - 1] + 1
        edge.append(e)
    df = fs / N

    print("spectrum: bin width %.2f Hz, frame %.2f ms" % (df, 1000 * N / fs))
    print("spectrum: edges", edge)
    widths = [edge[b + 1] - edge[b] for b in range(bars)]
    print("spectrum: bins/bar", widths)
    print("spectrum: range %.0f Hz .. %.0f Hz" % (edge[0] * df, (edge[-1]) * df))

    fig, ax = plt.subplots(figsize=(8, 3.6))
    for b in range(bars):
        f0, f1 = edge[b] * df, edge[b + 1] * df
        ax.barh(b, f1 - f0, left=f0, height=0.8, color="#2a9d8f")
    ax.set_xscale("log")
    ax.set_xlabel("frequency (Hz), FFT bin k maps to k * fs / N")
    ax.set_ylabel("bar index")
    ax.set_title("spectrum: frequency span of each of the %d bars (N=%d, fs=%.0f Hz)" % (bars, N, fs))
    ax.grid(True, which="both", axis="x", alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "spectrum_bands.png"), dpi=120)
    plt.close(fig)

    m = np.logspace(0, 7.5, 600)
    h = np.clip((np.log10(m + 1.0) - d["LOG_FLOOR"]) / (d["LOG_CEIL"] - d["LOG_FLOOR"]) * area_h, 0, area_h).astype(int)
    fig, ax = plt.subplots(figsize=(8, 3.2))
    ax.plot(m, h, color="#e76f51")
    ax.set_xscale("log")
    ax.set_xlabel("peak FFT magnitude in band |X[k]| (int16 input, Hamming window)")
    ax.set_ylabel("bar height (px)")
    ax.set_title("spectrum: log-magnitude transfer, floor 10^%.1f, ceiling 10^%.1f" % (d["LOG_FLOOR"], d["LOG_CEIL"]))
    ax.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "spectrum_transfer.png"), dpi=120)
    plt.close(fig)

    # Bar ballistics for a single 1-frame impulse: instant attack, linear release.
    target = [0] * 3 + [area_h] + [0] * 40
    bar, peak, bs, ps = 0, 0, [], []
    for t in target:
        if t >= bar:
            bar = t
        else:
            bar = max(t, bar - d["FALL"], 0)
        if bar >= peak:
            peak = bar
        else:
            peak = max(peak - d["PEAK_FALL"], bar)
        bs.append(bar)
        ps.append(peak)
    fig, ax = plt.subplots(figsize=(8, 2.8))
    ax.step(range(len(target)), target, where="post", color="#999", label="raw height h")
    ax.step(range(len(bs)), bs, where="post", color="#2a9d8f", label="bar (FALL=%d px/frame)" % d["FALL"])
    ax.step(range(len(ps)), ps, where="post", color="#e76f51", label="peak cap (PEAK_FALL=%d px/frame)" % d["PEAK_FALL"])
    ax.set_xlabel("frame")
    ax.set_ylabel("px")
    ax.set_title("spectrum: response of one bar to a one-frame full-scale impulse")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "spectrum_ballistics.png"), dpi=120)
    plt.close(fig)


# --------------------------------------------------------------------------
# spotify: accent colour derivation (port of deriveAccent())
# --------------------------------------------------------------------------
def constrain(v, a, b):
    return a if v < a else b if v > b else v


def derive_accent(aR, aG, aB):
    avg = (aR + aG + aB) // 3
    r = avg + int((aR - avg) * 1.8)
    g = avg + int((aG - avg) * 1.8)
    b = avg + int((aB - avg) * 1.8)
    luma = (r * 77 + g * 150 + b * 29) >> 8
    if luma < 1:
        luma = 1
    f = 210.0 / luma
    return constrain(int(r * f), 0, 255), constrain(int(g * f), 0, 255), constrain(int(b * f), 0, 255)


def rgb565_roundtrip(r, g, b):
    # The firmware averages 5/6/5-bit channels and shifts back to 8 bit.
    return (r >> 3) << 3, (g >> 2) << 2, (b >> 3) << 3


def spotify_figure():
    hues = np.linspace(0, 1, 12, endpoint=False)
    rows = [(0.35, 0.25), (0.6, 0.45), (0.3, 0.8), (0.8, 0.85), (0.08, 0.5)]
    fig, axes = plt.subplots(len(rows), 1, figsize=(8, 0.9 * len(rows) + 0.6))
    for ax, (s, v) in zip(axes, rows):
        img = np.zeros((2, len(hues), 3))
        for j, hh in enumerate(hues):
            r, g, b = (int(round(c * 255)) for c in colorsys.hsv_to_rgb(hh, s, v))
            r, g, b = rgb565_roundtrip(r, g, b)
            img[0, j] = (r / 255, g / 255, b / 255)
            img[1, j] = [c / 255 for c in derive_accent(r, g, b)]
        ax.imshow(img, aspect="auto")
        ax.set_xticks([])
        ax.set_yticks([0, 1])
        ax.set_yticklabels(["cover mean", "accent"], fontsize=7)
        ax.set_title("cover mean colour S=%.2f V=%.2f" % (s, v), fontsize=8, loc="left")
    fig.suptitle("spotify: deriveAccent() applied to mean cover colours (exact port)", fontsize=9)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "spotify_accent.png"), dpi=120)
    plt.close(fig)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    spectrum_figures()
    spotify_figure()
    print("figures written to", OUT)
