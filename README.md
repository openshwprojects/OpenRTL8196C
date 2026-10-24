# OpenRTL8196C

A from-scratch bare-metal SDK for the Realtek RTL8196C (Lexra RLX4181
MIPS core) as found in cheap routers such as the Cyfrowy Polsat / Edimax
LT-6408n. Programs are compiled on Windows with clang, pushed into RAM
through the stock boot loader's UART console (EW/DW/J commands) and run
without touching the flash.

Inspired by p.kaczmarek2's elektroda.pl series on this router.

## Layout

```
docs/       research notes and the bring-up log
sdk/        startup code, linker script, drivers (uart, irq, timer, gpio), tiny libc/printf
examples/   01_hello 02_counter 03_blink 04_timer_poll 05_timer_irq 06_uart_echo 07_uart_irq
tools/      build.py (clang/lld), flash.py (loader + OpenBeken power cycle), uart_boot_capture.py
```

## Requirements

- LLVM (clang, ld.lld, llvm-objcopy): `winget install LLVM.LLVM`
- Python 3 with `pyserial`
- USB-UART adapter on the router's console (38400 8N1)
- Optional: a Tasmota/OpenBeken mains socket to power-cycle the router

## Workflow

```bash
python tools/build.py examples/01_hello
```

```bash
python tools/flash.py build/01_hello/app.bin
```

`flash.py` power-cycles the socket, catches the loader with ESC, writes the
image with `EW`, verifies with `DW`, jumps with `J` and then relays the UART
to your terminal.

## Documentation

- [docs/01-hardware-notes.md](docs/01-hardware-notes.md): registers and measured hardware facts
- [docs/02-bring-up-log.md](docs/02-bring-up-log.md): what was tried, in order
- [docs/03-roadmap.md](docs/03-roadmap.md): partitions, OTA, RTOS, OpenBeken
- [docs/04-flash.md](docs/04-flash.md): SPI flash, loader image format, backup, writing, partition plan
- [docs/05-wifi-handover.md](docs/05-wifi-handover.md): RTL8192CE Wi-Fi bring-up, AP/STA drivers, and hardware notes
- [docs/06-openbeken-vfs-ftp.md](docs/06-openbeken-vfs-ftp.md): Dual-homed OpenBeken Wi-Fi, unified LittleFS + USB (FAT/exFAT/NTFS) VFS, 10 MB streaming HTTP, FTP (`drv_ftp.c`), & SMB2 (`drv_smb.c`) server benchmarks

## Status (2026-10-17)

Working on the LT-6408n board, all verified over the real UART and Ethernet:

| Example / Target | What it shows |
|------------------|---------------|
| 01_hello | code runs from RAM, dumps CP0 and peripheral registers |
| 02_counter | 1 2 3 4 5 ... on the UART |
| 03_blink | LAN LEDs as GPIO |
| 04_timer_poll | TC0 periodic timer, TC1 free-running microsecond counter |
| 05_timer_irq | timer interrupt through the Lexra vector at 0x80000080 |
| 06_uart_echo | UART receive by polling, `reboot` via the watchdog |
| 07_uart_irq | UART receive by interrupt, one IRQ per byte |
| 08_eth_link | link/speed per switch port, frames seen on the CPU port with their source port |
| 09_eth_ping | ARP + ICMP echo responder: the board answers `ping` from the LAN |
| 11_udp_console | UDP console: peek/poke registers, LEDs, link and stats from the PC, plus UDP echo |
| 12_http_server | web page served from RAM: status, LED buttons, register dump, JSON API (http://192.168.0.196/) |
| 13_multi_socket | HTTP, a telnet-style TCP console, TCP/UDP echo, UDP console and TCP OTA all at once |
| 14_flash_backup | streams the 8 MB SPI flash over TCP port 9697 (`tools/flashdump.py`) |
| 15_ota | DHCPv4 client + HTTP + TCP/UDP console + TCP OTA server on port 9698 (`tools/ota.py`) |
| 16_stage2 | stage-2 A/B partition chooser (`cs6c` image for `0x010000`) |
| OpenBeken (`build_obk.py`) | Full OpenBeken (`PLATFORM_RTL8196C`) firmware running in cached `KSEG0` (`0x80500000` I-cache + D-cache): Dual-homed Ethernet + Wi-Fi (STA & Soft-AP), Web UI, REST API, 7 router LEDs, 32 GPIOs, unified LittleFS + USB (`/usb0..3` FAT/exFAT/NTFS R/W) VFS, streaming HTTP `/api/lfs/`, FTP (`drv_ftp.c`), & SMB2 (`drv_smb.c`, `\\<ip>\obk`) servers, HTTP & TCP OTA (`3.0 MB/s`), NTP, SSDP, Berry, MQTT 3.1.1 + Home Assistant Discovery |
| 10_usb_probe | raw USB register diagnostics (superseded by 17) |
| 17_usb_host | SDK USB host driver: power, PHY, EHCI + OHCI, hotplug, enumeration, descriptors |
| 18_usb_mouse | reads a USB mouse over an interrupt endpoint: movement, buttons and scroll wheel (HID report descriptor parsed by `sdk/src/hid.c`) |
| 19_usb_storage | pendrive / USB disk, read only: SCSI identity and capacity, read speed, partition table, FAT12/16/32 root listing (`usb_msc.c`, `fat.c`) |
| 20_usb_storage_soak | read-only soak: whole disk read 3 times with per-MB CRCs, every file CRC'd (`FILE size crc32 path`, compare with `tools/fat_crc.py`) |
| 21_usb_fatfs | **writes** to a FAT/exFAT stick through FatFs: raw sector test with restore, files in `/rtltest` |
| 22_usb_ntfs | NTFS stick or disk, read only: volume info, full tree walk, every file read and CRC'd (`ntfs.c`) |
| 23_pcie_probe | PCIe bring-up (stock sequence), link training, identifies the Wi-Fi chip from its config space, powers the port down again |
| 24_wifi_fw | Wi-Fi step 1: RTL8192CE power-on and firmware start (needs `tools/extract_wifi_blobs.py` run on your own flash backup; the firmware is never committed) |
| 25_wifi_scan | Wi-Fi step 2: passive scan of channels 1-13, receive only. Lists networks heard (BSSID, SSID, channel, signal, security); same blob requirement as 24 |
| 26_wifi_active | Wi-Fi step 3: active scan. Sends broadcast probe requests (first on-air TX) and collects probe responses + beacons (`sdk/src/wl.c`) |
| 27_wifi_sta | **Working**: Wi-Fi station. Joins a WPA2-PSK (or open) network, 4-way handshake, hardware AES-CCMP, DHCP client, DNS through the router, SDK web page on the Wi-Fi address (`wl_sta.c`, `wpa_crypto.c`). Credentials in git-ignored `wifi_config.h` |
| 28_wifi_ap | Wi-Fi: soft-AP demo, beacons an open test network and answers probe/auth/association |
| 29_wifi_ap_http | **Working**: open Wi-Fi AP with DHCP, captive DNS and the SDK web server at http://192.168.4.1/ (`wl_ap.c`, `dhcpd.c`, `dnsd.c`); a PC joins and gets the page over the air |
| 30_ram_exec | **Working**: SPI NOR Flash XIP (`0xBD`/`0x9D`) vs Uncached SDRAM (`KSEG1` `0xA0500000`) vs Cached SDRAM (`KSEG0` `0x80500000` I-cache + D-cache) execution profiler (`69.9x` over SPI XIP, `43.0x` on WPA2 PBKDF2-SHA1) |

Wi-Fi state, code map, hardware gotchas and the plan for OpenBeken over Wi-Fi: `docs/05-wifi-handover.md`.

The experiments that measured the undocumented bits live in `probes/`
(timer layout, exception vector, interrupt routing, the Ethernet receive
hunt, SPI flash PIO). They build the same way but are not maintained as examples.

## SDK layout

```
sdk/include/board_config.h   MAC, IP, ports: the only per-bench constants
sdk/src/uart.c printf.c      polled UART, tiny printf/snprintf/vsnprintf
sdk/src/timer.c              TC0/TC1, calibrated delay_ms/micros/timer_ms/every_ms, watchdog
sdk/src/irq.c exception.S    Lexra vector trampoline, context save, lwl/lwr/swl/swr emulation, ota_reloc_stub
sdk/src/gpio.c               GPIO + LAN LEDs
sdk/src/spiflash.c           SPI NOR driver (< 0x10000 write guard, erase/program/verify, H601 factory MAC)
sdk/src/flash_layout.c       cs6c, RPT1 partition table, RAPP app header, BTRY/BCNF boot state, stage-2 chooser
sdk/src/swcore.c             switch CPU-port NIC (rings), port status, MDIO
sdk/src/net.c                multi-homed ARP/IP/ICMP/UDP stack (static alias + DHCP IP)
sdk/src/dhcp.c               non-blocking DHCPv4 client
sdk/src/tcp.c                TCP server & client core: 4 listeners, 16 connections, window flow control, retransmit
sdk/src/ota.c                TCP OTA server on port 9698 (status, RAM hot-reload, test-flash, A/B slot flash)
sdk/src/heap.c               1.5 MB dynamic memory allocator (malloc/free/calloc/realloc)
sdk/src/rtos.c rtos_switch.S cooperative FreeRTOS/Beken task scheduler, software timers, mutexes
sdk/src/lwip_sockets.c       BSD / lwIP socket & DNS layer on top of net.c/tcp.c
sdk/src/lwip_mqtt.c          MQTT 3.1.1 client on top of tcp.c
sdk/src/httpd.c              HTTP/1.1 GET/HEAD with routes on top of tcp.c
sdk/src/console.c cli.c      the command set (peek/poke/led/...), string builder, parsers
sdk/src/statuspage.c         the status web pages used by 12, 13 and 15
sdk/src/usb.c                USB host: EHCI + OHCI, hotplug, control/bulk/interrupt transfers
sdk/src/hid.c                HID report descriptor parser (mouse layout incl. wheel)
sdk/src/usb_msc.c            USB mass storage: bulk-only transport + SCSI (read/write sectors)
sdk/src/fat.c                read-only FAT12/16/32 with long file names
sdk/src/ntfs.c               read-only NTFS (MBR/GPT, fragmented and sparse files, attribute lists, UTF-8 names)
sdk/src/pcie.c               PCIe root port: stock bring-up sequence, BAR2 window at 0xB9000000, power down
sdk/src/wl.c                 RTL8192CE Wi-Fi driver: MAC/BB/RF bring-up, firmware, fuse read, TX/RX rings, channel, op-mode
sdk/src/wl_ap.c              soft AP: beacons, probe/auth/assoc, client table, 802.11 <-> Ethernet bridge for net.c
sdk/src/wl_sta.c             station: scan, auth/assoc, WPA2 4-way handshake, CCMP keys in the chip, bridge for net.c
sdk/src/wpa_crypto.c         SHA-1, HMAC, PBKDF2, 802.11i PRF, AES-128, RFC 3394 unwrap (host-tested, tools/host_tests)
sdk/src/dhcpd.c dnsd.c       tiny DHCP server and captive DNS for the AP
sdk/src/diskio_usb.c         FatFs disk glue for USB mass storage (sdk/third_party/fatfs: read/write FAT + exFAT)
sdk/rtl8196c.ld              link script (default 0xA0500000 uncached; 0x80500000 cached KSEG0 for OpenBeken)
```

Tools read `bench.ini` (COM port, socket IP, board IP); `BENCH_PORT`,
`BENCH_BOARD_IP` and friends override it.

Ethernet needs no extra wiring: plug any LAN port into your switch. The
examples print which switch port the cable is on (LAN4 = port 3). With
11_udp_console / 13_multi_socket / 15_ota running, talk to the board from the PC:

```bash
python tools/netcon.py "peek b8003100 8"
python tools/netcon.py --tcp
python tools/netcon.py --tcp-echo 20
```

Hot-reload any image into RAM over Ethernet in ~0.7–5s (no UART or power-cycle needed):

```bash
python tools/ota.py --status
python tools/ota.py --ram build/15_ota/app.img
```

Build, deploy to RAM or SPI flash, and test OpenBeken (`PLATFORM_RTL8196C`):

```bash
python tools/build_obk.py
python tools/ota.py --ram build/obk/openbeken_rtl8196c.img
python tools/flash_obk.py                    # full standardized SPI flash install + cold-boot verification
python tools/flash_obk.py --update-only      # OTA-flash OpenBeken into inactive A/B slot
python tools/flash_obk.py --verify-only      # power-cycle & verify standalone cold boot from SPI flash
python tools/test_obk.py                     # 12-step hardware test suite
```

Run the hardware regression suite with:

```bash
python tools/test_examples.py
```

Measured facts that differ from the public RTL8196E headers: timers count
up with the value in bits 31:4 and a 200 MHz bus clock; interrupt sources
are UART0 = 7, TC0 = 14, TC1 = 15; routing nibble N drives CPU line IPN.
Details in [docs/01-hardware-notes.md](docs/01-hardware-notes.md), the
story in [docs/02-bring-up-log.md](docs/02-bring-up-log.md).
