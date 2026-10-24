#!/usr/bin/env python3
"""Standardized build & SPI flash installer for OpenBeken on Realtek RTL8196C.

Safety guarantees:
  - NEVER touches 0x000000..0x00FFFF (factory <RealTek> root bootloader + H601/6V03
    factory MAC & RF calibration). Pressing ESC on UART at power-on ALWAYS enters
    the <RealTek> rescue prompt.
  - Verifies that an 8 MB full flash backup exists in backups/ before writing.
  - Programs and CRC32-verifies:
      0x01F000 (4 KB)   : RPT1 partition table
      0x01E000 (4 KB)   : BTRY/BCNF bootstate sector (initialized to 0xFF on first install)
      0x020000 (2 MB)   : App Slot A (RAPP-wrapped OpenBeken firmware)
      0x220000 (2 MB)   : App Slot B (A/B OTA slot)
      0x010000 (56 KB)  : Stage-2 A/B chooser bootloader (stage2.cs6c, loaded at 0xA0300000)
      0x420000 (3.875M) : Persistent OpenBeken Config + FlashVars + LittleFS

Usage:
  python tools/flash_obk.py                 # Full build + flash + cold-boot verification
  python tools/flash_obk.py --update-only   # Update inactive A/B slot only & reboot
  python tools/flash_obk.py --restore-stock backups/flash_20261014_222426.bin
"""
import argparse
import json
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
import serial

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import bench    # noqa: E402
import mkimage  # noqa: E402
import ota      # noqa: E402

PROTECTED_END = 0x010000
STAGE2_FLASH_OFF = 0x010000
STAGE2_MAX_SIZE = 0x00E000
STAGE2_LOAD_ADDR = 0xA0500000
STAGE2_STACK_TOP = 0xA0600000
BOOTSTATE_FLASH_OFF = 0x01E000
PTABLE_FLASH_OFF = 0x01F000
LFS_FLASH_OFF = 0x420000


def run_cmd(cmd: list[str]):
    print(f"[flash_obk] $ {' '.join(str(c) for c in cmd)}")
    r = subprocess.run([str(c) for c in cmd], cwd=str(ROOT))
    if r.returncode != 0:
        sys.exit(f"[flash_obk] Command failed with exit code {r.returncode}")


def check_backup_exists():
    bdir = ROOT / "backups"
    backups = sorted(bdir.glob("flash_*.bin")) if bdir.exists() else []
    valid = [p for p in backups if p.stat().st_size == 8 * 1024 * 1024]
    if not valid:
        sys.exit("[flash_obk] SAFETY ABORT: No 8 MB flash backup found in backups/flash_*.bin!")
    print(f"[flash_obk] Verified 8 MB flash backup present: {valid[0].name}")
    return valid[0]


def raw_flash_safe(ip: str, port: int, offset: int, data: bytes, label: str):
    if offset < PROTECTED_END:
        sys.exit(f"[flash_obk] SAFETY ABORT: Attempted write at {offset:#x} (< {PROTECTED_END:#x})")
    if offset & 0xFFF:
        sys.exit(f"[flash_obk] SAFETY ABORT: Flash offset {offset:#x} is not 4 KB sector-aligned")
    print(f"[flash_obk] Programming {label} at flash offset 0x{offset:06X} ({len(data)} bytes)...")
    resp = ota.send_ota(ip, port, ota.CMD_RAW_FLASH, data, target_addr=offset, timeout=90.0)
    if not resp.startswith("OK "):
        sys.exit(f"[flash_obk] Failed to program {label} at 0x{offset:06X}: {resp}")
    return resp


def probe_ota_status(ip: str, port: int, timeout: float = 15.0) -> str | None:
    try:
        return ota.send_ota(ip, port, ota.CMD_STATUS, b"", timeout=timeout)
    except Exception:
        return None


def wait_for_ota(ip: str, port: int, timeout: float = 30.0) -> str:
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = probe_ota_status(ip, port, timeout=15.0)
        if st and st.startswith("OK "):
            return st
        time.sleep(0.5)
    sys.exit(f"[flash_obk] Timed out waiting for OTA server on {ip}:{port}")


def ensure_ota_reachable(ip: str, port: int):
    run_cmd([sys.executable, "tools/build.py", "examples/15_ota"])
    ota_app = (ROOT / "build" / "15_ota" / "app.bin").read_bytes()

    for _ in range(3):
        st = probe_ota_status(ip, port, timeout=15.0)
        if st and st.startswith("OK "):
            print(f"[flash_obk] Router OTA service online at {ip}:{port}; hot-loading fresh 15_ota flasher into RAM...")
            ota.send_ota(ip, port, ota.CMD_RAM, ota_app, target_addr=0xA0500000, timeout=15.0)
            time.sleep(1.5)
            return wait_for_ota(ip, port, timeout=15.0)
        time.sleep(0.5)

    print(f"[flash_obk] OTA port {ip}:{port} not responding; bootstrapping 15_ota over UART ({bench.PORT})...")
    run_cmd([sys.executable, "tools/flash.py", "build/15_ota/app.bin", "--listen", "3"])
    return wait_for_ota(ip, port, timeout=30.0)


def build_artifacts():
    print("\n=== Step 1: Building Stage-2 Bootloader, Partition Table & OpenBeken ===")
    run_cmd([
        sys.executable, "tools/build.py", "examples/16_stage2",
        "--load", f"0x{STAGE2_LOAD_ADDR:08X}",
        "--stack", f"0x{STAGE2_STACK_TOP:08X}",
    ])
    s2_bin = (ROOT / "build" / "16_stage2" / "app.bin").read_bytes()
    s2_cs6c = mkimage.make_cs6c(s2_bin, STAGE2_LOAD_ADDR, STAGE2_FLASH_OFF)
    if len(s2_cs6c) > STAGE2_MAX_SIZE:
        sys.exit(f"[flash_obk] stage2.cs6c ({len(s2_cs6c)} B) exceeds STAGE2_MAX_SIZE ({STAGE2_MAX_SIZE} B)")
    s2_path = ROOT / "build" / "16_stage2" / "stage2.cs6c"
    s2_path.write_bytes(s2_cs6c)
    mkimage.inspect_image(s2_path)

    run_cmd([sys.executable, "tools/build_obk.py"])
    obk_img_path = ROOT / "build" / "obk" / "openbeken_rtl8196c.img"

    ptable_bytes = mkimage.make_ptable()
    ptable_path = ROOT / "build" / "obk" / "ptable.bin"
    ptable_path.write_bytes(ptable_bytes)
    mkimage.inspect_image(ptable_path)

    return s2_path, ptable_path, obk_img_path


def verify_cold_boot(ip: str, com_port: str, baud: int, power_cycle: bool = True):
    print("\n=== Step 5: Cold Power-Cycle & Standalone SPI Flash Boot Verification ===")
    with serial.Serial(com_port, baud, timeout=0.2) as ser:
        ser.reset_input_buffer()
        if power_cycle:
            print("[flash_obk] Power-cycling router via bench socket (hands-off, no ESC pressed)...")
            import flash  # noqa: E402
            print(f"[obk] power off -> {flash.obk(bench.OBK, 'Power OFF')}")
            time.sleep(4.0)
            ser.reset_input_buffer()
            print(f"[obk] power on  -> {flash.obk(bench.OBK, 'Power ON')}")
        else:
            print("[flash_obk] Triggering hardware watchdog reset into ROM bootloader...")
            try:
                req = urllib.request.Request(f"http://{ip}/api/cmnd", data=b"reboot", method="POST")
                urllib.request.urlopen(req, timeout=3.0).read()
            except Exception:
                pass

        log_buf = ""
        t0 = time.time()
        saw_rom = False
        saw_stage2 = False
        saw_obk = False
        ready_at = None
        while time.time() - t0 < 25.0:
            chunk = ser.read(4096)
            if chunk:
                txt = chunk.decode("ascii", "replace")
                log_buf += txt
                if "jump to image start=" in log_buf.lower() or "booting" in log_buf.lower():
                    saw_rom = True
                if "[stage2] OpenRTL8196C chooser" in log_buf:
                    saw_stage2 = True
                if "[obk-rtl8196c] Ready:" in log_buf and not saw_obk:
                    saw_obk = True
                    ready_at = time.time()
            if saw_obk and ready_at and (time.time() - ready_at >= 1.0):
                break

    print("--- Captured Cold-Boot UART Log ---")
    for line in log_buf.splitlines():
        s = line.strip().encode("ascii", "replace").decode("ascii")
        if s:
            print("  " + s)
    print("-----------------------------------")

    if not (saw_rom and saw_stage2 and saw_obk):
        sys.exit(
            f"[flash_obk] Cold-boot verification incomplete: "
            f"rom={saw_rom} stage2={saw_stage2} obk={saw_obk}"
        )

    # Verify HTTP /api/info and OTA status after cold boot (allow up to 25s for Windows NIC carrier recovery)
    deadline = time.time() + 25.0
    info = None
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(f"http://{ip}/api/info", timeout=2.0) as r:
                info = json.loads(r.read().decode("utf-8", "replace"))
                break
        except Exception:
            time.sleep(0.5)
    if not info or info.get("chipset") != "RTL8196C":
        sys.exit(f"[flash_obk] Router did not answer /api/info after cold boot: {info}")

    st_after = ota.send_ota(ip, ota.OTA_PORT, ota.CMD_STATUS, b"", timeout=5.0)
    print(f"[flash_obk] Post-boot /api/info: chipset={info['chipset']} mac={info['mac']} ip={info['ip']} build={info['build']}")
    print(f"[flash_obk] Post-boot OTA status: {st_after}")
    assert "ptable=flash" in st_after, f"Expected ptable=flash, got: {st_after}"
    assert "conf=1" in st_after, f"Expected confirmed boot (conf=1), got: {st_after}"
    print("\n[flash_obk] SUCCESS: OpenBeken is installed in SPI flash and cold-boots standalone!")


def restore_stock_flash(ip: str, port: int, backup_file: Path):
    data = backup_file.read_bytes()
    if len(data) != 8 * 1024 * 1024:
        sys.exit(f"[flash_obk] Invalid backup size {len(data)} (expected 8,388,608 bytes)")
    # Stock kernel + rootfs occupy 0x010000 .. 0x5E0000
    start = 0x010000
    end = 0x5E0000
    chunk_sz = 0x080000  # 512 KB per OTA_CMD_RAW_FLASH
    print(f"[flash_obk] Restoring stock firmware 0x{start:06X}..0x{end:06X} from {backup_file.name}...")
    for off in range(start, end, chunk_sz):
        blk = data[off:off + chunk_sz]
        raw_flash_safe(ip, port, off, blk, f"stock[0x{off:06X}..0x{off+len(blk):06X}]")
    print("[flash_obk] Stock firmware restored! Power-cycle the router to boot stock Linux.")


def main():
    ap = argparse.ArgumentParser(description="Standardized SPI Flash Installer for OpenBeken on RTL8196C")
    ap.add_argument("--ip", default=bench.BOARD_IP, help="Router IP address (default: 192.168.0.196)")
    ap.add_argument("--port", type=int, default=ota.OTA_PORT, help="TCP OTA port (default: 9698)")
    ap.add_argument("--com", default=bench.PORT, help="UART COM port (default from bench.ini)")
    ap.add_argument("--baud", type=int, default=bench.BAUD, help="UART baud rate (default: 38400)")
    ap.add_argument("--update-only", action="store_true", help="Only update inactive A/B app slot and reboot")
    ap.add_argument("--verify-only", action="store_true", help="Power-cycle and verify standalone cold-boot from SPI flash")
    ap.add_argument("--wipe-storage", action="store_true", help="Reset OpenBeken SPI config & LittleFS at 0x420000")
    ap.add_argument("--no-cold-verify", action="store_true", help="Skip cold power-cycle verification at the end")
    ap.add_argument("--restore-stock", type=Path, metavar="BACKUP_BIN", help="Restore stock Linux firmware (0x010000..0x5E0000) from backup")
    args = ap.parse_args()

    check_backup_exists()

    if args.verify_only:
        verify_cold_boot(args.ip, args.com, args.baud, power_cycle=True)
        return

    if args.restore_stock:
        ensure_ota_reachable(args.ip, args.port)
        restore_stock_flash(args.ip, args.port, args.restore_stock)
        return

    s2_path, ptable_path, obk_img_path = build_artifacts()
    obk_img = obk_img_path.read_bytes()

    print("\n=== Step 2: Connecting to Router Ethernet OTA Service ===")
    st = ensure_ota_reachable(args.ip, args.port)

    if args.update_only:
        print("\n=== Step 3: Flashing OpenBeken to Inactive A/B Slot ===")
        resp = ota.send_ota(args.ip, args.port, ota.CMD_FLASH, obk_img, target_addr=1, timeout=90.0)
        if not resp.startswith("OK "):
            sys.exit(f"[flash_obk] A/B slot update failed: {resp}")
        if not args.no_cold_verify:
            verify_cold_boot(args.ip, args.com, args.baud, power_cycle=False)
        return

    print("\n=== Step 3: Writing Partition Table, Boot State, App Slot & Stage-2 to SPI Flash ===")
    # 3a. Partition table at 0x01F000
    raw_flash_safe(args.ip, args.port, PTABLE_FLASH_OFF, ptable_path.read_bytes(), "RPT1 partition table")

    # 3b. Bootstate sector at 0x01E000 (format to 0xFF if unformatted)
    if "last=-1" in st or "recs=256" in st:
        raw_flash_safe(args.ip, args.port, BOOTSTATE_FLASH_OFF, b"\xFF" * 4096, "bootstate sector (0xFF init)")

    # 3c. Optional wipe of 0x420000 storage header
    if args.wipe_storage:
        raw_flash_safe(args.ip, args.port, LFS_FLASH_OFF, b"\xFF" * 4096, "SPI storage header reset")

    # 3d. OpenBeken RAPP image into appA (0x020000) / appB (0x220000)
    print(f"[flash_obk] Programming OpenBeken RAPP image ({len(obk_img)} bytes) into A/B slot...")
    resp = ota.send_ota(args.ip, args.port, ota.CMD_FLASH, obk_img, target_addr=0, timeout=90.0)
    if not resp.startswith("OK "):
        sys.exit(f"[flash_obk] Failed to program OpenBeken RAPP slot: {resp}")

    # 3e. Stage-2 chooser (stage2.cs6c) at 0x010000 (written LAST once ptable + app slot are valid!)
    raw_flash_safe(args.ip, args.port, STAGE2_FLASH_OFF, s2_path.read_bytes(), "Stage-2 bootloader (stage2.cs6c)")

    st_final = wait_for_ota(args.ip, args.port, timeout=15.0)
    print(f"[flash_obk] Flash programming complete! Status: {st_final}")
    assert "ptable=flash" in st_final, f"Expected ptable=flash in {st_final}"
    assert "pick=0" in st_final or "pick=1" in st_final, f"Expected valid boot pick in {st_final}"

    if not args.no_cold_verify:
        verify_cold_boot(args.ip, args.com, args.baud, power_cycle=True)
    else:
        print("\n=== Step 4: Booting Fresh OpenBeken Image in RAM ===")
        ota.send_ota(args.ip, args.port, ota.CMD_RAM, obk_img, target_addr=0x80500000, timeout=20.0)
        time.sleep(8.0)
        wait_for_ota(args.ip, args.port, timeout=15.0)


if __name__ == "__main__":
    main()
