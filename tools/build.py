#!/usr/bin/env python3
"""Build one or all examples into raw RAM images for the RTL8196C.

    python tools/build.py examples/01_hello            # -> build/01_hello/app.{elf,bin,ew,lst,map}
    python tools/build.py --all                        # build all examples in examples/*
    python tools/build.py examples/01_hello --load 0x80500000 --march mips2

Toolchain: GNU mips-mti-elf (Codescape 2019.09-03, GCC 7.4) unpacked into
toolchain/mips-mti-elf/.  Set MIPS_GCC_BIN to point elsewhere.  GNU as pads
the R3000 load-delay slot for -march=mips1, which the Lexra RLX4181 may need.
"""
import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT / "sdk"
PREFIX = "mips-mti-elf-"

FORBIDDEN_INSNS = ("\tmul\t", "\tmovn\t", "\tmovz\t", "\tclz\t", "\tins\t", "\text\t", "\tteq\t", "\ttne\t")
UNALIGNED_INSNS = ("\tlwl\t", "\tlwr\t", "\tswl\t", "\tswr\t")


def find_bin() -> Path:
    cand = [os.environ.get("MIPS_GCC_BIN")]
    cand += [str(p) for p in sorted((ROOT / "toolchain" / "mips-mti-elf").glob("*/bin"))]
    for d in cand:
        if d and (Path(d, PREFIX + "gcc.exe").exists() or Path(d, PREFIX + "gcc").exists()):
            return Path(d)
    sys.exit("mips-mti-elf-gcc not found: unpack the Codescape toolchain into toolchain/ "
             "or set MIPS_GCC_BIN")


def arch_flags(march: str = "mips1") -> list[str]:
    return [
        f"-march={march}", "-mabi=32", "-msoft-float", "-EB", "-G0",
        "-mno-abicalls", "-fno-pic", "-mno-check-zero-division", "-mno-branch-likely",
    ]


def common_cflags(march: str = "mips1", opt: str = "2") -> list[str]:
    return arch_flags(march) + [
        f"-O{opt}", "-std=gnu11", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fomit-frame-pointer", "-fno-common",
        "-ffunction-sections", "-fdata-sections",
        "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
    ]


def as_flags(march: str = "mips1") -> list[str]:
    return arch_flags(march) + [f"-I{SDK / 'include'}", "-Wa,--fatal-warnings"]


def scan_disassembly(dis_text: str) -> tuple[list[str], list[str]]:
    forbidden = []
    unaligned = []
    for line in dis_text.splitlines():
        if any(bad in line for bad in FORBIDDEN_INSNS):
            forbidden.append(line)
        if any(u in line for u in UNALIGNED_INSNS):
            unaligned.append(line)
    return forbidden, unaligned


def run(cmd, verbose):
    if verbose:
        print(" ".join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.stdout:
        print(r.stdout, end="")
    if r.returncode:
        print(r.stderr, file=sys.stderr)
        sys.exit(f"command failed ({r.returncode}): {Path(str(cmd[0])).name}")
    elif r.stderr:
        print(r.stderr, end="", file=sys.stderr)
    return r


def build_example(ex: Path, args, binp: Path):
    gcc = binp / (PREFIX + "gcc")
    objcopy = binp / (PREFIX + "objcopy")
    objdump = binp / (PREFIX + "objdump")
    nm = binp / (PREFIX + "nm")

    ex = ex.resolve()
    if not (ex / "main.c").exists():
        sys.exit(f"no main.c in {ex}")
    out = ROOT / "build" / ex.name
    out.mkdir(parents=True, exist_ok=True)

    arch = arch_flags(args.march)
    cflags = common_cflags(args.march, args.O) + [
        "-Wall", "-Wextra", "-Werror=implicit-function-declaration",
        f"-I{SDK / 'include'}", f"-I{ex}",
    ]
    asflags = as_flags(args.march)

    # third-party libraries live unmodified in sdk/third_party/<name>/ and are
    # compiled with their own include path and without our warning flags
    third = sorted((SDK / "third_party").glob("*/*.c")) if (SDK / "third_party").is_dir() else []
    third_inc = [f"-I{d}" for d in sorted((SDK / "third_party").glob("*")) if d.is_dir()] if third else []
    cflags += third_inc

    sources = sorted((SDK / "src").glob("*.c")) + sorted((SDK / "src").glob("*.S"))
    sources += third
    sources += sorted(ex.glob("*.c")) + sorted(ex.glob("*.S"))

    objs = []
    for src in sources:
        obj = out / (src.stem + ".o")
        flags = asflags if src.suffix == ".S" else cflags
        if src in third:
            flags = [f for f in cflags if not f.startswith("-W")] + ["-w"]
        run([gcc] + flags + ["-c", src, "-o", obj], args.verbose)
        objs.append(obj)

    elf = out / "app.elf"
    run([gcc] + arch + ["-nostdlib", "-nostartfiles", "-static",
         "-Wl,--build-id=none", "-Wl,--gc-sections",
         f"-Wl,--defsym=LOAD_ADDR={args.load}", f"-Wl,--defsym=STACK_TOP={args.stack}",
         "-Wl,-T," + str(SDK / "rtl8196c.ld"),
         f"-Wl,-Map,{out / 'app.map'}", "-o", elf] + objs, args.verbose)

    binf = out / "app.bin"
    run([objcopy, "-O", "binary", elf, binf], args.verbose)

    r = subprocess.run([str(objdump), "-d", str(elf)], capture_output=True, text=True)
    (out / "app.lst").write_text(r.stdout)
    forbidden, unaligned = scan_disassembly(r.stdout)
    if forbidden:
        sys.exit(f"ERROR: {len(forbidden)} forbidden non-MIPS-I instructions found in {ex.name}")

    data = binf.read_bytes()
    pad = (-len(data)) % 4
    data += b"\0" * pad
    words = struct.unpack(f">{len(data)//4}I", data)
    base = int(args.load, 16)

    # the loader jumps to LOAD_ADDR, not to the ELF entry: byte 0 must be _start
    r = subprocess.run([str(nm), str(elf)], capture_output=True, text=True)
    start_addr = None
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "_start":
            start_addr = int(parts[0], 16) & 0xFFFFFFFF   # Linux nm sign-extends to 64 bits
    if start_addr != base:
        sys.exit(f"_start is at {start_addr:#x}, expected {base:#x}: check the linker script")

    with open(out / "app.ew", "w") as f:
        addr = base
        for i in range(0, len(words), args.words_per_line):
            chunk = words[i:i + args.words_per_line]
            f.write(f"EW {addr:08X} " + " ".join(f"{w:08X}" for w in chunk) + "\n")
            addr += 4 * len(chunk)
        f.write(f"DW {base:08X} {len(words)}\n")
        f.write(f"J {base:08X}\n")

    print(f"{ex.name}: {len(data)} bytes ({len(words)} words) at {base:#x} -> build/{ex.name}/")
    if unaligned:
        print(f"warning: {len(unaligned)} unaligned load/store instructions (lwl/lwr/swl/swr) in the image")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("example", nargs="?", help="directory with main.c (or 'all' to build every example)")
    ap.add_argument("--all", action="store_true", help="build all examples in examples/*")
    ap.add_argument("--march", default="mips1", help="mips1 (default, safest for RLX4181), mips2, mips32")
    ap.add_argument("--load", default="0xA0500000", help="link/load address")
    ap.add_argument("--stack", default="0xA0600000", help="initial stack pointer")
    ap.add_argument("-O", default="2")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--words-per-line", type=int, default=8, help="EW words per loader line")
    args = ap.parse_args()

    binp = find_bin()
    if args.all or args.example in (None, "all"):
        for ex in sorted((ROOT / "examples").iterdir()):
            if ex.is_dir() and (ex / "main.c").exists():
                build_example(ex, args, binp)
    else:
        build_example(ROOT / args.example, args, binp)


if __name__ == "__main__":
    main()

