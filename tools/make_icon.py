"""Generates resources/app.ico (speaker with sound waves). Requires Pillow.

Usage: python tools/make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SCALE = 4  # Draw large and downsample for smooth edges


def draw(size: int) -> Image.Image:
    s = size * SCALE
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = s * 0.04
    d.rounded_rectangle([pad, pad, s - pad, s - pad], radius=s * 0.22, fill=(28, 92, 168, 255))

    white = (255, 255, 255, 255)
    # Speaker body and cone
    d.rectangle([s * 0.18, s * 0.40, s * 0.32, s * 0.60], fill=white)
    d.polygon([(s * 0.32, s * 0.40), (s * 0.50, s * 0.24), (s * 0.50, s * 0.76), (s * 0.32, s * 0.60)], fill=white)
    # Sound waves (fewer at tiny sizes so they stay legible)
    waves = [(0.20, 0.085)] if size <= 20 else [(0.17, 0.065), (0.29, 0.065)]
    cx, cy = s * 0.50, s * 0.50
    for radius, width in waves:
        r = s * radius
        d.arc([cx - r, cy - r, cx + r, cy + r], start=-50, end=50, fill=(255, 196, 64, 255), width=max(1, int(s * width)))
    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    out = Path(__file__).resolve().parent.parent / "resources" / "app.ico"
    out.parent.mkdir(exist_ok=True)
    images = [draw(n) for n in SIZES]
    images[-1].save(out, format="ICO", sizes=[(n, n) for n in SIZES], append_images=images[:-1])
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
