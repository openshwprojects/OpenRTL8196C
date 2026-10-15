#!/usr/bin/env python3
"""Load a RAM image into the RTL8196C through the boot loader console and run it.

    python tools/flash.py build/01_hello/app.bin                 # power-cycle via OBK, load, run, show output
    python tools/flash.py build/01_hello/app.bin --no-cycle      # loader prompt must already be up
    python tools/flash.py --prompt-only                          # just get to the <RealTek> prompt
    python tools/flash.py build/x/app.bin --listen 30            # stop after 30 s of output

Sequence: open COM port, power-cycle the socket (Tasmota/OpenBeken HTTP API),
send ESC repeatedly until the "<RealTek>" prompt shows up, write the image
with EW (8 words per line), read it back with DW, jump with J, then relay
UART output to the terminal (Ctrl-C to quit; typed keys are forwarded).
"""
import argparse
import struct
import sys
import threading
import time
import urllib.parse
import urllib.request

import serial

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
import bench  # noqa: E402

PROMPT = b"<RealTek>"


def obk(ip, cmd, timeout=5.0):
    url = f"http://{ip}/cm?cmnd={urllib.parse.quote(cmd)}"
    try:
        return urllib.request.urlopen(url, timeout=timeout).read().decode(errors="replace")
    except Exception as e:
        return f"<{type(e).__name__}>"


class Loader:
    def __init__(self, port, baud, verbose):
        self.ser = serial.Serial(port, baud, timeout=0.02)
        self.verbose = verbose
        self.buf = b""

    def log(self, *a):
        print(*a, flush=True)

    def pump(self):
        d = self.ser.read(4096)
        if d:
            self.buf += d
            if self.verbose:
                sys.stdout.write(d.decode("ascii", "replace"))
                sys.stdout.flush()
        return d

    def wait_for(self, token, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            self.pump()
            if token in self.buf:
                return True
        return False

    def enter_prompt(self, cycle, obk_ip, off_time, timeout):
        if cycle:
            self.log(f"[obk] power off -> {obk(obk_ip, 'Power OFF')}")
            time.sleep(off_time)
            self.ser.reset_input_buffer()
            self.buf = b""
            self.log(f"[obk] power on  -> {obk(obk_ip, 'Power ON')}")
        self.log("[loader] sending ESC until prompt")
        t0 = time.time()
        n = 0
        while time.time() - t0 < timeout:
            self.ser.write(b"\x1b")
            n += 1
            self.pump()
            if PROMPT in self.buf:
                self.log(f"[loader] prompt after {n} ESC, {time.time()-t0:.1f}s")
                time.sleep(0.2)
                self.pump()
                return True
            if b"Jump to image" in self.buf or b"decompressing" in self.buf:
                self.log("[loader] missed the window: firmware is booting")
                return False
        self.log("[loader] no prompt (check TX wiring / baud)")
        if not self.verbose:
            self.log("[loader] received: " + repr(self.buf[-300:]))
        return False

    def cmd(self, line, timeout=3.0):
        """Send a command, return the text printed before the next prompt."""
        self.buf = b""
        self.ser.write(line.encode() + b"\r")
        self.ser.flush()
        if not self.wait_for(PROMPT, timeout):
            raise RuntimeError(f"no prompt after {line!r}: {self.buf[-200:]!r}")
        resp = self.buf
        self.buf = b""
        return resp.decode("ascii", "replace")

    def dump_words(self, addr, nwords):
        txt = self.cmd(f"DW {addr:08X} {nwords}", timeout=5 + nwords / 50)
        out = {}
        for line in txt.splitlines():
            parts = line.replace("\t", " ").split()
            if len(parts) >= 2 and parts[0].endswith(":"):
                try:
                    a = int(parts[0][:-1], 16)
                except ValueError:
                    continue
                for i, w in enumerate(parts[1:]):
                    try:
                        out[a + 4 * i] = int(w, 16)
                    except ValueError:
                        pass
        return out

    def load(self, data, base, per_line, verify):
        pad = (-len(data)) % 4
        data += b"\0" * pad
        words = struct.unpack(f">{len(data)//4}I", data)
        total = len(words)
        self.log(f"[load] {len(data)} bytes = {total} words at {base:#010x}")
        t0 = time.time()
        for i in range(0, total, per_line):
            chunk = words[i:i + per_line]
            addr = base + 4 * i
            self.cmd(f"EW {addr:08X} " + " ".join(f"{w:08X}" for w in chunk))
            if (i // per_line) % 16 == 0:
                pct = 100 * (i + len(chunk)) // total
                sys.stdout.write(f"\r[load] {pct:3d}%")
                sys.stdout.flush()
        self.log(f"\r[load] 100% in {time.time()-t0:.1f}s")
        if verify:
            self.log("[verify] reading back")
            bad = 0
            step = 64
            for i in range(0, total, step):
                n = min(step, total - i)
                got = self.dump_words(base + 4 * i, n)
                for k in range(n):
                    a = base + 4 * (i + k)
                    if got.get(a) != words[i + k]:
                        bad += 1
                        if bad <= 8:
                            self.log(f"[verify] mismatch at {a:08X}: wrote {words[i+k]:08X} read {got.get(a)}")
            if bad:
                raise RuntimeError(f"{bad} words differ")
            self.log("[verify] ok")

    def jump(self, base):
        self.log(f"[run] J {base:08X}")
        self.buf = b""
        self.ser.write(f"J {base:08X}\r".encode())
        self.ser.flush()

    def relay(self, seconds, logfile, timestamps=False, sends=()):
        """sends: list of (delay_seconds, bytes) to inject into the UART while relaying."""
        self.log(f"[relay] UART <-> terminal ({'Ctrl-C to stop' if not seconds else f'{seconds}s'})")
        stop = threading.Event()
        at_line_start = True
        pending = sorted(sends)

        def keyboard():
            try:
                import msvcrt
                while not stop.is_set():
                    if msvcrt.kbhit():
                        ch = msvcrt.getwch()
                        self.ser.write(ch.encode("latin1", "replace"))
                    else:
                        time.sleep(0.01)
            except ImportError:
                pass

        threading.Thread(target=keyboard, daemon=True).start()
        t0 = time.time()
        log = open(logfile, "ab") if logfile else None
        try:
            while not seconds or time.time() - t0 < seconds:
                while pending and time.time() - t0 >= pending[0][0]:
                    _, payload = pending.pop(0)
                    self.ser.write(payload)
                    self.ser.flush()
                d = self.ser.read(4096)
                if d:
                    if log:
                        log.write(d)
                        log.flush()
                    text = d.decode("ascii", "replace").replace("\r\n", "\n").replace("\r", "\n")
                    if timestamps:
                        out = []
                        for ch in text:
                            if at_line_start and ch != "\n":
                                out.append(f"[{time.time()-t0:8.3f}] ")
                            out.append(ch)
                            at_line_start = ch == "\n"
                        text = "".join(out)
                    sys.stdout.write(text)
                    sys.stdout.flush()
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()
            if log:
                log.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", nargs="?", help="raw .bin produced by tools/build.py")
    ap.add_argument("--port", default=bench.PORT)
    ap.add_argument("--baud", type=int, default=bench.BAUD)
    ap.add_argument("--load", default=bench.LOAD)
    ap.add_argument("--obk", default=bench.OBK, help="Tasmota/OpenBeken socket IP (bench.ini)")
    ap.add_argument("--no-cycle", action="store_true", help="do not power-cycle, expect the prompt")
    ap.add_argument("--off-time", type=float, default=4.0)
    ap.add_argument("--retries", type=int, default=3, help="power-cycle attempts to reach the prompt")
    ap.add_argument("--esc-timeout", type=float, default=25.0)
    ap.add_argument("--words-per-line", type=int, default=8)
    ap.add_argument("--no-verify", action="store_true")
    ap.add_argument("--no-run", action="store_true")
    ap.add_argument("--prompt-only", action="store_true")
    ap.add_argument("--listen", type=float, default=0, help="seconds of output to relay, 0 = until Ctrl-C")
    ap.add_argument("--log", default="", help="append raw UART output to this file")
    ap.add_argument("-t", "--timestamps", action="store_true", help="prefix relayed lines with seconds since jump")
    ap.add_argument("--send", action="append", default=[], metavar="DELAY:TEXT",
                    help="send TEXT (python escapes allowed, e.g. 'hello\\r') DELAY seconds after the jump; repeatable")
    ap.add_argument("-v", "--verbose", action="store_true", help="show loader traffic")
    args = ap.parse_args()

    base = int(args.load, 16)
    sends = []
    for item in args.send:
        delay, _, text = item.partition(":")
        sends.append((float(delay), text.encode("latin1").decode("unicode_escape").encode("latin1")))
    ld = Loader(args.port, args.baud, args.verbose)
    for attempt in range(1, args.retries + 1):
        if ld.enter_prompt(not args.no_cycle, args.obk, args.off_time, args.esc_timeout):
            break
        if args.no_cycle or attempt == args.retries:
            sys.exit(1)
        ld.log(f"[loader] retry {attempt + 1}/{args.retries}")
    ld.cmd("")                                   # sync on a fresh prompt
    if args.prompt_only or not args.image:
        ld.log("[loader] at prompt")
        ld.relay(args.listen, args.log, args.timestamps, sends)
        return
    data = open(args.image, "rb").read()
    ld.load(data, base, args.words_per_line, not args.no_verify)
    if args.no_run:
        ld.relay(args.listen, args.log, args.timestamps, sends)
        return
    ld.jump(base)
    ld.relay(args.listen, args.log, args.timestamps, sends)


if __name__ == "__main__":
    main()
