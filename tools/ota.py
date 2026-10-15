#!/usr/bin/env python3
"""Over-The-Air (Ethernet TCP) firmware uploader & status tool for OpenRTL8196C.

Examples:
    python tools/ota.py --status
    python tools/ota.py --ram build/15_ota/app.bin
    python tools/ota.py --test-flash build/15_ota/app.bin
    python tools/ota.py --flash build/15_ota/app.bin --reboot
"""
import argparse
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench    # noqa: E402
import mkimage  # noqa: E402

OTA_PORT = 9698

CMD_STATUS     = 0
CMD_RAM        = 1
CMD_FLASH      = 2
CMD_TEST_FLASH = 3
CMD_RAW_FLASH  = 4


def send_ota(ip: str, port: int, mode: int, payload: bytes, target_addr: int = 0, timeout: float = 30.0) -> str:
    hdr = struct.pack(">4sIII", b"ROTA", mode, len(payload), target_addr)
    t0 = time.time()
    with socket.create_connection((ip, port), timeout=5.0) as s:
        s.settimeout(timeout)
        s.sendall(hdr)
        if payload:
            sent = 0
            chunk = 4096
            while sent < len(payload):
                n = min(chunk, len(payload) - sent)
                s.sendall(payload[sent:sent + n])
                sent += n
        resp = b""
        while not resp.endswith(b"\n"):
            d = s.recv(1024)
            if not d:
                break
            resp += d
    dt = time.time() - t0
    text = resp.decode("ascii", "replace").strip()
    if payload:
        kbps = (len(payload) / 1024.0) / max(dt, 0.001)
        print(f"[ota] sent {len(payload)} bytes in {dt*1000:.0f} ms ({kbps:.1f} KB/s)")
    print(f"[ota] {text}")
    return text


def ensure_rapp(data: bytes, load_addr: int, version: str) -> bytes:
    if len(data) >= 52 and data[:4] == b"RAPP":
        return data
    return mkimage.make_rapp(data, load_addr, load_addr, seq=1, version=version)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", nargs="?", type=Path, help=".bin or .rapp image file")
    ap.add_argument("--ip", default=bench.BOARD_IP, help="board IP (default from bench.ini)")
    ap.add_argument("--port", type=int, default=OTA_PORT)
    ap.add_argument("--status", action="store_true", help="query OTA/partition/slot status")
    ap.add_argument("--ram", action="store_true", help="upload to RAM and jump immediately (no flash write)")
    ap.add_argument("--test-flash", action="store_true", help="test RAPP flash write/verify/restore at 0x7E0000")
    ap.add_argument("--flash", action="store_true", help="write RAPP image to inactive slot A/B")
    ap.add_argument("--raw-flash", default="", metavar="HEX_OFFSET", help="write raw file at flash offset")
    ap.add_argument("--reboot", action="store_true", help="reboot after --flash")
    ap.add_argument("--load", default=bench.LOAD, help="RAM load address (hex, default from bench.ini)")
    ap.add_argument("--version", default="0.2.0", help="RAPP version tag when wrapping a raw .bin")
    args = ap.parse_args()

    if args.status or not args.image:
        resp = send_ota(args.ip, args.port, CMD_STATUS, b"")
        sys.exit(0 if resp.startswith("OK ") else 1)

    raw = args.image.read_bytes()
    load_addr = int(args.load, 16)

    if args.raw_flash:
        off = int(args.raw_flash, 16)
        resp = send_ota(args.ip, args.port, CMD_RAW_FLASH, raw, target_addr=off, timeout=60.0)
    elif args.test_flash:
        rapp = ensure_rapp(raw, load_addr, args.version)
        resp = send_ota(args.ip, args.port, CMD_TEST_FLASH, rapp, target_addr=0x7E0000, timeout=60.0)
    elif args.flash:
        rapp = ensure_rapp(raw, load_addr, args.version)
        resp = send_ota(args.ip, args.port, CMD_FLASH, rapp, target_addr=(1 if args.reboot else 0), timeout=60.0)
    else:
        # Default to --ram if --ram or no flash flag given
        rapp = ensure_rapp(raw, load_addr, args.version)
        resp = send_ota(args.ip, args.port, CMD_RAM, rapp, target_addr=load_addr, timeout=15.0)

    sys.exit(0 if resp.startswith("OK ") else 1)


if __name__ == "__main__":
    main()
