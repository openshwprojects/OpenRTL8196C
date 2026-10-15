# Roadmap

Target: run OpenBeken on the RTL8196C router, with this SDK as its HAL.
Agreed 2026-10-14, in this order, each step tested on hardware before
the next.

1. **Full flash backup** over Ethernet (`examples/14_flash_backup`,
   `tools/flashdump.py`). Keep the file; it is the way back to stock.
   DONE 2026-10-14, plus a verified SPI erase/program driver.
2. **Partition table and stage-2 chooser.** Layout (8 MB MX25L6405):

   | Offset | Size | Content |
   |--------|------|---------|
   | 0x000000 | 64 KB | factory loader, never touched (ESC = emergency console) |
   | 0x010000 | 56 KB | stage-2: picks the newest valid app slot, boot-count fallback |
   | 0x01E000 | 4 KB | boot state (append-only "trying"/"confirmed" records) |
   | 0x01F000 | 4 KB | partition table (magic, version, CRC, entries) |
   | 0x020000 | 2 MB | app slot A |
   | 0x220000 | 2 MB | app slot B |
   | 0x420000 | ~3.9 MB | LittleFS |

   Sizes live in the table; the stage-2 and the app read it. Only the
   loader and the stage-2 address are fixed. Full design and the
   loader's image format: [04-flash.md](04-flash.md).
   DONE 2026-10-15 (`sdk/include/flash_layout.h`, `sdk/src/flash_layout.c`,
   `examples/16_stage2`, `tools/mkimage.py`).
3. **OTA over TCP**: the app receives an image into RAM, verifies header
   and CRC, erases and programs the inactive slot through the SPI
   controller, bumps the sequence number, reboots. Plus an OTA sample and
   `tools/ota.py`. DHCP goes in before this.
   DONE 2026-10-15 (`sdk/src/dhcp.c`, `sdk/src/ota.c`, `examples/15_ota`,
   `tools/ota.py` supporting `--status`, `--ram`, `--test-flash`, `--flash`,
   and `--raw-flash`, plus OpenBeken Web UI `POST /api/ota`).
4. **LittleFS** on the remaining space, served by the console and the web
   server.
   DONE 2026-10-15 (integrated into OpenBeken `/api/lfs/*` and `autoexec.bat`).
5. **RTOS**: cooperative FreeRTOS-compatible multi-tasking scheduler,
   MIPS-I context switch (`sdk/src/rtos_switch.S`, `sdk/src/rtos.c`),
   software timers, mutexes, and 1.5 MB heap allocator (`sdk/src/heap.c`).
   DONE 2026-10-15.
6. **OpenBeken port** (`PLATFORM_RTL8196C`): full OpenBeken build and HAL
   (`C:\GIT\OpenBK7231T_App\src\hal\rtl8196c\hal_rtl8196c.c`,
   `sdk/include/obk_shim/`, `sdk/src/lwip_sockets.c`, `sdk/src/lwip_mqtt.c`,
   `tools/build_obk.py`, `tools/test_obk.py`).
   DONE 2026-10-15 and verified on hardware (Web UI, REST API, 7 router LEDs,
   32 GPIO pins, UART console, LittleFS, soft-reboot, Web UI HTTP OTA,
   TCP OTA on port 9698, NTP, and MQTT 3.1.1).

Rules: never write below 0x10000; first flash write of any step needs an
explicit go from the owner; `backups/` is git-ignored and must be kept.
