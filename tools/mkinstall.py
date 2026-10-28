#!/usr/bin/env python3
"""Build a full-flash OpenBeken install image for UART flashers (BK7231GUIFlashTool, uartflash.py).

    python tools/mkinstall.py            # use the existing stage-2 / ptable / OBK build outputs
    python tools/mkinstall.py --build    # rebuild them first (same steps as flash_obk.py)

Output: build/obk/openbeken_rtl8196c_install.bin, a flash image that starts at
offset 0 and is written at offset 0.  Flashers skip the first 64 KB (factory
loader + H601/6V03 settings, 0xFF here) and program the rest:

    0x010000  stage-2 chooser (cs6c)
    0x01E000  boot state, blank (old records would refer to old images)
    0x01F000  RPT1 partition table
    0x020000  OpenBeken RAPP image in slot A, seq 1
    0x220000  first sector of slot B, blank, so an older OTA image there
              (possibly with a higher seq) can no longer win
LittleFS / OBK config at 0x420000 are not touched.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import mkimage  # noqa: E402

STAGE2_OFF, STAGE2_MAX = 0x010000, 0x00E000
PTABLE_OFF = 0x01F000
SLOTA_OFF, SLOTA_SIZE = 0x020000, 0x200000
SLOTB_OFF = 0x220000
SECTOR = 0x1000
OUT = ROOT / "build" / "obk" / "openbeken_rtl8196c_install.bin"


def rapp_with_seq(img: bytes, seq: int) -> bytes:
    hdr = bytearray(img[:56])
    if hdr[:4] != b"RAPP":
        sys.exit("OpenBeken image has no RAPP header")
    struct.pack_into(">I", hdr, 8, seq)
    struct.pack_into(">I", hdr, 52, zlib.crc32(bytes(hdr[:52])))
    return bytes(hdr) + img[56:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", action="store_true", help="rebuild stage-2, ptable and OpenBeken first")
    ap.add_argument("-o", "--out", type=Path, default=OUT)
    args = ap.parse_args()

    if args.build:
        import flash_obk
        stage2, ptable, obk = flash_obk.build_artifacts()
    else:
        stage2 = ROOT / "build" / "16_stage2" / "stage2.cs6c"
        ptable = ROOT / "build" / "obk" / "ptable.bin"
        obk = ROOT / "build" / "obk" / "openbeken_rtl8196c.img"
    for p in (stage2, ptable, obk):
        if not p.exists():
            sys.exit(f"{p.relative_to(ROOT)} missing: run with --build")

    s2 = stage2.read_bytes()
    app = rapp_with_seq(obk.read_bytes(), 1)
    if len(s2) > STAGE2_MAX:
        sys.exit(f"stage-2 is {len(s2)} bytes, the partition holds {STAGE2_MAX}")
    if len(app) > SLOTA_SIZE:
        sys.exit(f"OpenBeken image is {len(app)} bytes, slot A holds {SLOTA_SIZE}")

    img = bytearray(b"\xFF" * (SLOTB_OFF + SECTOR))
    img[STAGE2_OFF:STAGE2_OFF + len(s2)] = s2
    pt = ptable.read_bytes()
    img[PTABLE_OFF:PTABLE_OFF + len(pt)] = pt
    img[SLOTA_OFF:SLOTA_OFF + len(app)] = app

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(img)
    data_sectors = sum(1 for o in range(STAGE2_OFF, len(img), SECTOR) if img[o:o + SECTOR] != b"\xFF" * SECTOR)
    print(f"stage-2 {len(s2)} B, ptable {len(pt)} B, OpenBeken {len(app)} B (seq 1)")
    print(f"{args.out.relative_to(ROOT)}: {len(img)} bytes, write at offset 0; "
          f"{data_sectors} sectors carry data, the rest are erased")


if __name__ == "__main__":
    main()
