#!/usr/bin/env python3
"""Talk to examples/31_uart_flasher (the BK7231GUIFlashTool stub) over UART0.

    python tools/uartflash.py info                       # power-cycle, load stub, show chip/flash/MAC
    python tools/uartflash.py read 0 0x800000 dump.bin   # read flash to a file
    python tools/uartflash.py write 0x20000 app.bin      # program (sector aligned; below 0x10000 is skipped)
    python tools/uartflash.py write 0 build/obk/openbeken_rtl8196c_install.bin   # full OBK install (tools/mkinstall.py)
    python tools/uartflash.py selftest                   # baud sweep, CRC checks, write+restore test
    python tools/uartflash.py --running info             # stub already up (at 115200), skip the loader

The stub is loaded the same way as any example (OBK power cycle, ESC, EW,
J); --no-cycle waits for a manual power cycle instead.  Same protocol as the
GUI flasher's ECRBaseFlasher: A5/5A frames with a sum8, XMODEM-1K for data.
"""
import argparse
import os
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402
import flash  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
STUB = ROOT / "build" / "31_uart_flasher" / "app.bin"
STUB_ENTRY = 0x80500000      # python tools/build.py examples/31_uart_flasher --load 0x80500000 --stack 0x80600000
IDLE_BAUD = 115200

CMD_FLASH_ERASE, CMD_BAUD, CMD_CHIP_INFO = 0x04, 0x07, 0x20
CMD_CRC32, CMD_FLASH_ID, CMD_XMODEM_WRITE, CMD_XMODEM_READ = 0x8F, 0x90, 0x91, 0x92
CMD_GET_MAC, CMD_REBOOT = 0x95, 0x9F
STATUS = {1: "ERROR", 2: "ADDR_ERROR", 3: "TYPE_ERROR", 4: "LEN_ERROR", 5: "CRC_ERROR", 0x10: "NOT_SUPPORTED"}

SOH, STX, EOT, ACK, NAK, CAN, C = 1, 2, 4, 6, 0x15, 0x18, 0x43


class StubError(RuntimeError):
    pass


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class Stub:
    def __init__(self, ser):
        self.ser = ser

    def _read(self, n, deadline):
        out = b""
        while len(out) < n:
            if time.time() > deadline:
                raise StubError(f"timeout ({len(out)}/{n} bytes)")
            out += self.ser.read(n - len(out))
        return out

    def cmd(self, ctype, params=b"", timeout=1.0, new_baud=None):
        frame = bytes([0xA5, ctype]) + struct.pack("<H", len(params)) + params
        frame += bytes([sum(frame) & 0xFF])
        self.ser.reset_input_buffer()
        self.ser.write(frame)
        self.ser.flush()
        if new_baud:
            time.sleep(len(frame) * 10 / self.ser.baudrate + 0.002)
            self.ser.baudrate = new_baud
        deadline = time.time() + timeout
        while self._read(1, deadline)[0] != 0x5A:
            pass
        hdr = b"\x5A" + self._read(3, deadline)
        n = hdr[2] | hdr[3] << 8
        rest = self._read(n + 2, deadline)
        if hdr[1] != ctype:
            raise StubError(f"reply type {hdr[1]:#x} for command {ctype:#x}")
        if (sum(hdr + rest[:-1]) & 0xFF) != rest[-1]:
            raise StubError("reply checksum mismatch")
        if rest[-2]:
            raise StubError(f"command {ctype:#x} failed: {STATUS.get(rest[-2], hex(rest[-2]))}")
        return rest[:n]

    def set_baud(self, baud):
        self.cmd(CMD_BAUD, struct.pack("<I", baud), timeout=2.0, new_baud=baud)

    def chip_info(self):
        return struct.unpack("<II", self.cmd(CMD_CHIP_INFO)[:8])

    def rx_errors(self):
        """Stub's UART receive error counters: overrun, parity, framing, break."""
        return struct.unpack("<IIII", self.cmd(CMD_CHIP_INFO)[8:24])

    def flash_id(self):
        return self.cmd(CMD_FLASH_ID)[:3]

    def crc32(self, addr, length):
        return struct.unpack("<I", self.cmd(CMD_CRC32, struct.pack("<II", addr, length), timeout=20.0))[0]

    def erase(self, addr, length):
        self.cmd(CMD_FLASH_ERASE, struct.pack("<II", addr, length), timeout=10.0)

    def read(self, addr, length, progress=True):
        self.cmd(CMD_XMODEM_READ, struct.pack("<II", addr, length))
        out = bytearray()
        expect = 1
        t_c = 0
        started = False
        while True:
            if not started and time.time() - t_c > 0.25:
                self.ser.write(bytes([C]))
                t_c = time.time()
            b = self.ser.read(1)
            if not b:
                continue
            h = b[0]
            if h == EOT:
                self.ser.write(bytes([ACK]))
                break
            if h == CAN:
                raise StubError("stub cancelled the read")
            if h not in (STX, SOH):
                continue
            started = True
            size = 1024 if h == STX else 128
            pkt = self._read(size + 4, time.time() + 2.0)
            data = pkt[2:2 + size]
            if pkt[0] + pkt[1] != 0xFF or crc16_xmodem(data) != (pkt[-2] << 8 | pkt[-1]):
                self.ser.reset_input_buffer()
                self.ser.write(bytes([NAK]))
                continue
            if pkt[0] == expect & 0xFF:
                out += data
                expect += 1
                if progress and len(out) % 0x10000 == 0:
                    sys.stdout.write(f"\r[read] {len(out) * 100 // length:3d}%")
                    sys.stdout.flush()
            self.ser.write(bytes([ACK]))
        if progress:
            print("\r[read] 100%")
        if len(out) < length:
            raise StubError(f"read returned {len(out)} of {length} bytes")
        return bytes(out[:length])

    def write(self, addr, data, progress=True):
        self.cmd(CMD_XMODEM_WRITE, struct.pack("<II", addr, len(data)))
        deadline = time.time() + 30
        while True:
            b = self._read(1, deadline)[0]
            if b == C:
                break
            if b == CAN:
                raise StubError("stub cancelled the write")
        self.ser.reset_input_buffer()
        padded = data + b"\xFF" * (-len(data) % 1024)
        for i in range(0, len(padded), 1024):
            blk = (i // 1024 + 1) & 0xFF
            chunk = padded[i:i + 1024]
            crc = crc16_xmodem(chunk)
            pkt = bytes([STX, blk, 0xFF - blk]) + chunk + bytes([crc >> 8, crc & 0xFF])
            for _ in range(10):
                self.ser.write(pkt)
                r = self._read(1, time.time() + 15)[0]
                while r == C:
                    r = self._read(1, time.time() + 15)[0]
                if r == ACK:
                    break
                if r == CAN:
                    raise StubError(f"stub cancelled the write at {addr + i:#x} (erase/program/verify failed)")
            else:
                raise StubError(f"too many NAKs at {addr + i:#x}")
            if progress and (i + 1024) % 0x10000 == 0:
                sys.stdout.write(f"\r[write] {(i + 1024) * 100 // len(padded):3d}%")
                sys.stdout.flush()
        self.ser.write(bytes([EOT]))
        if self._read(1, time.time() + 5)[0] != ACK:
            raise StubError("no ACK for EOT")
        if progress:
            print("\r[write] 100%")

    def reboot(self):
        self.cmd(CMD_REBOOT)


def write_image(st, addr, data):
    """Program data at addr like the GUI flasher does: the protected first 64 KB
    is skipped, all-0xFF sectors are only erased, the rest goes over XMODEM."""
    if addr < 0x10000:
        skip = 0x10000 - addr
        print(f"[write] skipping {skip:#x} bytes below 0x10000 (loader + factory settings stay)")
        data, addr = data[skip:], 0x10000
    data += b"\xFF" * (-len(data) % 0x1000)
    runs = []                                    # (offset, length, is_blank)
    for o in range(0, len(data), 0x1000):
        blank = data[o:o + 0x1000] == b"\xFF" * 0x1000
        if runs and runs[-1][2] == blank:
            runs[-1][1] += 0x1000
        else:
            runs.append([o, 0x1000, blank])
    for o, n, blank in runs:
        if blank:
            print(f"[write] erase {addr + o:#08x}..{addr + o + n:#08x}")
            for c in range(o, o + n, 0x40000):     # stay well inside the 10 s command timeout
                st.erase(addr + c, min(0x40000, o + n - c))
        else:
            print(f"[write] data  {addr + o:#08x}..{addr + o + n:#08x}")
            st.write(addr + o, data[o:o + n])
    return addr, data


def start_stub(args):
    """Return a Stub talking at IDLE_BAUD."""
    ld = flash.Loader(args.port, 38400, args.verbose)
    if args.running:
        ld.ser.baudrate = IDLE_BAUD
        return Stub(ld.ser)
    if not STUB.exists():
        sys.exit(f"{STUB} missing: python tools/build.py examples/31_uart_flasher")
    for attempt in range(1, 4):
        if args.no_cycle:
            ld.log("[loader] power-cycle the router now")
        if ld.enter_prompt(not args.no_cycle, args.obk, 4.0, 600 if args.no_cycle else 25):
            break
        if attempt == 3:
            sys.exit("no <RealTek> prompt")
    ld.cmd("")
    # linked for cached KSEG0 (STUB_ENTRY); written through the uncached alias so the
    # words reach RAM and not just the D-cache, then started cached
    ld.load(STUB.read_bytes(), STUB_ENTRY | 0x20000000, 8, True)
    ld.jump(STUB_ENTRY)
    if not ld.wait_for(b"stub ready", 3.0):
        sys.exit(f"stub did not start: {ld.buf[-200:]!r}")
    st = Stub(ld.ser)
    st.set_baud(IDLE_BAUD)
    return st


def show_info(st):
    pid, clk16 = st.chip_info()
    want = zlib.crc32(b"RTL8196C")
    print(f"platform id {pid:08x} ({'RTL8196C' if pid == want else 'UNKNOWN'}), "
          f"UART clock {clk16 * 16 / 1e6:.3f} MHz")
    fid = st.flash_id()
    print(f"flash id {fid.hex()} -> {(1 << (fid[2] - 0x11)) // 8} MB")
    print("MAC " + ":".join(f"{b:02x}" for b in st.cmd(CMD_GET_MAC)))
    return clk16


def baudprobe(st, args):
    """Per rate: 100 small commands and three CRC-checked 64 KB reads, plus the stub's RX error counters."""
    clk16 = show_info(st)
    base = st.rx_errors()
    for baud in args.rates:
        div = round(clk16 / baud)
        try:
            st.set_baud(baud)
        except StubError as e:
            print(f"{baud:8d}: baud switch failed: {e}")
            st.ser.baudrate = IDLE_BAUD
            time.sleep(2.5)
        else:
            cmd_fail = 0
            for _ in range(100):
                try:
                    st.chip_info()
                except StubError:
                    cmd_fail += 1
                    time.sleep(0.05)
            read_fail = 0
            for _ in range(3):
                try:
                    if zlib.crc32(st.read(0, 0x10000, progress=False)) != st.crc32(0, 0x10000):
                        read_fail += 1
                except StubError:
                    read_fail += 1
                    time.sleep(0.5)
            print(f"{baud:8d} (div {div}, {abs(clk16 / div - baud) / baud * 100:.2f}% off): "
                  f"{cmd_fail}/100 commands failed, {read_fail}/3 reads failed")
            try:
                st.set_baud(IDLE_BAUD)
            except StubError:
                st.ser.baudrate = IDLE_BAUD
                time.sleep(2.5)                   # stub falls back to 115200 after 2 s without a good request
        time.sleep(0.2)
        try:
            err = st.rx_errors()
        except StubError:
            print("  stub lost, stopping")
            return False
        print("  stub RX errors since last rate: overrun %d, parity %d, framing %d, break %d"
              % tuple(e - b for e, b in zip(err, base)))
        base = err
    return True


def selftest(st, args):
    clk16 = show_info(st)
    ok = True

    def check(name, cond):
        nonlocal ok
        print(f"  {'ok  ' if cond else 'FAIL'} {name}")
        ok &= bool(cond)

    print("[test] protected area")
    for name, fn in (("erase 0x0 refused", lambda: st.erase(0, 0x1000)),
                     ("write 0x8000 refused", lambda: st.write(0x8000, b"\0" * 16, False))):
        try:
            fn()
            check(name, False)
        except StubError as e:
            check(name, "ADDR_ERROR" in str(e))

    backup = sorted((ROOT / "backups").glob("flash_*.bin"))
    best = IDLE_BAUD
    for baud in (115200, 230400, 250000, 400000, 614400, 768000, 1000000):    # rates measured to work, 2026-10-28
        div = round(clk16 / baud)
        err = abs(clk16 / div - baud) / baud * 100
        try:
            st.set_baud(baud)
            t0 = time.time()
            data = st.read(0, 0x10000, progress=False)
            dt = time.time() - t0
            good = zlib.crc32(data) == st.crc32(0, 0x10000)
            if backup:
                good &= data == backup[0].read_bytes()[:0x10000]
            check(f"{baud:7d} baud (divisor {div}, {err:.2f}% off): 64 KB in {dt:.2f} s "
                  f"= {0x10000 / dt / 1024:.1f} KB/s", good)
            if good:
                best = baud
        except StubError as e:
            print(f"  --   {baud} baud: {e}")
            st.ser.baudrate = IDLE_BAUD
            time.sleep(0.2)
            try:
                st.set_baud(IDLE_BAUD)
            except StubError:
                print("  stub lost at the old rate too, stopping")
                return False
            break
        if baud >= args.max_baud:
            break
    st.set_baud(best)
    print(f"[test] using {best} baud")

    area, size = args.scratch, 0x10000
    save = ROOT / "build" / f"uartflash_{area:06x}_orig.bin"
    print(f"[test] write/erase/restore at {area:#x} (original saved to {save.relative_to(ROOT)})")
    orig = st.read(area, size, progress=False)
    save.write_bytes(orig)
    pattern = os.urandom(size)
    t0 = time.time()
    st.write(area, pattern, progress=False)
    dt = time.time() - t0
    check(f"random 64 KB written in {dt:.2f} s, device CRC matches", st.crc32(area, size) == zlib.crc32(pattern))
    check("read back equals pattern", st.read(area, size, progress=False) == pattern)
    st.erase(area, 0x1000)
    check("4 KB erase leaves 0xFF", st.crc32(area, 0x1000) == zlib.crc32(b"\xFF" * 0x1000))
    st.write(area, orig, progress=False)
    check("original restored", st.crc32(area, size) == zlib.crc32(orig))

    if args.full:
        t0 = time.time()
        data = st.read(0, 0x800000)
        dt = time.time() - t0
        check(f"full 8 MB read in {dt:.1f} s, device CRC matches", zlib.crc32(data) == st.crc32(0, 0x800000))
    st.set_baud(IDLE_BAUD)
    print("[test] " + ("all passed" if ok else "FAILURES"))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["info", "read", "write", "selftest", "baudprobe", "reboot"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--port", default=bench.PORT)
    ap.add_argument("--obk", default=bench.OBK)
    ap.add_argument("--load", default=bench.LOAD)
    ap.add_argument("--baud", type=int, default=460800, help="transfer rate for read/write")
    ap.add_argument("--max-baud", type=int, default=2000000, help="selftest: highest rate to try")
    ap.add_argument("--scratch", type=lambda s: int(s, 0), default=0x7F0000, help="selftest write area (restored)")
    ap.add_argument("--full", action="store_true", help="selftest: also read all 8 MB")
    ap.add_argument("--rates", type=lambda s: [int(x) for x in s.split(",")],
                    default=[115200, 230400, 460800, 500000, 921600, 1000000], help="baudprobe: comma separated rates")
    ap.add_argument("--no-cycle", action="store_true", help="wait for a manual power cycle instead of the OBK socket")
    ap.add_argument("--running", action="store_true", help="stub is already running at 115200")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    st = start_stub(args)
    if args.action == "info":
        show_info(st)
    elif args.action == "selftest":
        sys.exit(0 if selftest(st, args) else 1)
    elif args.action == "baudprobe":
        sys.exit(0 if baudprobe(st, args) else 1)
    elif args.action == "reboot":
        st.reboot()
    elif args.action == "read":
        addr, length, out = int(args.args[0], 0), int(args.args[1], 0), args.args[2]
        st.set_baud(args.baud)
        data = st.read(addr, length)
        ok = zlib.crc32(data) == st.crc32(addr, length)
        st.set_baud(IDLE_BAUD)
        Path(out).write_bytes(data)
        print(f"{length} bytes -> {out}, CRC {'ok' if ok else 'MISMATCH'}")
        sys.exit(0 if ok else 1)
    elif args.action == "write":
        addr, data = int(args.args[0], 0), Path(args.args[1]).read_bytes()
        st.set_baud(args.baud)
        t0 = time.time()
        addr, data = write_image(st, addr, data)
        print(f"[write] {time.time() - t0:.1f} s")
        ok = zlib.crc32(data) == st.crc32(addr, len(data))
        st.set_baud(IDLE_BAUD)
        print(f"{len(data)} bytes at {addr:#x}, CRC {'ok' if ok else 'MISMATCH'}")
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
