#!/usr/bin/env python3
"""Talk to the board's consoles over the LAN (IP from bench.ini unless given).

    python tools/netcon.py hi                      # UDP console (port 9696)
    python tools/netcon.py "peek b8003100 8"
    python tools/netcon.py                         # interactive UDP console
    python tools/netcon.py --tcp hi                # TCP line console (port 23)
    python tools/netcon.py --tcp                   # interactive TCP console
    python tools/netcon.py --echo 20               # UDP echo round trips on port 7
    python tools/netcon.py --tcp-echo 20           # TCP echo on port 7
    python tools/netcon.py 192.168.0.50 hi         # explicit IP first
"""
import re
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402

UDP_PORT, TCP_PORT = 9696, 23


def udp_ask(sock, addr, cmd, timeout=1.0):
    sock.settimeout(timeout)
    t0 = time.time()
    sock.sendto(cmd.encode() if isinstance(cmd, str) else cmd, addr)
    try:
        data, _ = sock.recvfrom(4096)
    except socket.timeout:
        return None, time.time() - t0
    return data, time.time() - t0


class TcpConsole:
    def __init__(self, ip, timeout=2.0):
        self.s = socket.create_connection((ip, TCP_PORT), timeout=timeout)
        self.s.settimeout(timeout)
        self.read_until_prompt()        # banner

    def read_until_prompt(self):
        buf = b""
        while not buf.endswith(b"> "):
            d = self.s.recv(4096)
            if not d:
                break
            buf += d
        return buf[:-2].decode(errors="replace") if buf.endswith(b"> ") else buf.decode(errors="replace")

    def ask(self, cmd):
        t0 = time.time()
        self.s.sendall(cmd.encode() + b"\n")
        return self.read_until_prompt(), time.time() - t0

    def close(self):
        try:
            self.s.sendall(b"quit\n")
        except OSError:
            pass
        self.s.close()


def udp_echo(ip, n):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ok, times = 0, []
    for i in range(n):
        payload = f"echo {i} ".encode() + bytes(range(256)) * 2
        data, dt = udp_ask(sock, (ip, 7), payload)
        if data is None:
            print(f"{i}: timeout")
        else:
            times.append(dt)
            ok += data == payload
    if times:
        print(f"udp echo: {ok}/{n} ok, {len(payload)} bytes, rtt min {min(times)*1000:.2f} avg {sum(times)/len(times)*1000:.2f} max {max(times)*1000:.2f} ms")
    return ok == n


def tcp_echo(ip, n):
    s = socket.create_connection((ip, 7), timeout=2.0)
    ok, times = 0, []
    payload = bytes(range(256)) * 8          # 2048 bytes, more than one segment
    for i in range(n):
        t0 = time.time()
        s.sendall(payload)
        got = b""
        while len(got) < len(payload):
            d = s.recv(4096)
            if not d:
                break
            got += d
        times.append(time.time() - t0)
        ok += got == payload
    s.close()
    print(f"tcp echo: {ok}/{n} ok, {len(payload)} bytes, rtt min {min(times)*1000:.2f} avg {sum(times)/len(times)*1000:.2f} max {max(times)*1000:.2f} ms")
    return ok == n


def main():
    args = sys.argv[1:]
    ip = bench.BOARD_IP
    if args and re.match(r"^\d+\.\d+\.\d+\.\d+$", args[0]):
        ip = args.pop(0)
    if args and args[0] == "--echo":
        sys.exit(0 if udp_echo(ip, int(args[1]) if len(args) > 1 else 10) else 1)
    if args and args[0] == "--tcp-echo":
        sys.exit(0 if tcp_echo(ip, int(args[1]) if len(args) > 1 else 10) else 1)
    if args and args[0] == "--tcp":
        args.pop(0)
        con = TcpConsole(ip)
        if args:
            reply, dt = con.ask(" ".join(args))
            print(reply.rstrip() + f"\n({dt*1000:.1f} ms)")
        else:
            print(f"tcp console {ip}:{TCP_PORT} (help, quit)")
            while True:
                try:
                    line = input("> ")
                except (EOFError, KeyboardInterrupt):
                    break
                if line.strip():
                    reply, dt = con.ask(line)
                    print(reply.rstrip(), f"({dt*1000:.1f} ms)")
        con.close()
        return
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if args:
        reply, dt = udp_ask(sock, (ip, UDP_PORT), " ".join(args))
        if reply is None:
            print("no reply")
            sys.exit(1)
        print(reply.decode(errors="replace").rstrip() + f"\n({dt*1000:.1f} ms)")
        return
    print(f"udp console {ip}:{UDP_PORT} (help), Ctrl-C to quit")
    while True:
        try:
            line = input("> ")
        except (EOFError, KeyboardInterrupt):
            break
        if line.strip():
            reply, dt = udp_ask(sock, (ip, UDP_PORT), line)
            print(reply.decode(errors="replace").rstrip() if reply else "no reply", f"({dt*1000:.1f} ms)")


if __name__ == "__main__":
    main()
