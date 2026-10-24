#!/usr/bin/env python3
"""Build OpenBeken for Realtek RTL8196C (Lexra RLX4181).

Produces:
  build/obk/openbeken_rtl8196c.elf  (linked ELF)
  build/obk/openbeken_rtl8196c.bin  (raw binary at 0x80500000)
  build/obk/openbeken_rtl8196c.img  (RAPP-wrapped OTA image)
  build/obk/openbeken_rtl8196c.lst  (disassembly)
  build/obk/openbeken_rtl8196c.map  (linker map)
"""
import argparse
import concurrent.futures
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT / "sdk"


def default_obk_root() -> Path:
    for cand in (
        ROOT.parent.parent,                  # OpenBK7231T_App/sdk/OpenRTL8196C
        ROOT.parent / "OpenBK7231T_App",     # sibling ../OpenBK7231T_App
        ROOT / "OpenBK7231T_App",            # child ./OpenBK7231T_App
    ):
        if (cand / "platforms" / "obk_main.mk").exists():
            return cand
    return ROOT.parent / "OpenBK7231T_App"


DEFAULT_OBK = default_obk_root()

sys.path.insert(0, str(ROOT / "tools"))
import build  # noqa: E402


def parse_obk_sources(obk_root: Path):
    mk = obk_root / "platforms" / "obk_main.mk"
    plat_mk = obk_root / "platforms" / "RTL8196C" / "OpenBeken.mk"
    berry_mk = obk_root / "libraries" / "berry.mk"
    if not mk.exists():
        sys.exit(f"missing {mk}")
    srcs = []
    include_berry = False
    if plat_mk.exists():
        for raw in plat_mk.read_text(encoding="utf-8", errors="replace").splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"^SRC_C\s*\+=\s*\$\(OBK_DIR\)/src/(.+\.c)\s*$", line)
            if m:
                srcs.append(obk_root / "src" / m.group(1).strip())
            if "berry.mk" in line:
                include_berry = True
    for raw in mk.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^OBKM_SRC\s*\+=\s*\$\(OBK_SRCS\)(.+\.c)\s*$", line)
        if m:
            rel = m.group(1).strip()
            srcs.append(obk_root / "src" / rel)
    if include_berry and berry_mk.exists():
        for raw in berry_mk.read_text(encoding="utf-8", errors="replace").splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"^BERRY_SRC_C\s*\+=\s*\$\(BERRY_SRCPATH\)/(.+\.c)\s*$", line)
            if m:
                srcs.append(obk_root / "libraries" / "berry" / "src" / m.group(1).strip())
                continue
            m = re.match(r"^BERRY_SRC_C\s*\+=\s*\$\(BERRY_MODULEPATH\)/(.+\.c)\s*$", line)
            if m:
                srcs.append((obk_root / "src" / "berry" / "modules" / m.group(1).strip()).resolve())
    return srcs


def ensure_berry_generated(obk_root: Path):
    gen_dir = obk_root / "libraries" / "berry" / "generate"
    coc = obk_root / "libraries" / "berry" / "tools" / "coc" / "coc"
    if (gen_dir / "be_const_strtab_def.h").exists() or not coc.exists():
        return
    gen_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        sys.executable, str(coc),
        "-o", str(gen_dir),
        str(obk_root / "libraries" / "berry" / "src"),
        str(obk_root / "src" / "berry" / "modules"),
        "-c", str(obk_root / "include" / "berry_conf.h"),
    ], check=True)


def main():
    ap = argparse.ArgumentParser(description="Build OpenBeken for OpenRTL8196C")
    ap.add_argument("--obk", default=str(DEFAULT_OBK), help="Path to OpenBK7231T_App repo")
    ap.add_argument("--load", default="0x80500000", help="RAM load/entry address (cached KSEG0)")
    ap.add_argument("--stack", default="0x806F0000", help="Initial main stack top (below heap at 0x80700000)")
    ap.add_argument("--ver", default="1.18.RTL8196C", help="USER_SW_VER string")
    ap.add_argument("--seq", type=int, default=1, help="RAPP sequence number")
    ap.add_argument("--clean", action="store_true", help="Force full rebuild of all objects")
    ap.add_argument("-j", "--jobs", type=int, default=8, help="Parallel compile jobs")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    t0 = time.time()
    obk_root = Path(args.obk).resolve()
    binp = build.find_bin()
    gcc = binp / (build.PREFIX + "gcc")
    objcopy = binp / (build.PREFIX + "objcopy")
    objdump = binp / (build.PREFIX + "objdump")
    nm = binp / (build.PREFIX + "nm")
    size_tool = binp / (build.PREFIX + "size")

    out = ROOT / "build" / "obk"
    obj_dir = out / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)

    # Ensure Berry precompiled object headers exist
    ensure_berry_generated(obk_root)

    # Ensure wifi_blobs.inc exists in build/obk (git-ignored)
    blobs_inc = out / "wifi_blobs.inc"
    if not blobs_inc.exists():
        existing_blobs = sorted((ROOT / "examples").glob("*/wifi_blobs.inc"))
        if existing_blobs:
            blobs_inc.write_bytes(existing_blobs[0].read_bytes())
        else:
            backups = sorted((ROOT / "backups").glob("flash_*.bin"))
            if backups:
                subprocess.run([
                    sys.executable, str(ROOT / "tools" / "extract_wifi_blobs.py"),
                    str(backups[-1]), "--blobs-inc", str(blobs_inc)
                ], check=True)
            else:
                sys.exit("missing wifi_blobs.inc: run tools/extract_wifi_blobs.py on your flash backup first")

    arch = build.arch_flags("mips1")
    common_flags = build.common_cflags("mips1", "s") + [
        "-DPLATFORM_RTL8196C=1",
        f'-DUSER_SW_VER="{args.ver}"',
        f"-I{SDK / 'include' / 'obk_shim'}",
        f"-I{SDK / 'include'}",
        f"-I{SDK / 'third_party' / 'fatfs'}",
        f"-I{out}",
        f"-I{obk_root / 'include'}",
        f"-I{obk_root / 'src'}",
        f"-I{obk_root / 'libraries' / 'berry' / 'src'}",
    ]
    sdk_cflags = common_flags + ["-Wall", "-Wextra", "-Wno-unused-parameter"]
    obk_cflags = common_flags + [
        "-w",  # suppress 3rd-party driver warnings while keeping errors fatal
    ]
    asflags = build.as_flags("mips1")

    sdk_exclude = {"httpd.c", "statuspage.c", "console.c"}
    sdk_srcs = [
        p for p in (
            sorted((SDK / "src").glob("*.S"))
            + sorted((SDK / "src").glob("*.c"))
            + sorted((SDK / "third_party" / "fatfs").glob("*.c"))
        )
        if p.name not in sdk_exclude
    ]
    obk_srcs = parse_obk_sources(obk_root)

    tasks = []
    objs = []

    for src in sdk_srcs:
        obj = obj_dir / f"sdk_{src.stem}.o"
        objs.append(obj)
        flags = asflags if src.suffix == ".S" else sdk_cflags
        tasks.append((src, obj, flags))

    for src in obk_srcs:
        rel = src.resolve().relative_to(obk_root.resolve())
        safe_name = "obk_" + "_".join(rel.parts).replace(".c", ".o")
        obj = obj_dir / safe_name
        objs.append(obj)
        tasks.append((src, obj, obk_cflags))

    def compile_one(item):
        src, obj, flags = item
        if not args.clean and obj.exists() and obj.stat().st_mtime >= src.stat().st_mtime:
            return (src, 0, "", "", True)
        cmd = [str(gcc)] + [str(f) for f in flags] + ["-c", str(src), "-o", str(obj)]
        if args.verbose:
            print(" ".join(cmd))
        r = subprocess.run(cmd, capture_output=True, text=True)
        return (src, r.returncode, r.stdout, r.stderr, False)

    errors = 0
    compiled = 0
    cached = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for src, rc, out_s, err_s, was_cached in ex.map(compile_one, tasks):
            if was_cached:
                cached += 1
            else:
                compiled += 1
            if out_s:
                print(out_s, end="")
            if err_s:
                print(err_s, end="", file=sys.stderr)
            if rc != 0:
                errors += 1

    if errors:
        sys.exit(f"Build failed: {errors} file(s) failed to compile.")

    elf = out / "openbeken_rtl8196c.elf"
    binf = out / "openbeken_rtl8196c.bin"
    imgf = out / "openbeken_rtl8196c.img"
    mapf = out / "openbeken_rtl8196c.map"
    lstf = out / "openbeken_rtl8196c.lst"

    link_cmd = [
        str(gcc), *arch,
        "-nostdlib", "-nostartfiles", "-static",
        "-Wl,--build-id=none", "-Wl,--gc-sections",
        f"-Wl,--defsym=LOAD_ADDR={args.load}",
        f"-Wl,--defsym=STACK_TOP={args.stack}",
        f"-Wl,-T,{SDK / 'rtl8196c.ld'}",
        f"-Wl,-Map,{mapf}",
        "-o", str(elf),
        *[str(o) for o in objs],
    ]
    r = subprocess.run(link_cmd, capture_output=True, text=True)
    if r.stdout:
        print(r.stdout, end="")
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        sys.exit(f"Link failed ({r.returncode})")

    subprocess.run([str(objcopy), "-O", "binary", str(elf), str(binf)], check=True)

    # Wrap in RAPP header using mkimage.py
    subprocess.run([
        sys.executable, str(ROOT / "tools" / "mkimage.py"),
        "--type", "rapp",
        "-o", str(imgf),
        "--load", args.load,
        "--entry", args.load,
        "--seq", str(args.seq),
        "--version", args.ver,
        str(binf),
    ], check=True)

    # Disassemble & check ISA cleanliness
    r_dis = subprocess.run([str(objdump), "-d", str(elf)], capture_output=True, text=True)
    lstf.write_text(r_dis.stdout, encoding="utf-8")
    forbidden, lwl_insns = build.scan_disassembly(r_dis.stdout)

    # Check symbol layout
    r_nm = subprocess.run([str(nm), str(elf)], capture_output=True, text=True)
    syms = {}
    for line in r_nm.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)

    base = int(args.load, 16)
    stack_top = int(args.stack, 16)
    start_addr = syms.get("_start", 0)
    end_addr = syms.get("_end", 0)
    bss_start = syms.get("_bss_start", 0)

    if start_addr != base:
        sys.exit(f"ERROR: _start is at {start_addr:#x}, expected {base:#x}")
    if end_addr >= stack_top - 0x10000:
        sys.exit(f"ERROR: _end ({end_addr:#x}) is too close to STACK_TOP ({stack_top:#x})")
    if forbidden:
        sys.exit(f"ERROR: {len(forbidden)} forbidden non-MIPS-I instructions found:\n" + "\n".join(forbidden[:10]))

    r_sz = subprocess.run([str(size_tool), str(elf)], capture_output=True, text=True)
    dt = time.time() - t0
    raw_len = binf.stat().st_size
    img_len = imgf.stat().st_size

    print(r_sz.stdout.strip())
    print(f"[build_obk] Compiled {compiled} ({cached} cached) in {dt:.1f}s")
    print(f"[build_obk] Binary: {raw_len} bytes ({raw_len/1024:.1f} KB) -> {binf}")
    print(f"[build_obk] RAPP:   {img_len} bytes ({img_len/1024:.1f} KB) -> {imgf}")
    print(f"[build_obk] Layout: _start={start_addr:#010x} _bss={bss_start:#010x} _end={end_addr:#010x} stack={stack_top:#010x}")
    print(f"[build_obk] ISA check: 0 forbidden MIPS-II/32 insns, {len(lwl_insns)} lwl/lwr/swl/swr (emulated by irq.c)")


if __name__ == "__main__":
    main()

