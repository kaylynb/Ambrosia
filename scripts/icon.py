"""Remakes the icon files in assets/icon from each program's source image (see assets/icon/README.md).

Needs ImageMagick's `magick`, with its librsvg delegate for the game's SVG. Each size is made
from the source at that size rather than scaled down from a smaller one. The .ico is put together
here from PNG frames, which Windows reads from Vista on, as ImageMagick stores its 256 pixel frame
uncompressed.
"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ICON = Path(__file__).resolve().parent.parent / "assets" / "icon"
WINDOW_SIZES = [32, 64, 128, 256]
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SWITCH_BACKGROUND = "#1b2428"


def read_source(source: Path, size: int) -> list[str]:
    # The SVG is 512 pixels at 96 dpi; drawn at four times the size, then scaled to it. A PNG is
    # scaled from its full size.
    if source.suffix == ".svg":
        return ["-background", "none", "-density", str(96 * size * 4 // 512), str(source)]
    return [str(source)]


def render(source: Path, size: int, out: Path) -> None:
    subprocess.run(["magick", *read_source(source, size), "-resize", f"{size}x{size}", "-depth", "8",
                    "-strip", f"PNG32:{out}"], check=True)


def write_ico(frames: list[tuple[int, bytes]], out: Path) -> None:
    header = struct.pack("<HHH", 0, 1, len(frames))
    offset = len(header) + 16 * len(frames)
    entries = b""
    for size, png in frames:
        side = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(png), offset)
        offset += len(png)
    out.write_bytes(header + entries + b"".join(png for _, png in frames))


def make(program: str, source: str, switch: bool) -> None:
    folder = ICON / program
    source_path = folder / source
    # Each size is drawn once, for the .ico, and the window's sizes are kept from those.
    frames = []
    with tempfile.TemporaryDirectory() as temp:
        for size in ICO_SIZES:
            png = Path(temp) / f"{size}.png"
            render(source_path, size, png)
            frames.append((size, png.read_bytes()))
    write_ico(frames, folder / f"{program}.ico")
    for size, png in frames:
        if size in WINDOW_SIZES:
            (folder / f"{size}.png").write_bytes(png)
    if switch:
        # A JPEG has no transparency, so the icon sits on a dark background with a margin.
        subprocess.run(["magick", *read_source(source_path, 256), "-resize", "230x230",
                        "-background", SWITCH_BACKGROUND, "-gravity", "center", "-extent", "256x256",
                        "-flatten", "-quality", "92", "-strip", str(folder / "switch.jpg")], check=True)


def main() -> int:
    make("ambrosia", "ambrosia.svg", switch=True)
    # The editor has no Switch build.
    make("dented", "dented.png", switch=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
