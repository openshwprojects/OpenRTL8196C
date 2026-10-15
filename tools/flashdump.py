#!/usr/bin/env python3
"""Back up the board's SPI flash over TCP (examples/14_flash_backup must be running).

    python tools/flashdump.py                 # -> backups/flash_<date>.bin, sha256 printed
    python tools/flashdump.py --verify FILE   # dump again and compare with FILE
"""
import hashlib
import socket
import struct
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PORT = 9697


def dump(ip):
    s = socket.create_connection((ip, PORT), timeout=10)
    s.settimeout(10)
    hdr = b""
    while len(hdr) < 16:
        d = s.recv(16 - len(hdr))
        if not d:
            raise RuntimeError("connection closed before header")
        hdr += d
    magic, jedec, size, _ = struct.unpack(">4sIII", hdr)
    if magic != b"RTLF":
        raise RuntimeError(f"bad header {hdr!r}")
    print(f"flash: jedec id {jedec:06x}, size {size} bytes ({size // 1024 // 1024} MB)")
    data = bytearray()
    t0 = time.time()
    last = 0
    while len(data) < size:
        d = s.recv(65536)
        if not d:
            break
        data += d
        if len(data) - last >= 1 << 20:
            last = len(data)
            print(f"\r  {len(data) // 1024} KB  {len(data) / (time.time() - t0) / 1024:.0f} KB/s", end="", flush=True)
    s.close()
    dt = time.time() - t0
    print(f"\r  {len(data)} bytes in {dt:.1f} s ({len(data) / dt / 1024:.0f} KB/s)")
    if len(data) != size:
        raise RuntimeError(f"short dump: {len(data)} of {size}")
    return bytes(data)


def describe(data):
    sig = data[0x10000:0x10004]
    start, burn, ln = struct.unpack(">III", data[0x10004:0x10010])
    print(f"  image header at 0x10000: sig {sig!r} start {start:08x} burn {burn:08x} len {ln:#x}")
    for off in (0x120000, 0x100000, 0x110000):
        if data[off:off + 4] in (b"hsqs", b"sqsh"):
            print(f"  squashfs at {off:#x}")
    blank = sum(1 for i in range(0, len(data), 4096) if data[i:i + 4096] == b"\xff" * 4096)
    print(f"  {blank} of {len(data) // 4096} sectors are blank (0xFF)")


def main():
    ip = bench.BOARD_IP
    args = sys.argv[1:]
    if args and args[0] == "--verify":
        ref = Path(args[1]).read_bytes()
        data = dump(ip)
        if data == ref:
            print("verify OK: identical to", args[1])
            return
        diff = [i for i in range(0, len(data), 4096) if data[i:i + 4096] != ref[i:i + 4096]]
        print(f"verify FAILED: {len(diff)} sectors differ, first at {diff[0]:#x}" if diff else "verify FAILED: length differs")
        sys.exit(1)
    data = dump(ip)
    out_dir = ROOT / "backups"
    out_dir.mkdir(exist_ok=True)
    out = out_dir / f"flash_{datetime.now():%Y%m%d_%H%M%S}.bin"
    out.write_bytes(data)
    print(f"saved {out}")
    print(f"sha256 {hashlib.sha256(data).hexdigest()}")
    describe(data)


if __name__ == "__main__":
    main()
