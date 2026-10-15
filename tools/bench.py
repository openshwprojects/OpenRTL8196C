"""Bench settings shared by the tools: bench.ini in the repo root, overridable by BENCH_* env vars.

[bench]
port = COM4
baud = 38400
obk = 192.168.0.240
board_ip = 192.168.0.196
load = 0xA0500000
"""
import configparser
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
_DEFAULTS = {
    "port": "COM4",
    "baud": "38400",
    "obk": "192.168.0.240",
    "board_ip": "192.168.0.196",
    "load": "0xA0500000",
}

_cfg = configparser.ConfigParser()
_cfg.read(ROOT / "bench.ini")


def get(key):
    env = os.environ.get("BENCH_" + key.upper())
    if env:
        return env
    if _cfg.has_option("bench", key):
        return _cfg.get("bench", key)
    return _DEFAULTS[key]


PORT = get("port")
BAUD = int(get("baud"))
OBK = get("obk")
BOARD_IP = get("board_ip")
LOAD = get("load")
