#!/usr/bin/env python3
"""Back-to-back HTTP fetches against the board, with the failure mode of each.

    python tools/http_stress.py                 # 20 x GET /api/status on bench.ini's board_ip
    python tools/http_stress.py -n 50 /         # 50 x GET /
    python tools/http_stress.py --gap 0.2       # pause between requests

For every request: connect time, time to first byte, total time, bytes,
and what went wrong (refused, reset, timeout during connect/send/receive,
truncated body versus Content-Length).
"""
import argparse
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402


def fetch(ip, path, timeout):
    r = {"phase": "connect", "err": None, "connect_ms": None, "first_ms": None, "total_ms": None,
         "bytes": 0, "status": None, "clen": None, "local_port": None}
    t0 = time.perf_counter()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    data = b""
    try:
        s.connect((ip, 80))
        r["local_port"] = s.getsockname()[1]
        r["connect_ms"] = (time.perf_counter() - t0) * 1000
        r["phase"] = "send"
        s.sendall(f"GET {path} HTTP/1.1\r\nHost: {ip}\r\nConnection: close\r\n\r\n".encode())
        r["phase"] = "receive"
        while True:
            d = s.recv(65536)
            if not d:
                break
            if r["first_ms"] is None:
                r["first_ms"] = (time.perf_counter() - t0) * 1000
            data += d
        r["phase"] = "done"
    except socket.timeout:
        r["err"] = f"timeout during {r['phase']}"
    except ConnectionRefusedError:
        r["err"] = "refused (RST to SYN)"
    except ConnectionResetError:
        r["err"] = f"reset during {r['phase']}"
    except OSError as e:
        r["err"] = f"{type(e).__name__} during {r['phase']}: {e}"
    finally:
        s.close()
    r["total_ms"] = (time.perf_counter() - t0) * 1000
    r["bytes"] = len(data)
    head, _, body = data.partition(b"\r\n\r\n")
    if head.startswith(b"HTTP/1."):
        r["status"] = int(head.split()[1])
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                r["clen"] = int(line.split(b":")[1])
        if r["clen"] is not None and len(body) != r["clen"] and not r["err"]:
            r["err"] = f"truncated body {len(body)}/{r['clen']}"
    elif not r["err"]:
        r["err"] = f"no HTTP response ({len(data)} bytes)"
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", nargs="?", default="/api/status")
    ap.add_argument("-n", type=int, default=20)
    ap.add_argument("--ip", default=bench.BOARD_IP)
    ap.add_argument("--timeout", type=float, default=3.0)
    ap.add_argument("--gap", type=float, default=0.0, help="seconds between requests")
    args = ap.parse_args()

    bad = 0
    for i in range(args.n):
        r = fetch(args.ip, args.path, args.timeout)
        ok = r["err"] is None and r["status"] == 200
        bad += not ok
        fmt = lambda v: "-" if v is None else f"{v:7.1f}"
        print(f"{i + 1:3d} port {r['local_port'] or '-':>5}  connect {fmt(r['connect_ms'])} ms  first byte "
              f"{fmt(r['first_ms'])} ms  total {fmt(r['total_ms'])} ms  {r['bytes']:5d} B  "
              f"{'OK' if ok else 'FAIL: ' + str(r['err'])}", flush=True)
        if args.gap:
            time.sleep(args.gap)
    print(f"{args.n - bad}/{args.n} ok")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
