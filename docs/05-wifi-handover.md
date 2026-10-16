# Wi-Fi on the RTL8196C: handover notes

State as of 2026-10-17. Written so the next person (or agent) can continue
without re-deriving anything. The chronological detail is in
`02-bring-up-log.md`; register facts are in `01-hardware-notes.md` 6c.

## 1. What works, verified on the bench

| Capability | Example | Evidence |
|------------|---------|----------|
| PCIe link, chip identified (RTL8192CE 2T2R, `10ec:8191`) | `23_pcie_probe` | config space, SYS_CFG bit 27 |
| Firmware running in the chip's 8051 | `24_wifi_fw` | MCUFWDL WINTINI_RDY in 5 ms |
| Passive scan | `25_wifi_scan` | 7 networks, SSIDs, RSSI |
| Active scan (first on-air TX) | `26_wifi_active` | probe requests released by the TX engine |
| Soft AP + DHCP + captive DNS + HTTP | `29_wifi_ap_http` | a Windows PC joined, got 192.168.4.2, loaded the page |
| WPA2-PSK station + DHCP + DNS + HTTP | `27_wifi_sta` | handshake 671 ms, lease from the D-Link, example.com resolved, page fetched from the PC over the link |

Both roles are **receive and transmit, hardware AES-CCMP, bridged into the
SDK's own IP stack** (`net.c`/`tcp.c`/`httpd.c`). Nothing OBK yet.

## 2. Code map

```
sdk/src/pcie.c        root port bring-up (stock sequence), BAR2 window 0xB9000000
sdk/src/wl.c          chip driver: power-on, LLT, rings, firmware, MAC/BB/RF tables,
                      channel, TX (chained ring), RX poll, op-mode/BSSID, beacon queue
sdk/src/wl_ap.c       soft AP: beacons, probe/auth/assoc responder, client table,
                      802.11 <-> Ethernet bridge exposed as a net_link
sdk/src/wl_sta.c      station: scan, auth/assoc, EAPOL 4-way handshake, key table,
                      CCMP header handling, bridge as a net_link
sdk/src/wpa_crypto.c  SHA-1, HMAC, PBKDF2, 802.11i PRF, AES-128, RFC 3394 unwrap
sdk/src/dhcpd.c       tiny DHCP server (AP side)
sdk/src/dnsd.c        captive DNS (every A query -> our address)
sdk/src/net.c         gained a pluggable link layer: net_set_link() before net_init()
tools/extract_wifi_blobs.py   firmware + register tables out of the user's flash backup
tools/host_tests/wpa_crypto_test.c   clang host test, all vectors pass
```

The firmware and tables are Realtek's, taken from the user's own stock flash
dump. They are generated into `examples/*/wifi_blobs.inc` (git-ignored) by

    python tools/extract_wifi_blobs.py backups/flash_20261014_222426.bin

Run that once after a fresh checkout or the Wi-Fi examples will not build.

## 3. How to run things

- Build: `python tools/build.py examples/27_wifi_sta` (uncached load 0xA0500000
  is the default; all Wi-Fi work so far ran uncached).
- Flash + run: `python tools/flash.py build/27_wifi_sta/app.bin --listen 120 --log backups/x.log`.
  Pipe through `Select-String` to keep the output small; the raw UART always
  goes to the `--log` file.
- Station credentials: `examples/27_wifi_sta/wifi_config.h` (git-ignored).
  The user pastes them. **Do not write or echo the passphrase**: a safety rule
  blocked that in this session and it stays blocked.
- A board stuck in a long-running example reboots into the loader when it
  receives ESC on the UART (`uart_esc_reboot_poll()` in the examples), so
  `flash.py` can restart it even without a power cycle.
- The OBK smart socket's firmware can crash and then *report* OFF while the
  relay stays ON. Check: send `Power OFF`, read COM5 for 5 s; bytes = still
  powered. The user reboots the plug.

## 4. Hard-won hardware facts (details in 01-hardware-notes.md)

1. **D-cache is write-back.** CCTL 0x001 discards dirty lines; use 0x200
   (write back + invalidate). DMA buffers: write back before the device
   reads, invalidate after it writes. This bit the USB driver first.
2. **TX descriptors must be chained** (word 10 = next descriptor); RX
   descriptors are 32 bytes, TX 64. Mixing the structs broke RX.
3. **RCR needs ADF** to receive data frames (management only otherwise).
4. **Hardware AP mode stalls TX unless BCN_INTERVAL is set and a beacon
   descriptor is armed.** The beacon descriptor is persistent (OWN never
   clears); refill after BCNINT (HISR bit 13).
5. **TSF divider register (0x55C) uses only 6 bits; the MAC clock is 40 MHz**
   -> 0x28 for a 1 us TSF. 0x50 made the TSF run 2.5x fast.
6. **The chip strips the CCMP MIC on receive** but leaves the 8-byte CCMP
   header. Subtracting the MIC a second time cost a day of DHCP debugging.
7. **MACTXEN (CR bit 6) must stay set** or the MAC receives nothing.
8. Interrupt status registers are 0x120-0x12C, not 0x44 (that is GPIO).
9. TX descriptor values that are proven on the air: rate_id 0xC,
   w4 = hwseq_en | use_rate | 0x1F<<16, w5 = 0, no BMC bit. The
   "textbook" layout from trx.h is in `wl.c` under `#if 0`, unverified.

## 5. Open issues, with the best current lead

- **Hardware (TBTT) beacons are not seen by clients.** HISR shows TBDOK, the
  TBTT counter runs, but no PC/phone lists the SSID; software beacons from
  the normal queue (every 100 ms, `wl_ap_sw_beacons`) are what clients
  accept. Lead: compare the beacon-queue descriptor against rtl_pci's
  beacon tasklet field by field (pkt_offset, the `offset` field for the
  beacon queue, rate), or sniff the air with a PC adapter in monitor mode.
  This PC has no Wi-Fi adapter.
- **24 s PMK derivation** (PBKDF2 x4096) because the image runs uncached.
  Options: build cached (`--load 0x80500000 --stack 0x80F00000`; the USB
  code already handles the cache rules, the Wi-Fi code uses uncached
  KSEG1 buffers so it should be safe but is untested), or cache the PMK.
- **Diagnostics still in the code**: `wl_sta_trace`, `wl_ap_trace`,
  the RCR AICV bit under the trace flag, the `by name` probe counter.
  Harmless; prune when stable.
- Not implemented: TKIP (WPA1), WEP, 802.11n rates (everything is sent at
  1 Mbit CCK), power save, roaming, multiple BSS, A-MSDU/A-MPDU, hidden-SSID
  association, deauth handling beyond rescanning.
- `wl_hw_set_opmode(STA)` sets MSR but the station did not need the RCR
  CBSSID filters tuned; if other-AP traffic ever floods the ring, enable
  them after association.

## 6. Next step the user asked for: OpenBeken over Wi-Fi

OBK only calls `socket()`. In this port those sockets are backed by the
SDK's `lwip_sockets.c` -> `tcp.c`/`net.c`, and `net.c` now accepts any
`struct net_link`. So:

1. Decide the mode: OBK as **station** on the user's network (reuse
   `wl_sta.c`: start it, `net_set_link(wl_sta_link())`, then OBK's DHCP
   client as today over Ethernet), or OBK as **AP** with the panel at
   192.168.4.1 (`wl_ap.c` + `dhcpd.c` + `dnsd.c`, as in example 29).
2. Confirm which stack OBK's `socket()` uses in `tools/build_obk.py` /
   the OBK platform layer; if it is this SDK's `net.c`, the plug-in point
   is `net_set_link()` before `net_init()`.
3. The Ethernet switch and Wi-Fi are exclusive today (one link). A
   two-interface `net.c` (route by destination, two ARP tables) is needed
   if OBK must keep Ethernet and Wi-Fi at once.
4. Firmware blobs must be linked into the OBK image too (`wifi_blobs.inc`),
   and the Wi-Fi code must run inside OBK's cooperative scheduler: call
   `net_poll()`/`wl_sta_poll()`/`wl_ap_poll()` from its main loop or a task.
5. Credentials: OBK already stores SSID/password in its config; pass them to
   `wl_sta_start()`.

## 7. Working style that paid off

- Every unknown was resolved by a probe program with printed counters, not
  by reasoning; when a client's behaviour mattered, the "probe requests
  naming our SSID" counter told whether anyone was even trying.
- Linux rtlwifi (git.kernel.org, raw GitHub rate-limits) is the reference;
  no code was copied, sequences were re-derived and then measured.
- Host-test the crypto before it touches the board (clang is at
  `C:\Program Files\LLVM\bin\clang.exe`).
- Confirm the user is actually at the client device before a join test;
  several 5-15 minute runs proved nothing because nobody was trying.
