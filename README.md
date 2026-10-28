# OpenRTL8196C

**Main presentation:** https://www.elektroda.com/rtvforum/topic4185977.html

Previous part of my series:
[Getting started with programming the RTL8196C from a router - flashing, blink, UART](https://www.elektroda.com/rtvforum/topic4179473.html)
([Polish original](https://www.elektroda.pl/rtvforum/topic4179473.html))

OpenRTL8196C gives an old Wi-Fi router a second life. It consists of two parts:

- a bare-metal SDK for the Realtek RTL8196C, written from scratch (no Linux,
  no Realtek SDK code copied) with the help of AI and public RTL8196E sources,
- an [OpenBeken](https://github.com/openshwprojects/OpenBK7231T_App) port
  (`PLATFORM_RTL8196C`) built on that SDK, which turns the router into a
  Home Assistant / MQTT controller and a small USB file server with FTP and
  SMB2 shares.

My test board is the Cyfrowy Polsat / Edimax LT-6408n. I compile on Windows
with clang. During development nothing has to be written to flash: the stock
boot loader takes the image over UART (`EW`/`DW`/`J`), and later builds go
into RAM over Ethernet in a few seconds.

## Hardware

| Part | What is on the LT-6408n |
|------|-------------------------|
| SoC | Realtek RTL8196C |
| CPU | Lexra RLX4181, MIPS-I class, big-endian, no FPU, 390 MHz |
| Caches | 16 KB I-cache + 8 KB D-cache |
| RAM | 32 MB SDRAM |
| Flash | 8 MB SPI NOR (Macronix MX25L6405D) |
| Ethernet | 5-port 10/100 switch (LAN1..4 + WAN) |
| Wi-Fi | Realtek RTL8192CE 2T2R 802.11b/g/n on PCIe |
| USB | one USB 2.0 host port (EHCI + OHCI) |
| Console | UART0, 38400 8N1 |

The Lexra core has no `lwl`/`lwr`/`swl`/`swr`, which is why a stock MIPS32
toolchain does not work out of the box. The SDK builds with `-march=mips1`
and the exception handler emulates those instructions if one slips through.

## OpenBeken on the router

What the port does today (full write-up, benchmarks and all screenshots in
[docs/06-openbeken-vfs-ftp.md](docs/06-openbeken-vfs-ftp.md)):

- **Two network interfaces at once**: Ethernet plus Wi-Fi. If no Wi-Fi network
  is set (or joining it fails), the router opens its own AP,
  `OpenRTL8196C Router`, at `192.168.4.1` with DHCP and captive DNS.
- **Home Assistant**: MQTT 3.1.1 with discovery. The 7 router LEDs are
  exposed as switches; any of the 32 GPIOs can be given a relay role instead.
- **Usual OpenBeken features**: Web UI and Web App, REST API, Berry scripts,
  HTML hosting, NTP, SSDP, HTTP and TCP OTA.
- **USB storage**: LittleFS on the SPI flash and up to four USB drives
  (`/usb0..3`) appear in one file tree. FAT12/16/32 and exFAT (FatFs) and
  NTFS (own driver) can be read and written. The single port takes a
  standard USB hub; I tested two cascaded 4-port hubs with four drives.
- **FTP server** (`drv_ftp.c`): port 21, passive data on 2121, resume,
  rename, delete, mkdir/rmdir.
- **SMB2 server** (`drv_smb.c`): dialects 2.0.2/2.1, shares `\\<ip>\obk`
  (whole tree) and `\\<ip>\usb0..3`, NTLMv2 login and HMAC-SHA256 signing.
  It works with stock Windows 10/11, no insecure guest access needed.
- **Streaming `/api/lfs/`**: files go out in 4 KB chunks, so their size is
  no longer limited by free heap.

The FTP and SMB2 drivers only use OpenBeken's `lfs_*` file API, so they are
not tied to this chip.

### Speed

The biggest speedup came from moving OpenBeken from uncached RAM (`KSEG1`)
to cached RAM (`KSEG0`). Reading a 397,752 B file from the NTFS stick:

| Path | Uncached | Cached |
|------|---------:|-------:|
| HTTP `/api/lfs/` | ~360 KB/s | 3.60 MB/s |
| FTP `RETR` | ~384 KB/s | 2.21 MB/s |
| SMB2 `READ` (signed) | ~327 KB/s | 1.19 MB/s |
| CrystalDiskMark `SEQ1M Q1T1` on the mapped SMB2 drive | ~327 KB/s R / ~600 KB/s W | 3.77 MB/s R / 2.72 MB/s W |

![Four USB drives mapped from the router as SMB2 network drives](screenshots/44_explorer_this_pc_mapped_smb_drives.png)
![The router as an MQTT device in Home Assistant](screenshots/19_ha_device_toggled.png)
![CrystalDiskMark on the mapped SMB2 share](screenshots/47_crystaldiskmark_smb_usb.png)

## Getting started

You need:

- LLVM (clang, ld.lld, llvm-objcopy): `winget install LLVM.LLVM`
- Python 3 with `pyserial`
- a USB-UART adapter on the router's console header (38400 8N1)
- optionally, a Tasmota/OpenBeken smart socket, so the tools can power-cycle the router

Bench settings (COM port, socket IP, board IP) live in `bench.ini`;
`BENCH_PORT`, `BENCH_BOARD_IP` and similar variables override them.

Build an example and run it from RAM over UART:

```bash
python tools/build.py examples/01_hello
```

```bash
python tools/flash.py build/01_hello/app.bin
```

`flash.py` power-cycles the socket, sends ESC until the loader answers,
uploads the image with `EW`, checks it with `DW`, starts it with `J` and then
passes the UART through to your terminal.

Ethernet needs no extra wiring, any LAN port will do; the examples print
which switch port the cable is on (LAN4 = port 3). With 11_udp_console,
13_multi_socket or 15_ota running, you can talk to the board from the PC:

```bash
python tools/netcon.py "peek b8003100 8"
python tools/netcon.py --tcp
python tools/netcon.py --tcp-echo 20
```

Reload any image into RAM over Ethernet, without UART or a power cycle
(about 0.7-5 s):

```bash
python tools/ota.py --status
python tools/ota.py --ram build/15_ota/app.img
```

OpenBeken (`build_obk.py` finds an `OpenBK7231T_App` checkout next to this
repo, or takes `--obk <path>`):

```bash
python tools/build_obk.py
python tools/ota.py --ram build/obk/openbeken_rtl8196c.img
python tools/flash_obk.py                    # full SPI flash install + cold-boot check
python tools/flash_obk.py --update-only      # OTA into the inactive A/B slot
python tools/flash_obk.py --verify-only      # power-cycle and check a cold boot from flash
python tools/test_obk.py                     # 12-step hardware test suite
```

Regression run of the examples on real hardware:

```bash
python tools/test_examples.py
```

The factory loader at the start of flash is never touched (the SPI driver
refuses writes below `0x10000`), so the UART console is always there to fall
back to. Make a full flash backup first (`14_flash_backup` +
`tools/flashdump.py`): it is your way back to the stock firmware, and the
Wi-Fi examples need it, because the RTL8192CE firmware is extracted from it
by `tools/extract_wifi_blobs.py` and never committed.

## Examples

Status as of 2026-10-23; everything was checked on the LT-6408n over its
real UART, Ethernet, USB and Wi-Fi.

| Example | What it does |
|---------|--------------|
| 01_hello | proves code runs from RAM, prints CP0 and peripheral registers |
| 02_counter | counts 1 2 3 4 5 ... on the UART |
| 03_blink | blinks the LAN LEDs driven as GPIO |
| 04_timer_poll | TC0 periodic timer and a free-running microsecond counter on TC1 |
| 05_timer_irq | timer interrupt through the Lexra vector at 0x80000080 |
| 06_uart_echo | polled UART receive, `reboot` through the watchdog |
| 07_uart_irq | interrupt-driven UART receive, one IRQ per byte |
| 08_eth_link | link state and speed per switch port, received frames with their source port |
| 09_eth_ping | answers ARP and ICMP echo, so the board can be pinged from the LAN |
| 10_usb_probe | raw USB register dump (replaced by 17) |
| 11_udp_console | UDP console for peek/poke, LEDs, link and statistics, plus UDP echo |
| 12_http_server | web page from RAM with status, LED buttons, register dump and a JSON API |
| 13_multi_socket | HTTP, a telnet-style TCP console, TCP/UDP echo, UDP console and TCP OTA at the same time |
| 14_flash_backup | sends the whole 8 MB SPI flash over TCP port 9697 (`tools/flashdump.py`) |
| 15_ota | DHCPv4 client, HTTP, TCP/UDP console and a TCP OTA server on port 9698 (`tools/ota.py`) |
| 16_stage2 | second-stage boot loader choosing the A/B slot (`cs6c` image at `0x010000`) |
| 17_usb_host | USB host: power, PHY, EHCI + OHCI, hotplug, enumeration, descriptors |
| 18_usb_mouse | USB mouse over an interrupt endpoint: movement, buttons and wheel (HID report descriptor parsed by `sdk/src/hid.c`) |
| 19_usb_storage | read-only USB stick or disk: SCSI identity and size, read speed, partition table, FAT12/16/32 root listing |
| 20_usb_storage_soak | read-only soak test: the whole disk read 3 times with per-MB CRCs, every file CRC'd (compare with `tools/fat_crc.py`) |
| 21_usb_fatfs | **writes** to a FAT/exFAT stick through FatFs: raw sector test with restore, files in `/rtltest` |
| 22_usb_ntfs | read-only NTFS: volume info, full tree walk, every file read and CRC'd |
| 23_pcie_probe | PCIe bring-up with the stock sequence, link training, Wi-Fi chip identified from config space, port powered down again |
| 24_wifi_fw | Wi-Fi step 1: RTL8192CE power-on and firmware start (needs the blobs from your own flash backup) |
| 25_wifi_scan | Wi-Fi step 2: passive scan of channels 1-13, lists BSSID, SSID, channel, signal and security |
| 26_wifi_active | Wi-Fi step 3: active scan, the first transmission (broadcast probe requests) |
| 27_wifi_sta | Wi-Fi client: joins a WPA2-PSK or open network, 4-way handshake, hardware AES-CCMP, DHCP, DNS; credentials in git-ignored `wifi_config.h` |
| 28_wifi_ap | soft-AP demo: beacons an open network, answers probe/auth/association |
| 29_wifi_ap_http | open AP with DHCP, captive DNS and the SDK web page at http://192.168.4.1/ |
| 30_ram_exec | compares running code from SPI flash (XIP), uncached RAM and cached RAM: 69.9x faster than XIP in a tight loop, 43.0x on WPA2 PBKDF2-SHA1 |
| 30_usb_hub_msc | USB hub with up to 4 storage drives mounted at once as `/usb0..3` (FatFs or NTFS), interleaved cross-drive read check |

`probes/` holds the one-off experiments that pinned down the undocumented
parts (timer layout, exception vector, interrupt routing, Ethernet receive,
SPI flash PIO, cache policy). They build the same way but are not maintained.

## Repository layout

```
docs/         hardware notes, bring-up log, roadmap, flash layout, Wi-Fi and OpenBeken notes
sdk/          startup code, linker script, drivers, TCP/IP, USB, PCIe, Wi-Fi, RTOS shim, small libc
examples/     01_hello .. 30_usb_hub_msc
probes/       hardware experiments
screenshots/  OpenBeken Web UI, Home Assistant, FTP and SMB2
tools/        build, UART and OTA loaders, flash backup, OpenBeken build/flash/test, bench helpers
```

SDK sources:

```
sdk/include/board_config.h   MAC, IP and ports: the only per-bench constants
sdk/src/start.S              entry after the loader's J: stack, cache invalidate, .bss clear
sdk/src/libc.c softfp.c      MIPS-I C runtime (byte-wise memcpy etc.), software float and 64-bit math
sdk/src/uart.c printf.c      polled UART, small printf/snprintf/vsnprintf
sdk/src/timer.c              TC0/TC1, calibrated delay_ms/micros/timer_ms/every_ms, watchdog
sdk/src/irq.c exception.S    Lexra vector trampoline, context save, lwl/lwr/swl/swr emulation, ota_reloc_stub
sdk/src/gpio.c               GPIO and LAN LEDs
sdk/src/spiflash.c           SPI NOR: erase/program/verify, write guard below 0x10000, H601 factory MAC
sdk/src/flash_layout.c       cs6c, RPT1 partition table, RAPP app header, BTRY/BCNF boot state, stage-2 chooser
sdk/src/swcore.c             switch CPU-port NIC (rings), port status, MDIO
sdk/src/net.c                multi-homed ARP/IP/ICMP/UDP (static alias + DHCP address)
sdk/src/dhcp.c               non-blocking DHCPv4 client
sdk/src/tcp.c                TCP server and client: 4 listeners, 16 connections, window flow control, retransmit
sdk/src/ota.c                TCP OTA on port 9698: status, RAM hot reload, test flash, A/B slot flash
sdk/src/heap.c               1.5 MB heap (malloc/free/calloc/realloc)
sdk/src/rtos.c rtos_switch.S cooperative scheduler with a FreeRTOS/Beken-style API, software timers, mutexes
sdk/src/lwip_sockets.c       BSD/lwIP-style sockets and DNS on top of net.c and tcp.c
sdk/src/lwip_mqtt.c          MQTT 3.1.1 client
sdk/src/lwip_mdns.c          mDNS / DNS-SD responder (<hostname>.local, _http._tcp) on Ethernet and Wi-Fi
sdk/src/httpd.c              HTTP/1.1 GET/HEAD with routes
sdk/src/console.c cli.c      command set (peek/poke/led/...), string builder, parsers
sdk/src/statuspage.c         status pages used by examples 12, 13 and 15
sdk/src/usb.c                USB host: EHCI + OHCI, hubs, hotplug, control/bulk/interrupt transfers
sdk/src/hid.c                HID report descriptor parser (mouse layout including the wheel)
sdk/src/usb_msc.c            USB mass storage: bulk-only transport + SCSI sector read/write
sdk/src/fat.c                read-only FAT12/16/32 with long file names
sdk/src/ntfs.c               NTFS: MBR/GPT, fragmented and sparse files, attribute lists, UTF-8 names; create/write/truncate/remove for the OpenBeken VFS
sdk/src/diskio_usb.c         FatFs glue for USB storage (sdk/third_party/fatfs: FAT + exFAT read/write)
sdk/src/pcie.c               PCIe root port: stock bring-up sequence, BAR2 window at 0xB9000000, power down
sdk/src/wl.c                 RTL8192CE driver: MAC/BB/RF bring-up, firmware, fuses, TX/RX rings, channel, op mode
sdk/src/wl_ap.c              soft AP: beacons, probe/auth/assoc, client table, 802.11 <-> Ethernet bridge
sdk/src/wl_sta.c             station: scan, auth/assoc, WPA2 4-way handshake, CCMP keys in the chip, bridge
sdk/src/wpa_crypto.c         SHA-1, HMAC, PBKDF2, 802.11i PRF, AES-128, RFC 3394 unwrap (host-tested in tools/host_tests)
sdk/src/dhcpd.c dnsd.c       small DHCP server and captive DNS for the AP
sdk/rtl8196c.ld              linker script: 0xA0500000 uncached by default, 0x80500000 cached KSEG0 for OpenBeken
```

## Documentation

- [docs/01-hardware-notes.md](docs/01-hardware-notes.md): registers and measured hardware facts
- [docs/02-bring-up-log.md](docs/02-bring-up-log.md): the bring-up, step by step
- [docs/03-roadmap.md](docs/03-roadmap.md): partitions, OTA, RTOS, OpenBeken
- [docs/04-flash.md](docs/04-flash.md): SPI flash, loader image format, backup, writing, partition plan
- [docs/05-wifi-handover.md](docs/05-wifi-handover.md): RTL8192CE bring-up, AP/STA drivers, hardware notes
- [docs/06-openbeken-vfs-ftp.md](docs/06-openbeken-vfs-ftp.md): OpenBeken networking, USB/LittleFS VFS, HTTP streaming, FTP and SMB2 servers, benchmarks

A few measured facts differ from the public RTL8196E headers. The timers
count up, keep their value in bits 31:4 and run from a 200 MHz bus clock.
The interrupt sources are UART0 = 7, TC0 = 14 and TC1 = 15, and routing
nibble N drives CPU line IPN. Details are in
[docs/01-hardware-notes.md](docs/01-hardware-notes.md).

## Sources that helped

- Realtek RTL8196E BSP: https://github.com/yshifeng41/rtl819x
- independent firmware for the TOTOLINK N150RT (RTL8196E): https://github.com/Jhongwe1/router-customFW
- RTL8196E Linux port: https://github.com/shibajee/linux-rtl8196e
- Lexra LX4189 datasheet: https://doc.lagout.org/electronics/doc/lexra/lx4189-datasheet-1.9-c20081030.pdf
- OpenWrt wiki on the Realtek boot loader: https://oldwiki.archive.openwrt.org/doc/techref/bootloader/realtek
