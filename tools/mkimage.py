#!/usr/bin/env python3
"""Create or inspect RTL8196C firmware images:
  1. 'rapp' (default): 52-byte RAPP header + payload for Slot A/B & OTA
  2. 'cs6c': 16-byte factory loader header + payload + 16-bit zero-sum word for stage-2 (0x10000)

Usage:
    python tools/mkimage.py build/13_multi_socket/app.bin -o build/13_multi_socket/app.rapp
    python tools/mkimage.py build/stage2/app.bin --type cs6c --load 0xA0500000 --burn 0x10000 -o build/stage2/stage2.cs6c
    python tools/mkimage.py --info build/13_multi_socket/app.rapp
"""
import argparse
import struct
import sys
import time
import zlib
from pathlib import Path

APP_MAGIC = 0x52415050  # 'RAPP'
APP_HDR_VERSION = 1
APP_HDR_FMT = ">IIIIIII16sII"  # 52 bytes


def cs6c_checksum16(data: bytes) -> int:
    if len(data) % 2:
        data += b"\x00"
    words = struct.unpack(f">{len(data)//2}H", data)
    s = sum(words) & 0xFFFF
    return (-s) & 0xFFFF


def make_cs6c(payload: bytes, start_addr: int, burn_addr: int, sig: bytes = b"cs6c") -> bytes:
    if len(payload) % 2:
        payload += b"\x00"
    chk = cs6c_checksum16(payload)
    body = payload + struct.pack(">H", chk)
    hdr = struct.pack(">4sIII", sig, start_addr, burn_addr, len(body))
    return hdr + body


def make_rapp(payload: bytes, load_addr: int, entry: int, seq: int = 1,
              version: str = "0.1.0", build_time: int | None = None) -> bytes:
    if build_time is None:
        build_time = int(time.time())
    ver_bytes = version.encode("ascii", "replace")[:15].ljust(16, b"\x00")
    payload_crc = zlib.crc32(payload) & 0xFFFFFFFF
    hdr_wo_crc = struct.pack(">IIIIIII16sI",
                             APP_MAGIC, APP_HDR_VERSION, seq,
                             load_addr, entry, len(payload),
                             payload_crc, ver_bytes, build_time)
    hdr_crc = zlib.crc32(hdr_wo_crc) & 0xFFFFFFFF
    return hdr_wo_crc + struct.pack(">I", hdr_crc) + payload


def make_ptable() -> bytes:
    entries = [
        (b"boot",   0x010000, 0x00E000, 0),
        (b"state",  0x01E000, 0x001000, 0),
        (b"ptable", 0x01F000, 0x001000, 0),
        (b"appA",   0x020000, 0x200000, 0),
        (b"appB",   0x220000, 0x200000, 0),
        (b"lfs",    0x420000, 0x3E0000, 0),
    ]
    body = struct.pack(">IHH", 0x52505431, 1, len(entries))
    for i in range(8):
        if i < len(entries):
            name, off, sz, flg = entries[i]
            body += struct.pack(">8sIII", name.ljust(8, b"\x00"), off, sz, flg)
        else:
            body += b"\x00" * 20
    c = zlib.crc32(body) & 0xFFFFFFFF
    return body + struct.pack(">I", c)


def inspect_image(path: Path) -> int:
    data = path.read_bytes()
    if len(data) >= 172 and data[:4] == b"RPT1":
        magic, ver, cnt = struct.unpack(">IHH", data[:8])
        hcrc = struct.unpack(">I", data[168:172])[0]
        calc = zlib.crc32(data[:168]) & 0xFFFFFFFF
        print(f"{path}: RPT1 v{ver} entries={cnt} crc={hcrc:#010x} ({'OK' if hcrc == calc else 'BAD'})")
        return 0 if hcrc == calc else 1
    if len(data) >= 52 and data[:4] == b"RAPP":
        magic, hv, seq, load, entry, length, pcrc, ver, btime, hcrc = struct.unpack(APP_HDR_FMT, data[:52])
        calc_hcrc = zlib.crc32(data[:48]) & 0xFFFFFFFF
        payload = data[52:52 + length]
        calc_pcrc = zlib.crc32(payload) & 0xFFFFFFFF
        vstr = ver.split(b"\x00", 1)[0].decode("ascii", "replace")
        print(f"{path}: RAPP v{hv} seq={seq} version={vstr!r}")
        print(f"  load={load:#010x} entry={entry:#010x} payload={length} bytes")
        print(f"  hdr_crc={hcrc:#010x} ({'OK' if hcrc == calc_hcrc else 'BAD'}) "
              f"payload_crc={pcrc:#010x} ({'OK' if pcrc == calc_pcrc and len(payload) == length else 'BAD'})")
        return 0 if (hcrc == calc_hcrc and pcrc == calc_pcrc and len(payload) == length) else 1
    if len(data) >= 18 and data[:4] in (b"cs6c", b"cr6c"):
        sig, start, burn, length = struct.unpack(">4sIII", data[:16])
        body = data[16:16 + length]
        rem = cs6c_checksum16(body)
        print(f"{path}: {sig.decode()} start={start:#010x} burn={burn:#010x} len={length:#x}")
        print(f"  16-bit sum check: {'OK (0x0000)' if rem == 0 and len(body) == length else f'BAD ({rem:#06x})'}")
        return 0 if (rem == 0 and len(body) == length) else 1
    print(f"{path}: raw binary ({len(data)} bytes, crc32={zlib.crc32(data) & 0xFFFFFFFF:#010x})")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path, help="input .bin or image file")
    ap.add_argument("-o", "--output", type=Path, help="output image file")
    ap.add_argument("--type", choices=["rapp", "cs6c"], default="rapp")
    ap.add_argument("--load", default="0xA0500000", help="load address (hex)")
    ap.add_argument("--entry", default="", help="entry address (hex, defaults to --load)")
    ap.add_argument("--burn", default="0x10000", help="flash burn offset for cs6c")
    ap.add_argument("--seq", type=int, default=1, help="initial RAPP sequence number")
    ap.add_argument("--version", default="0.1.0", help="15-char version string for RAPP")
    ap.add_argument("--info", action="store_true", help="inspect existing image")
    args = ap.parse_args()

    if args.info or not args.output:
        if args.info:
            sys.exit(inspect_image(args.input))
        args.output = args.input.with_suffix("." + args.type)

    payload = args.input.read_bytes()
    load_addr = int(args.load, 16)
    entry_addr = int(args.entry, 16) if args.entry else load_addr

    if args.type == "cs6c":
        burn_addr = int(args.burn, 16)
        out = make_cs6c(payload, load_addr, burn_addr)
    else:
        out = make_rapp(payload, load_addr, entry_addr, seq=args.seq, version=args.version)

    args.output.write_bytes(out)
    inspect_image(args.output)


if __name__ == "__main__":
    main()
