# Bring-up log

Chronological record of what was tried on the bench. Newest at the bottom.

## 2026-10-14

**Bench**: LT-6408n router, CH340 USB-UART on COM4 (38400 8N1), mains power
through an OpenBeken socket at 192.168.0.240 (Tasmota HTTP API
`/cm?cmnd=Power%20OFF` / `Power%20ON`). PC: Windows 10, Python 3.13 +
pyserial, LLVM/clang as the cross compiler (WSL unavailable, no
virtualisation).

**Receive path works.** Power-cycling the socket and listening on COM4 gives
the loader banner and the full Linux boot log. The stock firmware ends in
`WEBS Restarting !` / `Initialize AP MIB failed!` and loops, exactly as the
article's author saw.

**Transmit path does not.** Three attempts to reach the loader prompt
failed:

| Attempt | Result |
|---------|--------|
| ESC once when "Booting..." appears | boot continues |
| ESC every 30 ms after "Booting..." | boot continues |
| ESC every ~10 ms from power-on for 25 s | boot continues |

Typing into the running Linux console (`echo`, `cat /proc/cpuinfo`) also
gets no echo and no reply. The author's stock firmware ignored input too,
so only the loader test is conclusive. Suspect: adapter TX not connected
to the router RX pad.

**Loader protocol (from part 1 of the article)**: prompt `<RealTek>`, dump
format `80008000:<tab>00001021<tab>0800200D ...`, flash is memory mapped at
0xBD000000, ESC during boot stops the loader.

**SDK skeleton written** (not yet compiled on hardware):
`sdk/` (start.S, exception.S, uart, printf, irq, timer, gpio, libc),
`examples/01..07`, `tools/build.py`, `tools/flash.py`.

**Toolchain hunt.** WSL cannot start on this PC (virtualisation disabled in
firmware). The official LLVM Windows build (23.1.3 via winget) turned out to
have no MIPS backend at all (`clang -print-targets`). The MIPS Codescape GNU
Tools 2019.09-03 "MTI Bare Metal" package (GCC 7.4.0, binutils) is still
downloadable for Windows x64 (450 MB, sha256 verified) and is now unpacked in
`toolchain/`. With `-march=mips1` GNU as fills the load delay slots itself,
which is exactly what an R3000-class core wants. All seven examples build;
01_hello is 5140 bytes.

**First code running.** After a wiring fix on the UART header the loader
prompt appeared (161 ESCs over 5.2 s after power-on; the prompt follows the
"---Ethernet init Okay!" line). `flash.py` wrote 01_hello (1285 words) with
EW, verified it with DW and jumped. Output:

```
PRId   = 0000cd01
Status = 00000000
Cause  = 00000000
GIMR   = 00000000  GISR = 00007002
IRR0-3 = 00000000 04000003 00000000 00000000
TCCNR  = c0000000  TCIR = a0000000  CDBR = 000e0000  WDT = a5000000
UART   LSR=00 IER=00 IIR=c1 LCR=03 MCR=00
PINMUX = 00040000
```

Conclusions: PRId 0xCD01 = RLX4181; Status.BEV = 0 and interrupts are off
at the prompt; the loader leaves TC0 running in periodic mode with divisor
14 and TC0IE set (TC0IP pending, GISR bit 8 set), so the loader itself uses
timer 0 with its own routing nibble 3; the watchdog is stopped (0xA5000000);
UART0 is 8N1 with FIFOs on; GCC `-march=mips1` code runs fine from the
uncached alias.

**Timers measured** (`examples/90_timer_probe`, PC timestamps): counters
count up from 0 to TCxDATA, wrap and set TCxIP; count in bits 31:4; with
divisor 200 the rate is 1.0014 MHz, so the bus clock is 200 MHz. The loader
runs TC0 at a 10 ms tick (divisor 14, data 0x22E0A). 02_counter prints
1 2 3 ... at ~0.8 s per number with `delay_loop(2000000)` (uncached code).

**Exception vector confirmed** (`examples/91_irq_probe`): trampolines at
0x000/0x080/0x180, `break` twice, restore 0x000 and 0x180, `break` again:
count went to 3, so the general exception vector is **0x80000080** and the
`jr $k0 ; rfe` return works. The loader's own words at 0x080 are
`401b6800 00000000` (`mfc0 $k1, $13`), i.e. the loader keeps a handler there.

**Interrupts not yet reaching the CPU.** With TC0IE set and TC0IP pending,
GISR stays at 0x7002 and Cause.IP stays 0 for every IRR1 nibble-0 value
0..15, with GIMR = 0xFFFFFFFF. Either the controller layout differs from
the RTL8196E header or the RLX4181 needs its Lexra extension registers
programmed. Next: `examples/92_intc_probe`.

**Routing semantics measured** (`examples/92_intc_probe`): with all 32 IRR
nibbles set to the same value N and TC0 wrapping, Cause.IP shows exactly
line N for N = 2..7 (0x400, 0x800 ... 0x8000); N = 0, 1, 8..15 raise
nothing. GIMR and IRRx read back what was written. GISR is stuck at 0x7002
and never reflects TC0. CP0 census: $1 = 0x1B00, $6 = 4, $15 = 0xCD01,
everything else 0 and no register traps. A software interrupt set via
Cause.IP0 reads back. Cause bit 31 reads 1 at rest.

**Source numbers measured** (`examples/93_route_probe`): baseline
always-pending sources = {1, 12, 13}; TC0 = 14, TC1 = 15, UART0 = 7, each
gated by the same-numbered GIMR bit. The constant GISR 0x7002 was simply
bits 1, 12, 13 plus TC0 pending. Header updated.

**Timer interrupt works** (`examples/05_timer_irq`): TC0 routed to IP2,
GIMR bit 14, Status 0xFC01. One interrupt per tick, ticks 1.000 s apart
over 11 s, Cause 0x400 (IP2) in the handler, Cause.BD occasionally set
(interrupt taken in a branch delay slot, return still correct).

**UART in, polled and by interrupt.** `06_uart_echo` echoes injected lines
(`flash.py --send`) and its `reboot` command, which writes 0 to WDTCNR,
restarts the SoC: the loader prints "Reboot Result from Watchdog Timeout!".
`07_uart_irq` with IER = RDA and source 7 routed to IP2 counted exactly one
interrupt per received byte (17 and 38) while TC0 ticked every second.
Side effect found: clearing the FIFOs in `uart_init()` while the loader's
"Jump to address" text is still in the TX FIFO garbles a few characters;
fixed by waiting for LSR.TEMT first.

## 2026-10-14, evening: regression suite and Ethernet

**Regression suite** `tools/test_examples.py` runs every example with
assertions (consecutive counters, 1.000 s tick spacing, byte-exact echo of
a 190-byte burst, one UART interrupt per byte, watchdog reboot). All 7 pass
after fixing one over-strict parser.

**Ethernet.** Driver written from the boot code's `swNic_poll.c` /
`swCore.c` (same chip). `08_eth_link` immediately showed link on switch
port 3 only (100 Mb/s full duplex, PHY id 001c:c880) but zero frames at
the CPU port, even for ARP broadcasts aimed at the board. Bisecting with
the loader's own rings (`94`, `95`) showed receive was dead from the
moment the loader jumped, independent of anything we wrote. The loader's
`J` handler turned out to clear `EnablePHYIf` in PCRP0..4 (visible as
0x3e instead of 0x3f). Setting the bit back in `eth_init()` fixed it:
broadcasts arrive tagged with their source port, `09_eth_ping` answers
ARP and ICMP, the PC pings the board (TTL 64, <1 ms), and the board's own
ARP requests get answered by the PC. Windows caches a failed ARP as
"Unreachable" and stops retrying, which is why the board now announces
itself with a periodic ARP request.

**UDP console** (`sdk/src/net.c`, `examples/11_udp_console`,
`tools/netcon.py`): a 300-line polled stack (ARP with a cache keyed by
source switch port, ICMP echo, UDP in/out with checksums). From the PC:
`hi`, `help`, `uptime`, `link`, `stats`, `peek`, `poke`, `led`, `reboot`
all answer in 9-14 ms; 50 echo datagrams of 520 bytes on port 7 came back
intact with 1.9 ms average round trip. REVR reads 0x80000002: this is a
revision B RTL8196C.

**HTTP server** (`sdk/src/tcp.c`, `examples/12_http_server`): a
request/response TCP server (4 connections, MSS 1400, response kept for
retransmission every 300 ms, 10 s idle timeout) under a page with status,
LED buttons, register dump and a JSON endpoint. curl and a browser both
work; a full page fetch including handshake takes 36-70 ms from the PC.

**How the network side fits together:** the SoC's 5-port switch forwards
between the jacks in hardware; the CPU port only sees broadcasts, unknown
destinations and frames to the board's MAC, so the board is one host with
one MAC/IP behind its own switch. The loader's switch setup is reused.
DHCP is not implemented yet (static 192.168.0.196).

**DRY pass and sockets.** `board_config.h` holds MAC/IP/ports; `cli.c`
(string builder, parsers) and `console.c` (the command set) are shared by
the UDP console, the new TCP console and the web pages (`statuspage.c`);
`vsnprintf` got its proper name; `delay_loop` moved to the timer module;
`every_ms()` replaced the hand-written elapsed-time checks; probes moved to
`probes/`; the tools read `bench.ini`. `tcp.c` was rewritten as a socket
style core: up to 4 listening ports and 8 simultaneous connections, each
with an 8 KB send buffer, peer-window flow control, go-back-N retransmit
every 300 ms, states through FIN_WAIT/CLOSE_WAIT/LAST_ACK. `httpd.c`
(routes, GET/HEAD, Content-Length) sits on top. `13_multi_socket` runs
HTTP, a telnet-style console on 23, TCP/UDP echo on 7 and the UDP console
at the same time; the regression test opens four TCP consoles at once,
fetches pages while they are open and echoes 2 KB blocks over TCP.

**Full flash backup** (`examples/14_flash_backup`, `tools/flashdump.py`):
the board streams the memory-mapped flash window (0xBD000000, 8 MB) over
TCP at ~136 KB/s, 60 s for the whole chip. Two passes identical, and the
file is byte-identical to the author's CH341 programmer dump of the same
router (openshwprojects/FlashDumps, LT-6508n-RTL8196C.bin), so the window
covers the full 8 MB with no aliasing. Saved as
`backups/flash_20261014_222426.bin`, sha256
bce0d187a8550cdf69450918a0a76799c1713c713e4c7c32038b48dff54e5245. Layout
seen: loader 0..0xFFFF, `cr6c` kernel at 0x10000 (len 0x10FFE0), SquashFS
at 0x120000 (4.9 MB), and the top 2 MB are all 0x00 (not erased).
Lesson: issuing PIO commands (RDID) on the SPI controller before using
the window made window reads return 0xFF until the next reset, so the
backup server does no PIO at all; `probes/96_spi_probe` investigates.

**SPI flash driver and first write** (`sdk/src/spiflash.c`,
`probes/96_spi_probe`, `probes/97_spi_write_test`). The RTL8196C has the
"8198-style" controller: SFCR 0xB8001200, SFCR2 0xB8001204 (memory window
settings, loader value 0x0bc08000), SFCSR 0xB8001208, SFDR 0xB800120C.
The first driver used the older 8196 offsets and its "deselect" writes
landed in SFCR2, which blanked the window (reads 0xFF) until reset; with
the right offsets RDID returns c22017 and the window survives. The write
test on the unused last sector 0x7FF000: sector erase 42 ms, 4 KB program
17 ms (256-byte pages), verify OK, erase back to 0xFF, zeros restored.
A full re-dump afterwards is byte-identical to the stock backup, so
nothing else on the chip was touched.

## 2026-10-15: USB host

`sdk/src/usb.c` replaces the probe code: stock-kernel init sequence
(verified by disassembling the stock kernel from the flash backup), EHCI
plus OHCI companion, hotplug by polling, control transfers on both, full
enumeration, interrupt IN on OHCI. A card reader was never seen; a
low-speed A4Tech mouse (09da:c10a) enumerated at once and
`examples/18_usb_mouse` decodes its movement and buttons. The stock GPIO
driver showed A4 = `usb_pwr`, A5 = reset button (the old note calling A4
the reset button was wrong), but on this board the port is powered
regardless of A4.

**Timer bug.** `delay_ms(5000)` took 3.4 s: the plain
`while (now - start < us)` loop ended each 1000 us wait after ~680 us,
while a single long wait on one start value was exact
(`probes/88_timer_glitch`). `delay_us` now accumulates the small steps
between reads and ignores implausible ones; all waits match the PC clock
within 1.5%. This affected every SDK delay, OpenBeken included.

**Pendrive and performance research.** A 1 GB high-speed pendrive
enumerates on EHCI; `usb_msc.c` (bulk-only + SCSI) and `fat.c` read it.
Every file read twice gives identical CRC32s. Timing split per bulk
transfer showed the bus was never the problem (2 MB in ~124 ms); the CPU
copy was:

| Step | memcpy | 2 MB raw read | 524 KB file walk |
|------|--------|---------------|------------------|
| byte memcpy, uncached code | 1.2 MB/s | 1.1 MB/s | 3268 ms |
| word memcpy, cluster-run reads | 7.8 MB/s | 5.1 MB/s | 1339 ms |
| same, code linked cached at 0x80500000 | 64 MB/s | 9.7 MB/s | 107 ms |

Running cached first broke all timing (a "1 s" retry took 0.25 s) and
exposed the real timer bug: back-to-back reads of TC1CNT flip the lowest
count bit (+1, -1, +1 ...; `probes/88_timer_glitch`), with >= 200 nops
between reads they are clean. `micros()` is now monotonic (a read up to 63
counts behind the last one is ignored), which makes every wait exact both
cached and uncached. Caching is safe for USB because the driver touches
its DMA memory only through KSEG1 aliases; the switch driver (`swcore.c`)
still assumes uncached .bss and would need the same treatment before
OpenBeken can run cached.

**HTTP dropped 3 of 20 fetches.** `test_12_http_server` got 17/20.
Replaying its requests with urllib (`tools/http_stress.py` for raw
sockets) showed fetches 18-20 timing out at connect, then recovering ~10 s
after the first request: connection slots were stuck. urllib closes as
soon as it has Content-Length bytes, so its FIN can arrive before it ACKs
ours; `tcp.c` freed a FIN_WAIT connection only when our FIN was acked *and*
the current segment carried a FIN, so in that order the connection sat in
FIN_WAIT_2 until the 10 s idle timeout and 16 of them filled the table.
A `peer_fin` flag now remembers the peer's FIN. After the fix: 172
connections opened and 172 closed, 0 timeouts, 0 dropped; tests 11, 12, 13
pass. OpenBeken uses the same `tcp.c`, so its web UI gets the fix too.

**USB parked (earlier).** `examples/10_usb_probe` holds the RTL8196C init sequence
from the vendor EHCI driver (bit 17 of 0xB8000010, PHY data via
0xB8000034 = 0x1f00|val, strobes at 0xB80210A4, little-endian EHCI
registers) and a minimal control-transfer path, but it has not been run
since the first attempt (which used the wrong, RTL8196D register set).

### Open questions

Answered today: GCC `-march=mips1` code runs (1); the vector is
0x80000080 (2); nibble N routes to IPN (3); the timer clock is 200 MHz /
divisor with the count in bits 31:4 (4). Still open:

1. Does the core interlock the load delay slot? GNU as pads it for mips1,
   so nothing has tested the hazard yet (a deliberate `.set noreorder`
   probe would).
2. Why did the article's author lose characters when polling LSR.THRE? Our
   polled driver has not lost any so far.
3. What are the permanently pending interrupt sources 1, 12, 13, and which
   source is the loader routing at nibble 8?
4. GPIO interrupts (reset button on GPIOA4) and the GPIO source number.
5. Running from the cached alias 0x80500000 with proper CCTL handling, for
   speed.

## 2026-10-15: DHCP, Ethernet OTA, Stage-2, RTOS, and OpenBeken Port

**Factory MAC (`spiflash_factory_mac`) & Flash Write Guard.** Located the
board's factory MAC address (`80:1f:02:ca:ad:92`, Edimax OUI) in the `H601`
settings block at flash offset `0x006007` (`0xBD006007`). Added a hard guard
in `sdk/src/spiflash.c` (`SPIFLASH_PROTECTED_END = 0x010000u`) refusing any
sector/block erase or page program below `0x010000`, plus `spiflash_erase_range()`
which uses 64 KB block erases where aligned and 4 KB sector erases at the
edges.

**Partition Table (`RPT1`), App Image (`RAPP`), Boot State (`BTRY`/`BCNF`), and Stage-2 (`examples/16_stage2`).**
Implemented `sdk/src/flash_layout.c` and `tools/mkimage.py`:
- `cs6c` stage-2 image generation with 16-bit big-endian zero-sum checksum.
- `RPT1` partition table (`boot` at `0x010000`, `state` at `0x01E000`,
  `ptable` at `0x01F000`, `appA` at `0x020000`, `appB` at `0x220000`,
  `lfs` at `0x420000`) with automatic fallback to built-in defaults when
  unformatted.
- `RAPP` 52-byte application image header with separate header and payload
  CRC-32s and monotonic sequence numbers (`seq`).
- Append-only 16-byte boot records (`BTRY`/`BCNF`) in `0x01E000` with
  3-strike automatic fallback to the alternate slot in `stage2_choose_slot()`.

**Non-blocking DHCPv4 Client & Multi-homed Networking (`sdk/src/dhcp.c`, `sdk/src/net.c`).**
Implemented a non-blocking DHCPv4 client (`DISCOVER -> OFFER -> REQUEST -> ACK`)
with `0.0.0.0 -> 255.255.255.255` broadcast support in `net_ip_send()`.
`net.c` now answers ARP and accepts IP/TCP/UDP traffic on *both* the static
bench IP (`192.168.0.196`) and the DHCP-leased IP (`192.168.0.134`), replying
from whichever local IP the peer addressed.

**Ethernet OTA Server (`sdk/src/ota.c`, `examples/15_ota`, `tools/ota.py`).**
Added TCP OTA service on port `9698` (`ROTA` 16-byte header) supporting:
- `--status`: reports partition table source, slot A/B versions & sequence
  numbers, chosen slot, and boot-state record summary.
- `--ram`: stages an image at `0xA0A00000`, verifies `RAPP` CRCs, quiesces
  switch NIC DMA (`CPU_ICR = 0` and `EnablePHYIf` cleared on all ports,
  matching the Realtek bootloader's `J` command so incoming packets never
  DMA into `.bss` while `_start` zeroes it), copies the position-independent
  `ota_reloc_stub` (`sdk/src/exception.S`) to `0xA0400000`, relocates the
  image to `0xA0500000`, flushes I/D caches via CP0 `$20` (`CCTL`), and
  jumps to the new image in ~0.6s–5s (~65 KB/s) without touching UART or
  flash.
- `--test-flash`: writes a `RAPP` image to the unused zero-filled top region
  (`0x7E0000`), commits page 0 (containing `RAPP` magic and bumped `seq`)
  *last*, verifies through the memory-mapped flash window, and restores the
  original zeros.
- `--flash` and `--raw-flash`: ready for production A/B slot programming.

**Lexra RLX4181 Unaligned Load/Store Emulation (`sdk/src/irq.c`).**
Third-party libraries (such as LittleFS and cJSON) occasionally emit
`lwl`/`lwr`/`swl`/`swr` through struct copies or packed pointers. Added
transparent `AdEL`/`AdES` (Cause ExcCode 4 and 5) instruction emulation in
`exception_dispatch()` for `lwl` (`0x22`), `lwr` (`0x26`), `swl` (`0x2A`),
and `swr` (`0x2E`), including branch-delay-slot (`Cause.BD`) target
resolution.

**RTOS, Heap, BSD Sockets, DNS & MQTT (`sdk/src/rtos.c`, `sdk/src/heap.c`, `sdk/src/lwip_sockets.c`, `sdk/src/lwip_mqtt.c`).**
- `heap.c`: 1.5 MB dynamic allocator (`0xA0700000..0xA0880000`) with 8-byte
  alignment, block splitting, and forward/backward coalescing.
- `rtos.c` & `rtos_switch.S`: cooperative FreeRTOS/Beken-compatible task
  scheduler (`xTaskCreate`, `vTaskDelay`, `rtos_create_thread`, software
  timers, mutexes) with MIPS O32 context switching and background network
  polling during yields.
- `lwip_sockets.c` & `tcp.c`: 1-based BSD socket layer (`SOCK_STREAM`,
  `SOCK_DGRAM`, non-blocking `accept`/`recv`/`select`, DNS `A`-record
  resolver, and TCP receive-window flow control `tcp_set_rcv_wnd` for
  multi-hundred-KB streaming HTTP POST uploads).
- `lwip_mqtt.c`: MQTT 3.1.1 client (`CONNECT`, `CONNACK`, `PUBLISH`,
  `PUBACK`, `SUBSCRIBE`, `PINGREQ`) on top of `tcp_connect()`.

**OpenBeken Port (`PLATFORM_RTL8196C`, `tools/build_obk.py`, `tools/test_obk.py`).**
Ported OpenBeken (`C:\GIT\OpenBK7231T_App`) to the RTL8196C:
- Compiles 179 translation units cleanly with `mips-mti-elf-gcc -march=mips1`
  into `build/obk/openbeken_rtl8196c.img` (345 KB).
- Maps all 32 GPIO pins (`PA0..PD7`) and pre-configures the 7 physical
  router LEDs (`PB2..PB5` LAN ports, `PC6` WAN port, `PB7`/`PC0` system
  LEDs) to OpenBeken Channels 1..7 (`IOR_LED_n`).
- Mapped OpenBeken configuration (`mainConfig_t`), retain variables
  (`FLASH_VARS_STRUCTURE`), and the 384 KB LittleFS partition (`0x430000..0x490000`)
  to real SPI flash in the `lfs` partition (`0x420000`), backed by a
  write-through SDRAM cache (`0xA0880000`) plus a clean boot snapshot at
  `0xA0900000`.
- Created the standardized SPI flash installer `tools/flash_obk.py`
  (preserving `0x000000..0x00FFFF` untouched so `<RealTek>` `ESC` recovery
  and `H601`/`6V03` factory MAC/calibration always survive), flashed
  `RPT1` (`0x01F000`), `bootstate` (`0x01E000`), `OpenBeken` (`appA` at
  `0x020000` and `appB` at `0x220000`), and `stage2.cs6c` (`0x010000`).
- Disassembled the LZMA-compressed `<RealTek>` v1.1f root bootloader and
  found that hands-off cold boot from SPI flash skips `eth_startup()`
  entirely; added `swcore_hw_init()` to `sdk/src/swcore.c` to perform the
  full switch ASIC reset, 5-port 10/100 PHY calibration, ASIC table clear,
  VLAN 8 / Netif 0 table creation, and `SW_FFCR = 3` L2 unknown-unicast
  CPU port trap on cold boot.
- Optimized `crc32_update()` in `sdk/src/libc.c` to use 32-bit word reads
  and a 16-entry nibble lookup table (speeding up SPI flash MMAP slot
  validation by ~8x) and refreshed `c->t_last` after `on_data` callbacks
  in `sdk/src/tcp.c`.
- Verified hands-off cold power-cycling (`python tools/flash_obk.py`) and
  all 9 end-to-end hardware tests (`python tools/test_obk.py`) running
  standalone from SPI flash (Web UI on static `192.168.0.196` + DHCP
  `192.168.0.134`, `/api/info` + `/api/pins`, 7 physical router LEDs, UART
  console on `COM5`, LittleFS SPI flash persistence across reboot, Web UI
  HTTP OTA into the alternate A/B slot, UDP NTP time sync, and TCP MQTT
  3.1.1 broker control).


## 2026-10-15 - USB zero-copy, write-back D-cache, write test

- **D-cache is write-back, not write-through** (probes/87_cache_policy, tests
  4-5). The first probe was fooled: CPU reads through KSEG1 see dirty cached
  lines. CCTL `0x001` alone threw away the stack and globals, which looked
  like a USB hang and then `usb_bulk` returning -10 (`dev.present` lost).
  `0x100` = write back, `0x200` = write back + invalidate; both verified.
  Constants `CCTL_DWB`/`CCTL_DWBINVAL` added to `rtl8196c.h`.
- **Zero-copy bulk** in `usb.c`: buffers that are 32-byte aligned, at least
  512 bytes and a multiple of 32 go straight to the controller (DWB before
  OUT, DWBINVAL before and after IN); anything else uses a 512-byte bounce
  buffer. `usb_zero_copy = 0` turns it off. probes/85_usb_zero_copy:
  direct reads over dirty 0xEE lines, at page offsets 0, 0x20 and 0x800 and
  through uncached aliases, all match the bounce reference; 50 x 64 KB
  without error.
- Pendrive read (examples/19, cached): 64 KB reads 9.7 -> **10.5 MB/s**,
  copy time 0 ms (bus-bound). File CRCs unchanged versus the soak log.
- **Write test: this stick cannot store data.** WRITE(10) returns good
  status (0 transport errors). The written sector *and its neighbours* then
  read 0xFF for at least 3 s; after re-enumeration the old contents are back
  (probes/84_usb_write). The same happens through bounce, direct, cached and
  uncached buffers, for 1 to 64 sectors and for zeros. MODE SENSE says not
  write-protected. Together with VID ffff and the weak block at MB 453, the
  flash controller has most likely locked into a read-only failure mode. The
  example 21 run left nothing behind: all 5 files and CRCs match the soak
  log afterwards.
  The write path (`msc_write`, FatFs `diskio_usb.c`) still needs a healthy
  stick for verification.
- New SDK call `msc_write_protected()` (MODE SENSE(6), falls back to (10)).
- **OTA relocation stub** (`exception.S` `ota_reloc_stub`): it issued
  DINVAL|IINVAL after copying the image. It now runs CCTL_DWBINVAL before the
  copy (flush the old program's dirty lines so none lands on the new image)
  and CCTL_DWBINVAL + CCTL_IINVAL after it. Bench: 15_ota (uncached) ->
  `tools/ota.py --ram` of probes/87 built for 0x80500000 and for 0xA0500000:
  both run correctly. The old stub also passed the 0x80500000 case, because
  the cache does not allocate on a write miss and 15_ota never cached those
  lines. The old stub only fails when the running program is itself cached
  at the destination, which is the planned cached-OBK setup.
- **Second stick** (090c:2000 Silicon Motion, 64 GB, USB 2.10): reads at
  **33 MB/s** with 64 KB reads (8 MB/s with 4 KB), against 10.5 MB/s on the
  first stick. probes/84_usb_write (save, test, restore near the end of the
  disk): every write path is correct, from bounce to direct and cached to
  uncached, for 1 to 64 sectors. The original sectors were restored (CRC
  checked), and the CSW residue was 0 on every command. **The write path
  works**; the first stick does not keep writes. The stick is NTFS (MBR type
  07, probes/83_disk_layout), so FatFs cannot mount it as it stands.
- `msc_stats` now counts commands with a non-zero CSW data residue.
- **Read-only NTFS** (`sdk/src/ntfs.c`, examples/22_usb_ntfs). It handles
  MBR, GPT and partitionless volumes, MFT records with fixups, resident and
  non-resident data, sparse runs and uninitialised tails (both read as
  zeros), `$ATTRIBUTE_LIST`, and `$I30` indexes with the in-use bitmap.
  Names are converted from UTF-16 to UTF-8. Compressed and encrypted files
  are refused. There is no libgcc: 64-bit offsets use adds and constant
  shifts plus the `shl64`/`shr64` helpers.
  First run on the 64 GB stick: 13 directories and 106 files (254 MB,
  largest 206 MB) read completely, 0 errors, file data at 13.8 MB/s.
  PC comparison (`tools/fat_crc.py D:\ --compare backups/stick2_ntfs.log`):
  **104 files identical, 0 different**. The other 2 board files are in
  `System Volume Information`, which Windows does not let a normal user list.
