#!/usr/bin/env python3
"""Hardware regression test: load every example and check its behaviour.

    python tools/test_examples.py              # all
    python tools/test_examples.py 05 07        # by prefix

Needs the bench from flash.py (COM port + OpenBeken socket).  Each test
power-cycles the board, loads the image, runs it for a while and asserts on
the UART output (timestamps come from the PC clock).
"""
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flash  # noqa: E402
import bench  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PORT, BAUD, OBK = bench.PORT, bench.BAUD, bench.OBK
BOARD_IP = bench.BOARD_IP
LOAD = int(bench.LOAD, 16)


class Run:
    """Capture (timestamp, line) pairs from the board while optionally sending input."""

    def __init__(self, name):
        self.name = name
        self.lines = []       # (t, text)
        self.raw = b""

    def execute(self, seconds, sends=()):
        ld = flash.Loader(PORT, BAUD, verbose=False)
        ok = False
        for attempt in range(3):
            if ld.enter_prompt(True, OBK, 4.0, 25.0):
                ok = True
                break
        if not ok:
            raise RuntimeError("loader prompt not reached")
        ld.cmd("")
        img = ROOT / "build" / self.name / "app.bin"
        ld.load(img.read_bytes(), LOAD, 8, verify=True)
        ld.jump(LOAD)
        t0 = time.time()
        pending = sorted(sends)
        buf = b""
        while time.time() - t0 < seconds:
            while pending and time.time() - t0 >= pending[0][0]:
                ld.ser.write(pending.pop(0)[1])
                ld.ser.flush()
            d = ld.ser.read(4096)
            if d:
                self.raw += d
                buf += d
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.replace(b"\r", b"").decode("ascii", "replace")
                    self.lines.append((time.time() - t0, text))
        ld.ser.close()
        return self

    def grep(self, pattern):
        rx = re.compile(pattern)
        return [(t, rx.search(s)) for t, s in self.lines if rx.search(s)]


def check(cond, msg, failures):
    if not cond:
        failures.append(msg)


def spacing_ok(times, expected, tol):
    gaps = [b - a for a, b in zip(times, times[1:])]
    bad = [g for g in gaps if abs(g - expected) > tol]
    return gaps, bad


def test_01_hello():
    r = Run("01_hello").execute(4)
    f = []
    check(r.grep(r"PRId\s+= 0000cd01"), "PRId not cd01", f)
    check(r.grep(r"^done$"), "no done", f)
    check(not r.grep(r"\*\*\* exception"), "exception reported", f)
    return f


def test_02_counter():
    r = Run("02_counter").execute(30)
    f = []
    nums = [int(m.group(1)) for _, m in r.grep(r"^(\d+)$")]
    check(len(nums) >= 25, f"only {len(nums)} numbers in 30 s", f)
    check(nums == list(range(1, len(nums) + 1)), f"not consecutive: {nums[:40]}", f)
    return f


def test_03_blink():
    r = Run("03_blink").execute(10)
    f = []
    steps = [(int(m.group(1)), int(m.group(2))) for _, m in r.grep(r"^step (\d+) led (\d+)$")]
    check(len(steps) >= 15, f"only {len(steps)} steps", f)
    check([s for s, _ in steps] == list(range(len(steps))), "steps not consecutive", f)
    check(all(l == s % 7 for s, l in steps), "led != step % 7", f)
    return f


def test_04_timer_poll():
    r = Run("04_timer_poll").execute(22)
    f = []
    hits = r.grep(r"^tick (\d+)\s+tc0=(\d+) tc1=(\d+) dtc1=(\d+)$")
    ticks = [int(m.group(1)) for _, m in hits]
    check(len(ticks) >= 18, f"only {len(ticks)} ticks in 22 s", f)
    check(ticks == list(range(1, len(ticks) + 1)), "ticks not consecutive", f)
    dt = [int(m.group(4)) for _, m in hits[1:]]
    check(all(abs(d - 1000000) <= 2000 for d in dt), f"dtc1 off: {dt[:10]}", f)
    gaps, bad = spacing_ok([t for t, _ in hits], 1.0, 0.06)
    check(not bad, f"PC spacing off: {['%.3f' % g for g in bad[:10]]}", f)
    return f


def test_05_timer_irq():
    r = Run("05_timer_irq").execute(32)
    f = []
    hits = r.grep(r"^tick (\d+)\s+irqs=(\d+) cause=([0-9a-f]{8}) gisr=([0-9a-f]{8})$")
    ticks = [int(m.group(1)) for _, m in hits]
    check(len(ticks) >= 28, f"only {len(ticks)} ticks in 32 s", f)
    check(ticks == list(range(1, len(ticks) + 1)), "ticks not consecutive", f)
    check(all(int(m.group(2)) == int(m.group(1)) for _, m in hits), "irq count != tick", f)
    check(all(int(m.group(3), 16) & 0x400 for _, m in hits), "Cause.IP2 not set", f)
    gaps, bad = spacing_ok([t for t, _ in hits], 1.0, 0.06)
    check(not bad, f"PC spacing off: {['%.3f' % g for g in bad[:10]]}", f)
    check(not r.grep(r"\*\*\* exception"), "exception reported", f)
    return f


def test_06_uart_echo():
    long = "L" * 70                              # longer than the 63-byte buffer
    sends = [(1.0, b"hello board\r"), (2.5, long.encode() + b"\r"),
             (4.0, b"x\x01y\r"), (5.5, b"reboot\r")]
    r = Run("06_uart_echo").execute(9, sends)
    f = []
    check(r.grep(r'^line \(11 bytes\): "hello board"$'), "line 1 not echoed", f)
    check(r.grep(r'^line \(63 bytes\): "L{63}"$'), "long line not truncated to 63", f)
    check(r.grep(r'^line \(3 bytes\): "x\x01y"$') or r.grep(r'^line \(3 bytes\): "x.y"$'), "binary line wrong", f)
    check(r.grep(r"\[01\]"), "control byte hex not shown", f)
    check(r.grep(r"Reboot Result from Watchdog"), "watchdog reboot not seen", f)
    return f


def test_07_uart_irq():
    burst = bytes(range(0x20, 0x7F)) * 2        # 190 bytes in one go
    sends = [(1.2, b"abc\r\n"), (2.5, burst), (4.5, b"tail\r\n")]
    r = Run("07_uart_irq").execute(8, sends)
    f = []
    check(b"abc" in r.raw, "abc not echoed", f)
    check(burst in r.raw, "190-byte burst not echoed intact", f)
    check(b"tail" in r.raw, "tail not echoed", f)
    # tick markers may land inside an echoed line, so scan the raw stream
    hits = list(re.finditer(rb"\[tick (\d+) uart_irqs=(\d+)\]", r.raw))
    ticks = [int(m.group(1)) for m in hits]
    check(ticks == list(range(1, len(ticks) + 1)) and len(ticks) >= 6, f"ticks wrong: {ticks}", f)
    final = int(hits[-1].group(2)) if hits else -1
    sent = sum(len(p) for _, p in sends)
    check(final == sent, f"uart_irqs={final}, bytes sent={sent}", f)
    return f


def test_08_eth_link():
    r = Run("08_eth_link").execute(8)
    f = []
    links = r.grep(r"^link:(.*)rx=(\d+)")
    check(len(links) >= 6, f"only {len(links)} link lines", f)
    check(any("UP/100FD" in m.group(1) or "UP/10" in m.group(1) for _, m in links),
          "no port reports link up (is a LAN cable plugged in?)", f)
    return f


def test_09_eth_ping():
    import subprocess
    r = Run("09_eth_ping")
    # ping from the PC in the background once the board is up (~25 s after start)
    cmd = ["powershell", "-NoProfile", "-Command",
           "Start-Sleep 3; ping -n 8 -w 800 " + BOARD_IP + " | Select-String 'Reply|Lost'"]
    proc = None
    orig_execute = r.execute

    def execute_with_ping(seconds, sends=()):
        nonlocal proc
        # start the ping right after the jump: hook by wrapping Loader.jump
        real_jump = flash.Loader.jump

        def jump_and_ping(self, base):
            nonlocal proc
            real_jump(self, base)
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)
        flash.Loader.jump = jump_and_ping
        try:
            return orig_execute(seconds, sends)
        finally:
            flash.Loader.jump = real_jump

    execute_with_ping(16)
    f = []
    out = proc.communicate(timeout=30)[0] if proc else ""
    replies = out.count("Reply from " + BOARD_IP)
    check(replies >= 4, f"only {replies} ping replies on the PC side:\n{out}", f)
    stats = r.grep(r"pings=(\d+)")
    check(stats and int(stats[-1][1].group(1)) >= 4, "board counted <4 pings", f)
    answers = r.grep(r"arp_answers=(\d+)")
    check(answers and int(answers[-1][1].group(1)) >= 1, "board's own ARP probe got no answer", f)
    return f


def test_11_udp_console():
    import socket
    r = Run("11_udp_console").execute(6)       # board keeps running after the relay ends
    f = []
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)

    def ask(cmd, port=9696):
        sock.sendto(cmd if isinstance(cmd, bytes) else cmd.encode(), (BOARD_IP, port))
        try:
            return sock.recvfrom(4096)[0]
        except socket.timeout:
            return None

    reply = None
    for _ in range(5):                          # give ARP a moment
        reply = ask("hi")
        if reply:
            break
        time.sleep(1)
    check(reply and b"OpenRTL8196C console" in reply, f"no/bad 'hi' reply: {reply!r}", f)
    check((ask("peek b8003100 2") or b"").startswith(b"b8003100:"), "peek failed", f)
    check(b"deadbeef" in (ask("poke a0400000 deadbeef") or b""), "poke failed", f)
    check(b"port 3: up" in (ask("link") or b""), "link report missing port 3 up", f)
    payload = bytes(range(256)) * 4
    ok = 0
    for i in range(20):
        if ask(payload, 7) == payload:
            ok += 1
    check(ok == 20, f"echo: only {ok}/20 datagrams came back intact", f)
    sock.close()
    return f


def test_12_http_server():
    import json
    import urllib.request
    import urllib.error
    Run("12_http_server").execute(6)            # board keeps running after the relay ends
    f = []
    base = "http://" + BOARD_IP

    def get(path, tries=5):
        for _ in range(tries):
            try:
                with urllib.request.urlopen(base + path, timeout=3) as resp:
                    return resp.status, resp.read()
            except urllib.error.HTTPError as e:
                return e.code, e.read()
            except Exception:
                time.sleep(1)
        return None, b""

    code, body = get("/")
    check(code == 200 and b"<title>OpenRTL8196C</title>" in body, f"GET / -> {code}", f)
    code, body = get("/api/status")
    try:
        js = json.loads(body)
        check(js["ports"][3]["link"] is True, "JSON says port 3 down", f)
    except Exception as e:  # noqa: BLE001
        check(False, f"bad JSON: {e}: {body[:80]!r}", f)
    code, body = get("/regs?addr=b8003100&n=4")
    check(code == 200 and body.startswith(b"b8003100:"), "regs dump wrong", f)
    code, body = get("/nothing")
    check(code == 404, f"404 expected, got {code}", f)
    # 20 back-to-back fetches must all succeed
    ok = sum(1 for _ in range(20) if get("/api/status", tries=1)[0] == 200)
    check(ok == 20, f"only {ok}/20 fetches succeeded", f)
    return f


def test_13_multi_socket():
    import socket
    import urllib.request
    import netcon
    Run("13_multi_socket").execute(6)
    f = []
    time.sleep(1)
    # 1. four TCP console connections open at the same time, each answering
    cons = []
    try:
        for i in range(4):
            cons.append(netcon.TcpConsole(BOARD_IP))
        for i, c in enumerate(cons):
            reply, _ = c.ask("hi")
            check("OpenRTL8196C console" in reply, f"tcp console #{i} bad reply: {reply!r}", f)
        reply, _ = cons[0].ask("peek b8003100 2")
        check(reply.startswith("b8003100:"), f"tcp console peek: {reply!r}", f)
    except Exception as e:  # noqa: BLE001
        check(False, f"tcp console failed: {e}", f)
    # 2. HTTP while the consoles are still open
    try:
        with urllib.request.urlopen("http://" + BOARD_IP + "/api/status", timeout=3) as r:
            check(r.status == 200 and b'"ports"' in r.read(), "http /api/status bad", f)
        with urllib.request.urlopen("http://" + BOARD_IP + "/", timeout=3) as r:
            check(b"<title>OpenRTL8196C</title>" in r.read(), "http / bad", f)
    except Exception as e:  # noqa: BLE001
        check(False, f"http failed with consoles open: {e}", f)
    # 3. UDP console while TCP consoles are open
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    data, _ = netcon.udp_ask(sock, (BOARD_IP, 9696), "uptime")
    check(data is not None and data.startswith(b"uptime"), f"udp console: {data!r}", f)
    sock.close()
    for c in cons:
        c.close()
    # 4. TCP echo of 2048-byte blocks (multi-segment) and UDP echo
    check(netcon.tcp_echo(BOARD_IP, 10), "tcp echo failed", f)
    check(netcon.udp_echo(BOARD_IP, 10), "udp echo failed", f)
    # 5. 20 sequential HTTP fetches afterwards: connections were released
    ok = 0
    for _ in range(20):
        try:
            with urllib.request.urlopen("http://" + BOARD_IP + "/api/status", timeout=3) as r:
                ok += r.status == 200
        except Exception:  # noqa: BLE001
            pass
    check(ok == 20, f"only {ok}/20 http fetches after the console session", f)
    return f


def test_15_ota():
    import socket
    import netcon
    import ota
    import mkimage
    r = Run("15_ota").execute(7)
    f = []
    check(r.grep(r"15_ota.*mac=80:1f:02:ca:ad:92 \(H601\)"), "factory MAC from H601 not reported", f)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    fl_rep, _ = netcon.udp_ask(sock, (BOARD_IP, 9696), "flash")
    check(fl_rep and b"factory_mac 80:1f:02:ca:ad:92 (H601)" in fl_rep, f"udp flash cmd bad: {fl_rep!r}", f)
    ip_rep, _ = netcon.udp_ask(sock, (BOARD_IP, 9696), "ip")
    check(ip_rep and b"ip " in ip_rep, f"udp ip cmd bad: {ip_rep!r}", f)

    # 1. OTA status over TCP 9698
    st = ota.send_ota(BOARD_IP, ota.OTA_PORT, ota.CMD_STATUS, b"")
    check(st.startswith("OK status ptable="), f"OTA status failed: {st!r}", f)

    # 2. Safe flash OTA test in spare top sector (0x7E0000): erase, write RAPP (page 0 last), verify, restore zeros
    raw_15 = (ROOT / "build" / "15_ota" / "app.bin").read_bytes()
    rapp_15 = mkimage.make_rapp(raw_15, LOAD, LOAD, seq=7, version="15_ota_test")
    tf = ota.send_ota(BOARD_IP, ota.OTA_PORT, ota.CMD_TEST_FLASH, rapp_15, target_addr=0x7E0000, timeout=60.0)
    check(tf.startswith("OK mode=test_flash") and "valid=1 restored=1" in tf, f"OTA test_flash failed: {tf!r}", f)

    # 3. RAM OTA jump: push 13_multi_socket over Ethernet into RAM and jump to it without UART or flash
    raw_13 = (ROOT / "build" / "13_multi_socket" / "app.bin").read_bytes()
    rapp_13 = mkimage.make_rapp(raw_13, LOAD, LOAD, seq=1, version="13_from_ota")
    rm = ota.send_ota(BOARD_IP, ota.OTA_PORT, ota.CMD_RAM, rapp_13, target_addr=LOAD, timeout=15.0)
    check(rm.startswith("OK mode=ram"), f"OTA ram jump failed: {rm!r}", f)

    time.sleep(1.5)
    # Verify the newly booted image in RAM responds (and "tag" from 15_ota is gone -> "unknown command 'tag'")
    tag_rep, _ = netcon.udp_ask(sock, (BOARD_IP, 9696), "tag")
    check(tag_rep and b"unknown command 'tag'" in tag_rep, f"RAM-booted 13_multi_socket did not replace 15_ota: {tag_rep!r}", f)
    sock.close()
    return f


TESTS = [test_01_hello, test_02_counter, test_03_blink, test_04_timer_poll,
         test_05_timer_irq, test_06_uart_echo, test_07_uart_irq,
         test_08_eth_link, test_09_eth_ping, test_11_udp_console, test_12_http_server,
         test_13_multi_socket, test_15_ota]


def main():
    sel = sys.argv[1:]
    results = []
    for t in TESTS:
        name = t.__name__[5:]
        if sel and not any(name.startswith(s) for s in sel):
            continue
        print(f"=== {name}", flush=True)
        try:
            failures = t()
        except Exception as e:  # noqa: BLE001
            failures = [f"crashed: {e}"]
        status = "PASS" if not failures else "FAIL"
        print(f"=== {name}: {status}" + ("".join("\n    - " + m for m in failures)), flush=True)
        results.append((name, status, failures))
    print("\nsummary:")
    for name, status, _ in results:
        print(f"  {name:16s} {status}")
    sys.exit(0 if all(s == "PASS" for _, s, _ in results) else 1)


if __name__ == "__main__":
    main()
