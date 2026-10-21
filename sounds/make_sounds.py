#!/usr/bin/env python3
"""The four StrikeSense alarm sounds, made from scratch (no recordings):
  reel_scream.wav  - a reel's drag screaming as a fish takes line (strike / run)
  reel_clicks.wav  - drag clicks, slow then faster (slow pull)
  rod_bell.wav     - the rod-tip bell jingling (knocks)
  falling_tone.wav - three falling bite-alarm tones (drop-back)
    python3 make_sounds.py        (needs numpy)
"""
import numpy as np, wave

SR = 22050
rng = np.random.default_rng(7)

def save(name, x):
    x = x / (np.max(np.abs(x)) + 1e-9) * 0.9
    with wave.open(name, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes((x * 32767).astype('<i2').tobytes())
    print(name, f"{len(x) / SR:.2f} s")

def fade(x, ms=15):
    n = int(SR * ms / 1000)
    x[:n] *= np.linspace(0, 1, n); x[-n:] *= np.linspace(1, 0, n)
    return x

def click(strength=1.0):
    # one pawl click on the drag's toothed wheel: a sharp tick + short metal ring
    n = int(SR * 0.012)
    t = np.arange(n) / SR
    f = rng.uniform(3300, 3900)
    ring = np.sin(2 * np.pi * f * t) * np.exp(-t / 0.0022)
    ring += 0.5 * np.sin(2 * np.pi * f * 1.73 * t) * np.exp(-t / 0.0015)
    tick = rng.normal(0, 1, n) * np.exp(-t / 0.0007)
    return strength * (0.8 * ring + 0.6 * tick)

def clicks(rate_fn, seconds, jitter=0.12, whir=0.0):
    x = np.zeros(int(SR * seconds) + SR // 10)
    t = 0.0
    while t < seconds:
        r = rate_fn(t)
        c = click(rng.uniform(0.75, 1.0))
        i = int(t * SR)
        x[i:i + len(c)] += c[:len(x) - i]
        t += (1 / r) * (1 + rng.uniform(-jitter, jitter))
    if whir > 0:
        # the spool spinning: a soft band of noise that follows the click rate
        tt = np.arange(len(x)) / SR
        nz = rng.normal(0, 1, len(x))
        k = np.ones(8) / 8
        nz = nz - np.convolve(nz, k, 'same')                 # brighter
        env = np.array([min(1, rate_fn(min(s, seconds)) / 60) for s in tt])
        x += whir * nz * env * 0.25
    return fade(x[:int(SR * seconds)])

# 1. screaming reel: a fast run - clicks jump from 20 to ~75 a second and keep going
def scream_rate(t):
    return 20 + 55 * min(1, t / 0.35) + 8 * np.sin(2 * np.pi * 1.7 * t)
save("reel_scream.wav", clicks(scream_rate, 2.6, jitter=0.08, whir=0.25))

# 2. slow pull: a few clicks, then faster and faster
save("reel_clicks.wav", clicks(lambda t: 3.0 * (30 / 3.0) ** (t / 3.0), 3.2, jitter=0.15))

# 3. rod bell: a small jingle bell shaken by the rod tip, twice
def bell_hit(f0, amp):
    n = int(SR * 0.6); t = np.arange(n) / SR
    y = np.zeros(n)
    for ratio, a, dec in ((1, 1, 0.20), (2.32, 0.55, 0.12), (4.25, 0.35, 0.07), (6.63, 0.2, 0.05)):
        y += a * np.sin(2 * np.pi * f0 * ratio * t + rng.uniform(0, 6)) * np.exp(-t / dec)
    y += 0.3 * rng.normal(0, 1, n) * np.exp(-t / 0.002)          # the ball hitting the shell
    return amp * y
x = np.zeros(int(SR * 2.4))
for g, start in enumerate((0.0, 1.0)):
    for k in range(7):                                            # one shake = a quick rattle
        tt = start + k * rng.uniform(0.035, 0.06)
        h = bell_hit(rng.uniform(2350, 2550), (1 - k * 0.1) * rng.uniform(0.6, 1))
        i = int(tt * SR); x[i:i + len(h)] += h[:len(x) - i]
save("rod_bell.wav", fade(x))

# 4. falling tone: dee - doo - dum, like a bite alarm on a drop-back
x = np.zeros(int(SR * 1.5))
for k, f in enumerate((1180, 880, 620)):
    n = int(SR * 0.32); t = np.arange(n) / SR
    tone = np.sin(2 * np.pi * f * t) + 0.35 * np.sin(2 * np.pi * 2 * f * t) + 0.15 * np.sin(2 * np.pi * 3 * f * t)
    tone *= (1 + 0.15 * np.sin(2 * np.pi * 9 * t))                # a little warble
    env = np.minimum(1, t / 0.01) * np.minimum(1, (0.32 - t) / 0.06)
    i = int(k * 0.42 * SR); x[i:i + n] += tone * env
save("falling_tone.wav", x)
