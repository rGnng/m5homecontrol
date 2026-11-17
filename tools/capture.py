#!/usr/bin/env python3
"""Capture a screenshot from the M5Stack Home Control UI over USB serial.

The firmware sends a raw RGB565 frame when it receives an uppercase 'S'.
Protocol: b"SHOT" + <u16 width> + <u16 height> + <u32 length> + pixels + b"DONE"

Usage:
    python3 tools/capture.py --port /dev/cu.usbserial-5A6C0461241
    python3 tools/capture.py --out preview/home.png
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: python3 -m pip install pyserial")

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: python3 -m pip install Pillow")


MAGIC = b"SHOTIMG"
DONE = b"DONE"


def read_exact(ser: serial.Serial, size: int, timeout: float) -> bytes:
    deadline = time.time() + timeout
    data = bytearray()
    while len(data) < size and time.time() < deadline:
        chunk = ser.read(size - len(data))
        if chunk:
            data.extend(chunk)
    if len(data) != size:
        raise TimeoutError(f"expected {size} bytes, received {len(data)}")
    return bytes(data)


def wait_for(ser: serial.Serial, token: bytes, timeout: float) -> None:
    deadline = time.time() + timeout
    buf = bytearray()
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        buf.extend(b)
        if len(buf) > len(token):
            del buf[0]
        if buf == token:
            return
    raise TimeoutError(f"timed out waiting for {token!r}")


def rgb565le_to_rgb(pixels: bytes, width: int, height: int) -> bytes:
    out = bytearray(width * height * 3)
    o = 0
    for i in range(0, len(pixels), 2):
        v = pixels[i] | (pixels[i + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        out[o] = (r * 255 + 15) // 31
        out[o + 1] = (g * 255 + 31) // 63
        out[o + 2] = (b * 255 + 15) // 31
        o += 3
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/cu.usbserial-5A6C0461241")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", default="preview/home.png")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--reset", action="store_true", help="toggle DTR/RTS before capture")
    args = parser.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        if args.reset:
            ser.setDTR(False)
            ser.setRTS(True)
            time.sleep(0.15)
            ser.setRTS(False)
            time.sleep(2.0)
        ser.reset_input_buffer()
        time.sleep(0.3)
        ser.write(b"S")
        ser.flush()

        wait_for(ser, MAGIC, args.timeout)
        header = read_exact(ser, 8, args.timeout)
        width, height, length = struct.unpack("<HHI", header)
        if width != 320 or height != 240:
            raise ValueError(f"unexpected frame size: {width}x{height}")
        if length != width * height * 2:
            raise ValueError(f"unexpected payload length: {length}")

        pixels = read_exact(ser, length, args.timeout)
        wait_for(ser, DONE, args.timeout)

    rgb = rgb565le_to_rgb(pixels, width, height)
    Image.frombytes("RGB", (width, height), rgb).save(out)
    print(f"saved {out} ({width}x{height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
