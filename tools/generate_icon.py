#!/usr/bin/env python3
"""Regenerate original hdd-noise icons from assets/app-icon.svg.

Development dependency: Pillow. The C++ application has no Python dependency.
This deliberately small SVG renderer supports the flat geometric shapes in the
source icon. Tiny ICO frames omit optional details and use slightly stronger
strokes so the platter and green spindle remain legible at tray-icon sizes.
"""

from __future__ import annotations

import io
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError as error:
    raise SystemExit("Pillow is needed only to regenerate icon assets.") from error


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
SOURCE = ASSETS / "app-icon.svg"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
SUPERSAMPLE = 8


def render_svg(size: int) -> Image.Image:
    """Render the SVG's native vector geometry into an RGBA icon frame."""
    document = ET.parse(SOURCE).getroot()
    scale = size * SUPERSAMPLE / 32.0
    image = Image.new("RGBA", (size * SUPERSAMPLE,) * 2, (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    for element in document:
        kind = element.tag.rsplit("}", 1)[-1]
        if kind not in {"rect", "circle", "polygon", "line"}:
            continue
        if size <= 24 and element.get("data-detail") == "true":
            continue
        fill = element.get("fill")
        stroke = element.get("stroke")
        width = max(1, round(float(element.get("stroke-width", "0")) * scale))
        if stroke and size <= 24:
            width = max(width, round(0.72 * SUPERSAMPLE))

        def number(name: str, default: str = "0") -> float:
            return float(element.get(name, default)) * scale

        if kind == "rect":
            x, y = number("x"), number("y")
            bounds = (x, y, x + number("width"), y + number("height"))
            draw.rounded_rectangle(bounds, radius=number("rx"), fill=fill,
                                   outline=stroke, width=width)
        elif kind == "circle":
            x, y, radius = number("cx"), number("cy"), number("r")
            draw.ellipse((x - radius, y - radius, x + radius, y + radius),
                         fill=fill, outline=stroke, width=width)
        elif kind == "polygon":
            points = [tuple(float(part) * scale for part in pair.split(","))
                      for pair in element.get("points", "").split()]
            draw.polygon(points, fill=fill)
            if stroke:
                draw.line(points + points[:1], fill=stroke, width=width,
                          joint="curve")
        else:
            draw.line((number("x1"), number("y1"), number("x2"), number("y2")),
                      fill=stroke, width=width)

    return image.resize((size, size), Image.Resampling.LANCZOS)


def png_bytes(image: Image.Image) -> bytes:
    output = io.BytesIO()
    image.save(output, format="PNG", compress_level=9)
    return output.getvalue()


def save_ico(frames: dict[int, Image.Image], path: Path) -> None:
    """Write a standard ICO directory containing true-alpha RGBA PNG frames."""
    blobs = [png_bytes(frames[size]) for size in SIZES]
    offset = 6 + 16 * len(SIZES)
    header = bytearray(struct.pack("<HHH", 0, 1, len(SIZES)))
    for size, blob in zip(SIZES, blobs):
        dimension = size if size < 256 else 0
        header += struct.pack("<BBBBHHII", dimension, dimension, 0, 0,
                              1, 32, len(blob), offset)
        offset += len(blob)
    path.write_bytes(bytes(header) + b"".join(blobs))


def font(size: int) -> ImageFont.ImageFont:
    for path in (Path("C:/Windows/Fonts/segoeui.ttf"),
                 Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")):
        if path.exists():
            return ImageFont.truetype(str(path), size=size)
    return ImageFont.load_default()


def preview(frames: dict[int, Image.Image]) -> Image.Image:
    image = Image.new("RGB", (1056, 552), "#EEF1F4")
    draw = ImageDraw.Draw(image)
    title_font, label_font, small_font = font(28), font(19), font(16)
    draw.text((40, 28), "hdd-noise", font=title_font, fill="#253540")
    draw.text((40, 72), "Original HDD icon  /  transparent background", font=label_font,
              fill="#53636F")
    draw.rounded_rectangle((40, 126, 356, 458), radius=12, fill="white")
    image.paste(frames[256], (70, 158), frames[256])
    draw.text((116, 477), "256 px artwork", font=label_font, fill="#53636F")

    shown_sizes = (16, 20, 24, 32, 40, 48, 64)
    for top, background, foreground, label in (
        (126, "#FFFFFF", "#344650", "Light taskbar"),
        (316, "#20262C", "#D8E1E8", "Dark taskbar"),
    ):
        draw.rounded_rectangle((392, top, 1016, top + 168), radius=12, fill=background)
        draw.text((412, top + 15), label, font=label_font, fill=foreground)
        for index, size in enumerate(shown_sizes):
            center = 438 + index * 82
            image.paste(frames[size], (center - size // 2, top + 93 - size // 2),
                        frames[size])
            draw.text((center - 10, top + 134), str(size), font=small_font,
                      fill=foreground)
    return image


def validate(frames: dict[int, Image.Image], path: Path) -> None:
    raw = path.read_bytes()
    assert struct.unpack_from("<HHH", raw) == (0, 1, len(SIZES)), "Bad ICO header"
    for index, size in enumerate(SIZES):
        width, height, colors, reserved, planes, bits, count, offset = struct.unpack_from(
            "<BBBBHHII", raw, 6 + index * 16)
        assert (width or 256, height or 256) == (size, size)
        assert (colors, reserved, planes, bits) == (0, 0, 1, 32)
        with Image.open(io.BytesIO(raw[offset:offset + count])) as decoded:
            assert decoded.size == (size, size) and decoded.mode == "RGBA"
            assert decoded.tobytes() == frames[size].tobytes()
        histogram = frames[size].getchannel("A").histogram()
        assert histogram[0] and histogram[255], "Missing transparent or opaque pixels"
        assert sum(histogram[1:255]), "Missing antialiased alpha edge"
    with Image.open(path) as decoded_icon:
        assert decoded_icon.format == "ICO"
        assert decoded_icon.ico.sizes() == {(size, size) for size in SIZES}


def main() -> None:
    ASSETS.mkdir(exist_ok=True)
    frames = {size: render_svg(size) for size in SIZES}
    icon_path = ASSETS / "app-icon.ico"
    save_ico(frames, icon_path)
    frames[256].save(ASSETS / "app-icon.png", compress_level=9)
    preview(frames).save(ASSETS / "icon-preview.png", compress_level=9)
    validate(frames, icon_path)
    print("Validated 32-bit RGBA ICO frames: " + ", ".join(map(str, SIZES)))
    print("Original SVG, 256 px PNG and light/dark previews are in assets/.")


if __name__ == "__main__":
    main()
