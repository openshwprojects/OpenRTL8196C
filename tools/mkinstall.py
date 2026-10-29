#!/usr/bin/env python3
"""Build a full-flash OpenBeken install image for UART flashers (BK7231GUIFlashTool, uartflash.py).

    python tools/build_obk.py            # OpenBeken first (build/obk/openbeken_rtl8196c.img)
    python tools/mkinstall.py            # builds stage-2 and the partition table, writes the image
    python tools/mkinstall.py -o out.bin --obk-img other.img

Output: build/obk/openbeken_rtl8196c_install.bin, a flash image that starts at
offset 0 and is written at offset 0 (OpenBeken releases ship it as
OpenRTL8196C_QIO_<version>.bin).  Flashers skip the first 64 KB (factory
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
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import mkimage  # noqa: E402

STAGE2_OFF, STAGE2_MAX = 0x010000, 0x00E000
STAGE2_LOAD, STAGE2_STACK = 0xA0500000, 0xA0600000
PTABLE_OFF = 0x01F000
SLOTA_OFF, SLOTA_SIZE = 0x020000, 0x200000
SLOTB_OFF = 0x220000
SECTOR = 0x1000
OUT = ROOT / "build" / "obk" / "openbeken_rtl8196c_install.bin"
OBK_IMG = ROOT / "build" / "obk" / "openbeken_rtl8196c.img"


def build_stage2() -> bytes:
    subprocess.run([sys.executable, str(ROOT / "tools" / "build.py"), "examples/16_stage2",
                    "--load", f"{STAGE2_LOAD:#010x}", "--stack", f"{STAGE2_STACK:#010x}"],
                   cwd=ROOT, check=True)
    payload = (ROOT / "build" / "16_stage2" / "app.bin").read_bytes()
    cs6c = mkimage.make_cs6c(payload, STAGE2_LOAD, STAGE2_OFF)
    (ROOT / "build" / "16_stage2" / "stage2.cs6c").write_bytes(cs6c)
    return cs6c


RAPP_HDR = struct.calcsize(mkimage.APP_HDR_FMT)      # 52 bytes, header_crc32 is its last word


def rapp_with_seq(img: bytes, seq: int) -> bytes:
    hdr = bytearray(img[:RAPP_HDR])
    if hdr[:4] != b"RAPP":
        sys.exit("OpenBeken image has no RAPP header")
    struct.pack_into(">I", hdr, 8, seq)
    struct.pack_into(">I", hdr, RAPP_HDR - 4, zlib.crc32(bytes(hdr[:RAPP_HDR - 4])))
    return bytes(hdr) + img[RAPP_HDR:]


def check_rapp(app: bytes):
    """The same checks stage-2 makes before it boots a slot."""
    f = struct.unpack(mkimage.APP_HDR_FMT, app[:RAPP_HDR])
    if zlib.crc32(app[:RAPP_HDR - 4]) != f[9]:
        sys.exit("slot A image: bad header CRC")
    if zlib.crc32(app[RAPP_HDR:RAPP_HDR + f[5]]) != f[6]:
        sys.exit("slot A image: bad payload CRC")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", type=Path, default=OUT)
    ap.add_argument("--obk-img", type=Path, default=OBK_IMG, help="RAPP image from tools/build_obk.py")
    args = ap.parse_args()

    if not args.obk_img.exists():
        sys.exit(f"{args.obk_img} missing: run tools/build_obk.py first")
    s2 = build_stage2()
    pt = mkimage.make_ptable()
    app = rapp_with_seq(args.obk_img.read_bytes(), 1)
    if len(s2) > STAGE2_MAX:
        sys.exit(f"stage-2 is {len(s2)} bytes, the partition holds {STAGE2_MAX}")
    if len(app) > SLOTA_SIZE:
        sys.exit(f"OpenBeken image is {len(app)} bytes, slot A holds {SLOTA_SIZE}")

    img = bytearray(b"\xFF" * (SLOTB_OFF + SECTOR))
    img[STAGE2_OFF:STAGE2_OFF + len(s2)] = s2
    img[PTABLE_OFF:PTABLE_OFF + len(pt)] = pt
    img[SLOTA_OFF:SLOTA_OFF + len(app)] = app
    check_rapp(bytes(img[SLOTA_OFF:SLOTA_OFF + len(app)]))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(img)
    data_sectors = sum(1 for o in range(STAGE2_OFF, len(img), SECTOR) if img[o:o + SECTOR] != b"\xFF" * SECTOR)
    print(f"stage-2 {len(s2)} B, ptable {len(pt)} B, OpenBeken {len(app)} B (seq 1)")
    print(f"{args.out}: {len(img)} bytes, write at offset 0; "
          f"{data_sectors} sectors carry data, the rest are erased")


if __name__ == "__main__":
    main()
