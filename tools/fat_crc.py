#!/usr/bin/env python3
"""CRC32 and size of every file on a drive, in the same format as
examples/20_usb_storage_soak (FAT) and examples/22_usb_ntfs (NTFS) print,
so the two can be compared.

    python tools/fat_crc.py E:\\                     # pendrive plugged into the PC
    python tools/fat_crc.py E:\\ --compare board.log # diff against the board's output

Board names are UTF-8 (NTFS) or have '?' for non-ASCII characters (FAT).

Uses the standard CRC-32 (zlib), the same as the SDK's crc32().
"""
import argparse
import os
import re
import sys
import zlib


def scan(root):
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = "/" + os.path.relpath(full, root).replace(os.sep, "/")
            try:
                with open(full, "rb") as f:
                    data = f.read()
            except OSError as e:
                print(f"skip {rel}: {e}", file=sys.stderr)
                continue
            out[rel] = (len(data), zlib.crc32(data) & 0xFFFFFFFF)
    return out


def parse_board(path):
    out = {}
    rx = re.compile(r"FILE\s+(\d+)\s+crc32 ([0-9a-f]{8})\s+(/.*?)\s*(\(SHORT READ\))?$")
    for line in open(path, encoding="utf-8", errors="replace"):
        m = rx.search(line.rstrip())
        if m:
            out[m.group(3)] = (int(m.group(1)), int(m.group(2), 16))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--compare", help="UART log of examples/20_usb_storage_soak")
    args = ap.parse_args()
    pc = scan(args.root)
    if not args.compare:
        for rel, (size, crc) in pc.items():
            print(f"FILE {size:10d}  crc32 {crc:08x}  {rel}")
        return
    board = parse_board(args.compare)
    ok = bad = missing = 0
    for rel, (size, crc) in pc.items():
        # the board shows non-ASCII name characters as '?'
        key = next((b for b in board if len(b) == len(rel) and all(x == y or y == "?" for x, y in zip(rel, b))), None)
        if key is None:
            missing += 1
            print(f"not on board list: {rel}")
        elif board[key] == (size, crc):
            ok += 1
        else:
            bad += 1
            print(f"MISMATCH {rel}: pc {size} {crc:08x}  board {board[key][0]} {board[key][1]:08x}")
    print(f"{ok} files identical, {bad} different, {missing} not compared")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
