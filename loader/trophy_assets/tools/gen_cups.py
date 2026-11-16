# Original trophy cup icons for ryGT7 (CC0 / project licence). Drawn from primitives, no third-party art.
from PIL import Image, ImageDraw, ImageFilter, ImageChops
import os
S = 1024  # supersampled canvas, saved at 256
OUT = os.path.join(os.path.dirname(__file__), "out")
os.makedirs(OUT, exist_ok=True)

PAL = {  # (dark, mid, light, highlight)
    "bronze":   ((92, 46, 18), (176, 98, 48), (226, 150, 96), (255, 214, 178)),
    "silver":   ((88, 94, 104), (168, 176, 188), (214, 220, 230), (255, 255, 255)),
    "gold":     ((122, 78, 8), (214, 160, 32), (246, 210, 90), (255, 246, 200)),
    "platinum": ((70, 86, 120), (170, 190, 222), (222, 232, 250), (255, 255, 255)),
}

def mask():
    m = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(m)
    # bowl
    d.polygon([(250, 150), (774, 150), (720, 470), (304, 470)], fill=255)
    d.ellipse((304, 330, 720, 600), fill=255)
    d.rounded_rectangle((236, 130, 788, 190), 24, fill=255)
    # handles (rings)
    for x0 in (120, 704):
        ring = Image.new("L", (S, S), 0)
        rd = ImageDraw.Draw(ring)
        rd.ellipse((x0, 200, x0 + 200, 440), fill=255)
        rd.ellipse((x0 + 44, 244, x0 + 156, 396), fill=0)
        m = ImageChops.lighter(m, ring)
    d = ImageDraw.Draw(m)
    # stem
    d.polygon([(468, 580), (556, 580), (540, 740), (484, 740)], fill=255)
    d.rounded_rectangle((420, 720, 604, 770), 18, fill=255)
    # base
    d.polygon([(352, 790), (672, 790), (712, 900), (312, 900)], fill=255)
    d.rounded_rectangle((280, 880, 744, 940), 20, fill=255)
    return m

def gradient(c0, c1, c2):
    g = Image.new("RGB", (S, S))
    px = g.load()
    for x in range(S):
        t = x / (S - 1)
        # metallic: dark edge - light - mid - dark
        if t < 0.38:
            k = t / 0.38; a, b = c0, c2
        elif t < 0.62:
            k = (t - 0.38) / 0.24; a, b = c2, c1
        else:
            k = (t - 0.62) / 0.38; a, b = c1, c0
        col = tuple(int(a[i] + (b[i] - a[i]) * k) for i in range(3))
        for y in range(S):
            px[x, y] = col
    return g

def cup(name):
    c0, c1, c2, hi = PAL[name]
    m = mask()
    body = gradient(c0, c1, c2)
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.paste(body, (0, 0), m)
    # highlight stripe on bowl
    h = Image.new("L", (S, S), 0)
    ImageDraw.Draw(h).rounded_rectangle((350, 190, 400, 470), 25, fill=150)
    h = ImageChops.multiply(h, m).filter(ImageFilter.GaussianBlur(10))
    img.paste(Image.new("RGBA", (S, S), hi + (255,)), (0, 0), h)
    # star emblem on the bowl
    import math
    pts = []
    cx, cy, r1, r2 = 512, 340, 92, 38
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = r1 if i % 2 == 0 else r2
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    star = Image.new("L", (S, S), 0)
    ImageDraw.Draw(star).polygon(pts, fill=255)
    img.paste(Image.new("RGBA", (S, S), c0 + (255,)), (0, 0), star.filter(ImageFilter.GaussianBlur(2)))
    star_in = Image.new("L", (S, S), 0)
    ImageDraw.Draw(star_in).polygon([(cx + (p[0] - cx) * 0.78, cy + (p[1] - cy) * 0.78) for p in pts], fill=255)
    img.paste(Image.new("RGBA", (S, S), hi + (255,)), (0, 0), star_in)
    # dark outline
    edge = m.filter(ImageFilter.MaxFilter(13))
    outline = Image.new("RGBA", (S, S), tuple(max(0, v - 40) for v in c0) + (255,))
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(outline, (0, 0), edge)
    out.alpha_composite(img)
    if name == "platinum":  # soft glow
        glow = Image.new("RGBA", (S, S), (190, 215, 255, 0))
        ga = edge.filter(ImageFilter.GaussianBlur(28)).point(lambda v: int(v * 0.55))
        glow.putalpha(ga)
        glow.alpha_composite(out)
        out = glow
    out.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT, name + ".png"), optimize=True)

for n in PAL:
    cup(n)
# contact sheet for review
sheet = Image.new("RGBA", (1100, 300), (32, 34, 40, 255))
for i, n in enumerate(PAL):
    sheet.alpha_composite(Image.open(os.path.join(OUT, n + ".png")), (20 + i * 270, 22))
sheet.save(os.path.join(OUT, "_sheet.png"))
