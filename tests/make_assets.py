"""Generates the small PNG fixtures used by tests/HostSim. Not shipped."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / "assets"
ASSETS.mkdir(exist_ok=True)

FONT = r"C:\Windows\Fonts\msyh.ttc"
FONT_BOLD = r"C:\Windows\Fonts\msyhbd.ttc"


def font(size, bold=False):
    try:
        return ImageFont.truetype(FONT_BOLD if bold else FONT, size)
    except OSError:
        return ImageFont.load_default()


def gradient(size, start, end):
    image = Image.new("RGB", size)
    draw = ImageDraw.Draw(image)
    w, h = size
    for y in range(h):
        t = y / max(1, h - 1)
        color = tuple(int(start[i] + (end[i] - start[i]) * t) for i in range(3))
        draw.line([(0, y), (w, y)], fill=color)
    return image


def sample_one():
    image = gradient((1200, 800), (64, 120, 220), (150, 215, 255))
    draw = ImageDraw.Draw(image)
    for x, y, r, color in [
        (110, 120, 90, (255, 255, 255, 60)),
        (1030, 160, 140, (255, 255, 255, 40)),
        (950, 650, 70, (255, 255, 255, 50)),
    ]:
        draw.ellipse((x - r, y - r, x + r, y + r), fill=color)
    draw.rounded_rectangle((90, 220, 1110, 640), radius=36, fill=(255, 255, 255))
    draw.text((140, 270), "SnowAir", font=font(96, True), fill=(30, 34, 40))
    draw.text((146, 390), "晾衣架 · 截图随手挂", font=font(54, True), fill=(47, 111, 235))
    draw.text((146, 490), "点一下复制  ·  双击打开  ·  拖进聊天窗口", font=font(32),
              fill=(89, 97, 109))
    draw.text((146, 552), "test asset / 1200 x 800", font=font(24), fill=(138, 146, 158))
    image.save(ASSETS / "sample1.png")


def sample_two():
    image = gradient((900, 1200), (255, 214, 165), (255, 160, 170))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((70, 90, 830, 1110), radius=44, fill=(255, 255, 255, 235))
    draw.ellipse((140, 150, 760, 770), fill=(255, 236, 210))
    draw.text((300, 380), "02", font=font(220, True), fill=(210, 120, 80))
    draw.text((140, 820), "竖图也挂得住", font=font(64, True), fill=(60, 42, 32))
    draw.text((142, 920), "900 × 1200 · portrait fixture", font=font(30), fill=(120, 96, 82))
    image.save(ASSETS / "sample2.png")


def sample_three():
    image = gradient((1600, 700), (25, 27, 32), (58, 64, 82))
    draw = ImageDraw.Draw(image)
    for i, color in enumerate([(110, 168, 255), (91, 209, 139), (255, 196, 120)]):
        draw.rounded_rectangle((90 + i * 500, 150, 540 + i * 500, 540), radius=28,
                               outline=color, width=6)
        draw.text((130 + i * 500, 190), f"0{i + 1}", font=font(96, True), fill=color)
        draw.text((130 + i * 500, 320), ["copy", "open", "drag"][i], font=font(42, True),
                  fill=(235, 238, 245))
    draw.text((92, 590), "wide fixture · 1600 × 700", font=font(28), fill=(180, 188, 204))
    image.save(ASSETS / "sample3.png")


def main():
    sample_one()
    sample_two()
    sample_three()
    for path in sorted(ASSETS.glob("*.png")):
        print(path, path.stat().st_size)


if __name__ == "__main__":
    main()
