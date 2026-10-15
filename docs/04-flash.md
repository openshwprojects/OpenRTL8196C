# SPI flash: hardware, boot loader format, backup, writing, partition plan

Everything known about the router's flash, how the factory boot loader
uses it, how we backed it up, how we write it, and the agreed plan for
partitions, OTA and LittleFS. Each fact is tagged:

- **[measured]** observed on our board
- **[backup]** read from our 8 MB backup file
- **[vendor]** from Realtek's RTL8196C boot-code source
  (`rtl8196c-bootcode-1.0a`, mirrored in cobyism/edimax-br-6528n)
- **[plan]** agreed design, not built yet

Status on 2026-10-15: full backup done and verified; erase/program driver
with `< 0x10000` hardware-write guard, partition table (`RPT1`), app header
(`RAPP`), boot-state records (`BTRY`/`BCNF`), stage-2 chooser (`16_stage2`),
`tools/mkimage.py`, and TCP/HTTP OTA (`15_ota`, `tools/ota.py`, and
OpenBeken `/api/ota`) are built and verified on hardware. Flash OTA writes
have been tested in the spare top region (`0x7E0000..0x7FFFFF`) with zeros
restored afterwards, leaving the stock kernel/rootfs untouched until an
explicit owner go-ahead.

---

## 1. The chip

| Item | Value | Source |
|------|-------|--------|
| Part | Macronix MX25L6405 (silkscreen 25L6406E), 64 Mbit | [measured] RDID, kernel log |
| JEDEC ID | `C2 20 17` (Macronix, SPI NOR, 8 MB) | [measured] |
| Size | 8 MB = 0x800000 | [measured] |
| Sector / block / page | 4 KB erase / 64 KB erase / 256 B program | datasheet, [vendor] |
| Chip select | CS0 of the SoC's SPI controller | [vendor], kernel log |
| Status register at hand-off | 0x00 (no write protection, WIP=0) | [measured] |
| Erased state | 0xFF; programming only clears bits | datasheet, [measured] |

## 2. Reading: the memory window

The controller maps the whole chip read-only at physical 0x1D000000,
which we use through KSEG1 as **0xBD000000** (uncached).

- Flash offset `X` is at address `0xBD000000 + X`. [measured]
- The window covers the full 8 MB with no aliasing: our window dump is
  byte-identical to the author's dump made with a CH341 programmer on the
  desoldered chip. [measured]
- The loader configures the window before it hands over to us; we never
  touch that configuration except to save and restore it (section 4).
- Reading through the window needs no driver: `memcpy` from 0xBD000000 is
  all `spiflash_read()` does.

## 3. The controller registers

The RTL8196C has the newer "8198-style" SPI controller. The boot-code
header contains two register sets: the old 8196 one (SFCSR at +4, SFDR at
+8) and the 8198 one (SFCR2 at +4, SFCSR at +8, SFDR at +C). **The 8198
one is correct for this chip.** The boot code selects it with
`SUPPORT_SPI_MIO_8198_8196C`. [vendor] [measured]

| Reg | Addr | Value left by the loader | Meaning |
|-----|------|--------------------------|---------|
| SFCR | 0xB8001200 | 0x3F800000 | bits 31:29 clock divider (1 = /4), bit 28 read byte order big-endian, bit 27 write byte order big-endian, bits 26:23 CS timing (15) |
| SFCR2 | 0xB8001204 | 0x0BC08000 | memory-window read setup: bits 31:24 read command (0x0B = fast read), bits 23:21 size code (6), bit 20 read optimisation (0), bits 15:13 dummy cycles (4), all I/O single-bit |
| SFCSR | 0xB8001208 | 0xC8000000 | bit 31 CSB0, bit 30 CSB1 (1 = deasserted), bits 29:28 transfer length - 1, bit 27 ready, bits 26:25 I/O width, bits 23:16 command byte for CMD-style transfers |
| SFDR | 0xB800120C | | PIO data: bytes travel in the high-order lanes, first byte in bits 31:24 |
| SFDR2 | 0xB8001210 | | data for the CMD-byte style transfers (not used by us) |

SFCSR chip-select field values used by the driver: `CS(1)` = CSB0 low,
CSB1 high = chip 0 selected; `CS(3)` = both high = nothing selected.

## 4. Writing: the PIO protocol

Every command is a short sequence on SFCSR/SFDR. [vendor, verified]

1. Select the chip and set the transfer length:
   `SFCSR = LEN(n-1) | CS(1) | READY`, wait for READY (bit 27).
2. Write up to 4 bytes to SFDR, first byte in bits 31:24. Wait for READY.
3. To read, set the length again and read SFDR, wait for READY.
4. Deselect: `SFCSR = LEN(0) | CS(3) | READY`, wait for READY.

Commands used:

| Cmd | Code | Sequence |
|-----|------|----------|
| WREN (write enable) | 0x06 | 1 byte, deselect |
| RDSR (status) | 0x05 | 1 byte out, read 1 byte, poll bit 0 (WIP) until clear |
| RDID | 0x9F | 1 byte out, then a 4-byte read; ID in bits 31:8 |
| SE (4 KB sector erase) | 0x20 | WREN; one 4-byte word `0x20 << 24 | offset`; deselect; poll RDSR |
| BE (64 KB block erase) | 0xD8 | as SE |
| PP (page program) | 0x02 | WREN; one 4-byte word `0x02 << 24 | offset`; then the data in 4-byte words (shorter tail with LEN(n-1)); deselect; poll RDSR |

Rules the driver follows:

- **Save SFCR2 before any PIO and restore it after.** The vendor code
  masks bits in SFCR2 during PIO; restoring it is what keeps the window
  usable afterwards. [measured, see section 5]
- **Always deselect through SFCSR at 0xB8001208.**
- A page program must stay inside one 256-byte page; the driver splits
  writes on page boundaries.
- Programming can only turn 1s into 0s, so the target must be erased
  first. `spiflash_write()` does not erase; callers erase whole sectors.

Measured speeds on the 4 KB test sector:

| Operation | Time |
|-----------|------|
| Sector erase (4 KB) | 42 ms |
| Program 4 KB (16 pages) | 17 ms |
| Read the whole chip over TCP | 60 s (TCP-limited, 136 KB/s) |

That puts a full 2 MB slot at roughly 22 s erase (512 sectors) plus 9 s
programming. Using 64 KB block erases is probably faster; not measured yet.

API (`sdk/include/spiflash.h`): `spiflash_jedec_id`, `spiflash_status`,
`spiflash_read`, `spiflash_erase_sector`, `spiflash_erase_block`,
`spiflash_write`, `spiflash_verify`.

## 5. The trap we hit

Our first driver used the old 8196 register offsets from the same vendor
header. Its "deselect" writes went to 0xB8001204, which on this chip is
SFCR2, the memory-window configuration. Symptoms:

- The first two backups came back as junk (mostly 0x00 with 0xFF at the
  start) even though no flash had been written.
- RDID returned 0x9F000000, i.e. our own command byte.
- After any PIO command the window read 0xFF everywhere until a reset.

`probes/96_spi_probe` showed that putting SFCR2 back to 0x0BC08000
restored the window immediately. Switching to the 8198 offsets fixed the
driver: RDID returns C22017 and the window survives every command.
Nothing had been erased or programmed during the faulty runs; the next
backup matched the programmer dump exactly.

## 6. What is on the stock flash

From `backups/flash_20261014_222426.bin`. [backup]

| Offset | End | Content |
|--------|-----|---------|
| 0x000000 | ~0x005730 | factory boot loader code |
| 0x006000 | | hardware settings block, tag `H601`, length 0x048E (factory MAC, RF calibration) |
| 0x008000 | | default/current settings block, tag `6V03` |
| 0x010000 | 0x11FFF0 | Linux kernel image: header `cr6c`, start 0x80500000, burn 0x10000, len 0x10FFE0 |
| 0x120000 | 0x5D6BB0 | SquashFS 4.0 root file system, `hsqs`, 4,942,768 bytes |
| ~0x5DB003 | 0x800000 | unused, filled with **0x00** (never erased), about 2.1 MB |

The first 64 KB (loader and both settings blocks) are never to be
written (enforced by `SPIFLASH_PROTECTED_END = 0x010000u` in `spiflash.c`).
The factory LAN MAC (`80:1f:02:ca:ad:92`, Edimax OUI) sits inside the `H601`
block at flash offset `0x006007` (`0xBD006007`) and is read by
`spiflash_factory_mac()`. [measured]

## 7. How the factory loader boots an image

[vendor, checked against the backup]

1. At power-on it looks for an image header at flash offsets **0x10000**,
   then **0x20000**, then **0x30000**, and takes the first valid one.
2. The header is 16 bytes, big-endian:

   ```c
   struct img_header {
       char     signature[4];   /* "cs6c" or "cr6c" */
       uint32_t start_addr;     /* RAM address to copy to and jump to */
       uint32_t burn_addr;      /* informational: flash offset */
       uint32_t len;            /* bytes after the header, including the checksum word */
   };
   ```

3. Signature `cs6c` means "code only". `cr6c` means "code with root file
   system": only the first two letters are compared, and the loader then
   also requires a SquashFS signature at 0x120000 (or a list of other
   offsets). This build has the rootfs checksum check disabled, so only
   the signature matters there.
4. Checksum: the 16-bit big-endian words of the `len` bytes after the
   header must add up to **0** (mod 65536). The last word is the
   checksum that makes it so. Verified on the stock kernel: the sum is
   0x0000 and the last word is 0xF600.
5. If a valid image exists, the loader waits about 3 s for ESC on the
   UART (or the reset button). ESC gives the `<RealTek>` console; this
   never depends on the image, which is why it is our emergency path.
6. Otherwise it copies `len - 2` bytes from flash `offset + 16` to
   `start_addr`, prints `Jump to image start=0x...`, masks all
   interrupts in GIMR, flushes the caches and jumps to `start_addr`.

So any program can be booted from flash by giving it a `cs6c` header with
a correct checksum word and placing it at 0x10000.

Note: the loader's `J` command clears `EnablePHYIf` on all switch ports
before jumping (see 01-hardware-notes.md). Whether the power-on boot path
does the same is not verified; `eth_init()` sets the bit either way.

## 8. Procedures

### 8.1 Full backup (done)

Needs the LAN cable and the UART bench.

```bash
python tools/flash.py build/14_flash_backup/app.bin --listen 130
```

In a second terminal, about a minute later when the board is up:

```bash
python tools/flashdump.py
```

`examples/14_flash_backup` streams the window over TCP port 9697: a
16-byte header (`RTLF`, JEDEC ID, size) and then the 8 MB. It does no PIO
at all, so it cannot disturb the flash. `tools/flashdump.py` saves
`backups/flash_<date>.bin` and prints the SHA-256 and the layout.

Result: `backups/flash_20261014_222426.bin`, SHA-256
`bce0d187a8550cdf69450918a0a76799c1713c713e4c7c32038b48dff54e5245`.
Checked three ways: a second pass matched (`flashdump.py --verify`), it
is identical to the CH341 dump in openshwprojects/FlashDumps
(`Networking/Router/LT-6508n-RTL8196C.bin`), and the kernel header
checksum adds up. `backups/` is git-ignored: keep a copy elsewhere.

### 8.2 First write test (done)

`probes/97_spi_write_test` uses only the last sector, 0x7FF000, which is
part of the unused zero-filled area:

1. erase, check all 0xFF
2. program a 4 KB pattern, verify
3. erase again, check all 0xFF
4. program the original zeros back

All four steps passed, and the flash window kept working throughout. A
full re-dump afterwards was byte-identical to the backup.

### 8.3 Recovery (if a flashed image does not boot)

The loader itself is never written, so ESC always works:

1. `python tools/flash.py --prompt-only` (power-cycles and catches ESC).
2. From the console, load a program into RAM and run it as today. Only
   the boot image is broken, nothing else.

To put the stock firmware back:

- **Planned and preferred:** a small flash-writer program (to be written,
  same idea as 14 but receiving data) loaded through the loader, then the
  backup streamed in over Ethernet and programmed. About 1 minute.
- **Loader only:** get the data into RAM and use the loader's flash
  write command. The vendor source has two variants,
  `FLW <dst> <src> <length>` and, for SPI builds,
  `FLW <flash_offset> <ram_addr> <length_bytes> <spi_chip>`. Which one our
  loader has is **still to confirm with `HELP`** (the USB-UART adapter was
  unplugged when this was written). Uploading 5.7 MB with `EW` would take
  hours, so this needs TFTP (`LOADADDR`, then a TFTP put) or the writer
  program.
- **Last resort:** desolder the chip and write the backup with a CH341
  programmer, as the article's author did.

## 9. Agreed partition plan [plan]

Requirements from the owner: keep the ESC UART recovery, OTA over TCP,
some space for files (LittleFS), and 2 MB per app slot because the
target is OpenBeken.

| Offset | Size | Content |
|--------|------|---------|
| 0x000000 | 64 KB | factory loader + settings blocks, never written |
| 0x010000 | 56 KB | stage-2 chooser with a `cs6c` header, written once |
| 0x01E000 | 4 KB | boot state (append-only records, see below) |
| 0x01F000 | 4 KB | partition table |
| 0x020000 | 2 MB | app slot A |
| 0x220000 | 2 MB | app slot B |
| 0x420000 | 3.875 MB | LittleFS, 992 blocks of 4 KB |

Writing the stage-2 at 0x10000 replaces the stock kernel header, so from
then on power-on boots our code. The SquashFS and the rest of the stock
data get overwritten as slots and the file system are used; the backup is
the way back.

### 9.1 Partition table (0x1F000)

One 4 KB sector, read by both the stage-2 and the app, so sizes can be
changed later without rebuilding either:

```c
struct part_entry { char name[8]; uint32_t offset, size, flags; };
struct part_table {
    uint32_t magic;            /* 'RPT1' */
    uint16_t version, count;
    struct part_entry e[8];    /* "boot", "appA", "appB", "lfs", ... */
    uint32_t crc32;            /* over everything above */
};
```

Growing the slots later means moving LittleFS: back up its files,
rewrite the table, reformat, restore. The stage-2 does not change.

### 9.2 App image header (start of each slot)

Our own header, separate from the loader's `cs6c` header:

```c
struct app_header {
    uint32_t magic;            /* 'RAPP' */
    uint32_t hdr_version;
    uint32_t seq;              /* higher = newer, set by OTA */
    uint32_t load_addr;        /* where the stage-2 copies the payload */
    uint32_t entry;
    uint32_t length;           /* payload bytes */
    uint32_t payload_crc32;
    char     version[16];      /* e.g. git describe */
    uint32_t build_time;
    uint32_t header_crc32;
};
```

`tools/mkimage.py` (to be written) will produce both kinds of image: the
stage-2 with the loader's `cs6c` header and checksum word, and apps with
the header above.

### 9.3 Stage-2 chooser

About 100 lines, reads only, no flash driver:

1. Read the partition table through the window; fall back to the
   built-in default layout if its CRC is bad.
2. Validate both slot headers and payload CRCs.
3. Pick the valid slot with the highest `seq`, unless the boot state says
   it failed to confirm three times in a row; then pick the other one.
4. Append a "trying slot X" record to the boot state.
5. Copy the payload to `load_addr`, invalidate the I-cache (CCTL), jump.

It runs from the address in its `cs6c` header (0xA0500000, known to be
free while the loader runs) and refuses an app `load_addr` that overlaps
itself. Apps will be linked high enough to leave room, for example
0x80800000.

### 9.4 Boot state (0x1E000)

Append-only 16-byte records in one erased sector: "trying slot X, seq N"
written by the stage-2, "confirmed" written by the app once it is up
(network running, a few seconds without a crash). Only bits 1 to 0 are
written, so no erase per boot; the sector is erased when full. Three
unconfirmed attempts in a row make the stage-2 fall back to the other
slot.

### 9.5 OTA over TCP

1. The running app accepts an image (HTTP `POST /ota` or a raw TCP port)
   into RAM; there are 32 MB.
2. It checks the header magic, the CRCs and that `length` fits the slot.
3. It erases and programs the **inactive** slot, then verifies it
   against RAM.
4. It writes the new `seq` (one higher than the active slot) last, so a
   power cut before this point leaves the old slot as the newest valid
   one.
5. It reboots through the watchdog. The stage-2 boots the new slot; if
   the new app never confirms, the stage-2 goes back to the old slot.

PC side: `tools/ota.py <image>` (to be written).

### 9.6 LittleFS

The upstream C library (MIT) only needs read, prog, erase and sync
callbacks: `read` through the window, `prog` and `erase` through
`spiflash.c`. Configuration: block size 4096, 992 blocks, prog size 256,
read size 16 or more.

## 10. Safety rules

- Never write below 0x10000.
- Every new kind of flash write is tried first on the spare sector
  0x7FF000, then checked with a full re-dump against the backup.
- The first write of each roadmap step needs an explicit go from the
  owner.
- Keep `backups/flash_20261014_222426.bin` safe; it is git-ignored.

## 11. Open questions

- Exact `FLW` syntax of our loader (`HELP` output).
- Highest safe SPI clock (the loader uses /4).
- Answered: the factory MAC address sits in `H601` at `0x006007` (`80:1f:02:ca:ad:92`).
- Answered: the power-on flash boot path (`Jump to image start=0xa0500000...`) skips `eth_startup()` completely (`SW_FFCR = 0`); `eth_init()` in `sdk/src/swcore.c` now performs the full switch ASIC and PHY initialization (`swcore_hw_init()`) whenever `(REG32(SW_FFCR) & 3) != 3`.

## 12. Standardized SPI Flash Installer (`tools/flash_obk.py`)

`tools/flash_obk.py` automates building, flashing, updating, and verifying the complete SPI flash layout while strictly protecting `0x000000..0x00FFFF`:

```bash
# Full install: writes RPT1 (0x01F000), bootstate (0x01E000), OpenBeken (0x020000/0x220000),
# stage2.cs6c (0x010000), and verifies hands-off cold boot from SPI flash:
python tools/flash_obk.py

# Update only the inactive A/B slot (appA/appB) over Ethernet OTA:
python tools/flash_obk.py --update-only

# Power-cycle the router (no ESC) and verify standalone cold boot from SPI flash:
python tools/flash_obk.py --verify-only

# Restore stock firmware (0x010000..0x5E0000) from the 8 MB backup:
python tools/flash_obk.py --restore-stock backups/flash_20261014_222426.bin
```

## 13. Files

| File | Role |
|------|------|
| `sdk/include/spiflash.h`, `sdk/src/spiflash.c` | flash driver (`< 0x10000` write guard, `spiflash_erase_range`, `spiflash_factory_mac`) |
| `sdk/include/flash_layout.h`, `sdk/src/flash_layout.c` | `cs6c`, `RPT1` partition table, `RAPP` app header, `BTRY`/`BCNF` boot state, `stage2_choose_slot` |
| `sdk/include/ota.h`, `sdk/src/ota.c` | TCP OTA server on port 9698 (`status`, `ram`, `test_flash`, `flash`, `raw_flash`) + RAM relocator |
| `examples/14_flash_backup` | streams the flash over TCP port 9697 |
| `examples/15_ota` | DHCP + HTTP + TCP/UDP console + TCP OTA server |
| `examples/16_stage2` | stage-2 bootloader (`cs6c` image for `0x010000`) |
| `tools/flashdump.py` | saves and verifies backups |
| `tools/mkimage.py` | packs/inspects `cs6c` stage-2 images, `RAPP` app images, and `RPT1` partition tables |
| `tools/ota.py` | Ethernet OTA client (`--status`, `--ram`, `--test-flash`, `--flash`, `--raw-flash`) |
| `tools/flash_obk.py` | standardized SPI flash installer, A/B slot updater, cold-boot verifier, and stock restorer |
| `probes/96_spi_probe` | read-only controller checks |
| `probes/97_spi_write_test` | erase/program test on 0x7FF000 |
| `backups/flash_20261014_222426.bin` | stock 8 MB image |

