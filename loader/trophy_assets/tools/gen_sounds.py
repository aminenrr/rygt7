# Original trophy chimes for ryGT7, synthesised from sine/FM partials (no samples).
import numpy as np, wave, os
SR = 48000
OUT = os.path.join(os.path.dirname(__file__), "out")
rng = np.random.default_rng(11)

def hz(m): return 440 * 2 ** ((m - 69) / 12)

def bell(f, L):
    t = np.arange(int(SR * L)) / SR
    att = np.minimum(t / 0.003, 1)
    mod = 1.6 * np.exp(-t / 0.06) * np.sin(2 * np.pi * f * 2.0 * t)
    s = np.sin(2 * np.pi * f * t + mod) * np.exp(-t / 0.35)
    s += 0.35 * np.sin(2 * np.pi * f * 2.01 * t) * np.exp(-t / 0.18)
    s += 0.15 * np.sin(2 * np.pi * f * 3.98 * t) * np.exp(-t / 0.08)
    return s * att

def reverb(x, mix=0.28, dec=0.9):
    L = int(SR * dec * 2)
    ir = rng.standard_normal(L) * np.exp(-np.arange(L) / (SR * dec / 4))
    k = np.ones(8) / 8
    ir = np.convolve(ir, k, "same")
    ir /= np.sqrt((ir ** 2).sum())
    n = len(x) + L
    wet = np.fft.irfft(np.fft.rfft(x, n) * np.fft.rfft(ir, n), n)
    out = np.zeros(n); out[:len(x)] = x * (1 - mix); out += wet * mix
    return out

def render(notes, L):
    buf = np.zeros(int(SR * L))
    for start, m, amp, dur in notes:
        s = bell(hz(m), dur) * amp
        i = int(start * SR)
        buf[i:i + len(s)] += s[:len(buf) - i]
    y = reverb(buf)
    fade = int(0.25 * SR)
    y[-fade:] *= np.linspace(1, 0, fade)
    y /= np.abs(y).max() / 0.7
    return y

def save(name, y):
    pcm = (np.clip(y, -1, 1) * 32767).astype("<i2")
    with wave.open(os.path.join(OUT, name), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(pcm.tobytes())

# trophy: a bright rising fifth + octave
save("trophy.wav", render([(0.00, 79, 0.8, 1.0), (0.09, 86, 0.7, 1.1), (0.18, 91, 0.6, 1.3)], 1.6))
# platinum: a slower major arpeggio that rises and rings
save("platinum.wav", render([(0.00, 72, 0.7, 1.6), (0.14, 76, 0.7, 1.6), (0.28, 79, 0.7, 1.6),
                             (0.42, 84, 0.8, 1.8), (0.62, 88, 0.6, 2.0), (0.62, 91, 0.5, 2.0),
                             (0.62, 96, 0.35, 2.2)], 3.0))
