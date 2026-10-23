# OpenBeken Wi-Fi, Unified USB/LittleFS VFS, Streaming HTTP & FTP Server

State as of 2026-10-17 (`seq=35`). Documents the dual-homed OpenBeken Wi-Fi
integration, the unified LittleFS + USB (FAT16/FAT32/exFAT/NTFS) Virtual
Filesystem (`hal_vfs_rtl8196c.c`), zero-RAM-buffering HTTP/VFS streaming, the
cross-platform OpenBeken FTP server driver (`drv_ftp.c`), and the benchmark
results on the Edimax / Cyfrowy Polsat LT-6408n (Realtek RTL8196C + RTL8192CE).

---

## 1. Dual-Homed OpenBeken Networking (Ethernet + Wi-Fi STA / Soft-AP)

OpenBeken (`PLATFORM_RTL8196C`) runs dual-homed on Ethernet (`swcore.c`) and
802.11b/g/n Wi-Fi (`wl.c`, `wl_sta.c`, `wl_ap.c`):

- **Simultaneous Ethernet + Wi-Fi**: `sdk/src/net.c` maintains separate ARP and
  interface state for the Ethernet switch (`192.168.0.134` DHCP /
  `192.168.0.196` fallback) and the secondary `wifi_link` interface.
- **Automatic STA / Soft-AP Mode**:
  - When Wi-Fi credentials are configured in OpenBeken (`cfg_wifi`), the router
    associates via WPA2-PSK (`wl_sta.c`, hardware AES-CCMP) and obtains a DHCP
    lease.
  - When no SSID is configured (or STA association fails), it automatically
    starts the open Soft-AP (`SSID="OpenRTL8196C Router"`, channel 7,
    `192.168.4.1/24`) with built-in DHCP (`dhcpd.c`) and captive DNS (`dnsd.c`).
- **Wi-Fi Site Survey (`cfg_wifi?scan=1`)**: Active scan across channels 1–13
  populates OpenBeken's Web UI network scanner table with SSID, BSSID, channel,
  RSSI, and encryption type.
- **Home Assistant & MQTT 3.1.1**: Verified end-to-end with Mosquitto and Home
  Assistant auto-discovery (7 router LED channels + status telemetry).

---

## 2. Unified VFS (`hal_vfs_rtl8196c.c` / `hal_vfs_rtl8196c.h`)

### Architecture

OpenBeken natively uses LittleFS (`lfs_t lfs`, `lfs_file_open`,
`lfs_file_read`, `lfs_file_write`, `lfs_dir_open`, etc.) for all file
operations. On RTL8196C, `hal_vfs_rtl8196c.h` transparently redirects the
`lfs_*` API at compile time to `obk_vfs_*` in `hal_vfs_rtl8196c.c`:

| Path Prefix | Backing Store | Filesystem Driver | Capabilities |
|-------------|---------------|-------------------|--------------|
| `/` (default) | 8 MB SPI NOR Flash (`0x320000`, 512 KB) | LittleFS (`lfs.c`) | Full R/W, mkdir, remove, rename |
| `/usb0` .. `/usb3` | USB 2.0 EHCI/OHCI Mass Storage (up to 4 drives via hubs) | FatFs (`ff.c`) | FAT12 / FAT16 / FAT32 / exFAT full R/W, mkdir, remove, rename |
| `/usb0` .. `/usb3` | USB 2.0 EHCI/OHCI Mass Storage (NTFS partitions) | `sdk/src/ntfs.c` | NTFS full R/W (resident & non-resident data runs, cluster allocation, `$Bitmap`, INDX B-tree insertion/removal, create, overwrite, delete, mkdir, rename) |

Reading the root directory (`/`) returns all SPI Flash LittleFS files plus
virtual directory entries (`usb0`, `usb1`, `usb2`, ...) for every currently
mounted USB drive.

### Zero-Copy / Chunked HTTP `/api/lfs/` Streaming

Previously, `http_fns.c` buffered entire files in heap RAM during `GET /api/lfs/`
and `POST /api/lfs/` requests, limiting file sizes to available heap (< 1.5 MB).

- **`GET /api/lfs/<path>`**: Streams files directly from `lfs_file_read()` to
  `send()` in a 4 KB loop with backpressure handling (`EWOULDBLOCK` retry +
  `rtos_delay_milliseconds(1)`).
- **`POST /api/lfs/<path>`**: Streams incoming HTTP request body chunks directly
  from the TCP socket into `lfs_file_write()` using a 4 KB buffer as packets
  arrive over the wire.
- **RAM Footprint**: **4 KB static/stack chunk buffer** (0 bytes of file-size
  heap allocation), enabling multi-megabyte and gigabyte file transfers while
  Free Heap stays constant at ~1.42–1.49 MB.

---

## 3. Cross-Platform OpenBeken FTP Server (`drv_ftp.c` / `drv_ftp.h`)

### Design

Implemented in `src/driver/drv_ftp.c` and `src/driver/drv_ftp.h` (`ENABLE_DRIVER_FTP`):

- **100% Platform-Agnostic & DRY**: Built strictly on OpenBeken's standard
  `lfs_*` VFS API and BSD/lwIP non-blocking sockets. Works out-of-the-box on any
  OpenBeken platform with LittleFS, and automatically exposes `/usb0..3` on
  RTL8196C through the VFS shim.
- **Non-Blocking Cooperative State Machine**: Runs inside `DRV_FTP_RunQuickTick()`
  without spawning extra RTOS threads:
  - Control listener on port `21` (configurable via `ftp_port <port>`).
  - Fixed passive data listener on port `2121` (`PASV` / `EPSV`) plus active
    mode (`PORT`) support.
  - Transfers (`FTP_XFER_RETR` and `FTP_XFER_STOR`) step up to 16 chunks × 4 KB
    (64 KB) per tick, yielding immediately on `EWOULDBLOCK` so HTTP, MQTT, and
    Wi-Fi polling remain responsive.
- **Supported Commands**:
  - Session & Auth: `USER`, `PASS`, `ACCT`, `REIN`, `QUIT`, `NOOP`, `SYST`,
    `FEAT`, `OPTS`, `TYPE`, `MODE`, `STRU` (`ftp_auth <user> <pass>` or
    anonymous/any login by default).
  - Navigation: `PWD`/`XPWD`, `CWD`/`XCWD`, `CDUP`/`XCUP`.
  - Listing & Metadata: `LIST` (`-la` flag handling), `NLST`, `SIZE`, `MDTM`.
  - Transfers & Resume: `PASV`, `EPSV`, `PORT`, `REST`, `RETR`, `STOR`, `APPE`,
    `ABOR`.
  - File & Directory Management: `DELE`, `MKD`/`XMKD`, `RMD`/`XRMD`,
    `RNFR`/`RNTO` (backed by `lfs_rename` / `obk_vfs_rename`).

---

## 4. Benchmarks & Stress Test Results

Tested on the real LT-6408n router (`390 MHz` Lexra RLX4181, 100 Mbps Ethernet,
USB 2.0 EHCI high-speed hub with 3 drives attached simultaneously:
`/usb0` 58 GB NTFS, `/usb1` 1 GB FAT16, `/usb2` 1 GB FAT32).

### A. 10 MB JPEG VFS & HTTP `/api/lfs/` Streaming Profile

Test file: `10,485,760` bytes (10.0 MB JPEG, `SHA256 = 74a10604363ffdcb5285b4a13948a4d7aebe6811a80402364325655953b87290`).

| Operation | Target | Avg Time | Throughput | Free Heap Before / After | Integrity |
|-----------|--------|---------:|-----------:|--------------------------|-----------|
| Raw VFS Write (10 MB) | `/usb0` (NTFS) | 9.57 s | **1,070.9 KB/s** | 1,490,224 B / 1,490,224 B | 100% |
| Raw VFS Read (10 MB) | `/usb0` (NTFS) | 2.13 s | **4,807.8 KB/s** | 1,490,224 B / 1,490,224 B | CRC32 `0xA6ED7B52` |
| Raw VFS Write (10 MB) | `/usb1` (FAT16) | 12.16 s | **842.0 KB/s** | 1,490,224 B / 1,490,224 B | 100% |
| Raw VFS Read (10 MB) | `/usb1` (FAT16) | 1.98 s | **5,185.8 KB/s** | 1,490,224 B / 1,490,224 B | CRC32 `0xA6ED7B52` |
| HTTP `GET /api/lfs/` (10 MB × 5 runs) | `/usb0` (NTFS) | 28.42 s | **360.3 KB/s** | 1,490,224 B (0 B delta) | 5/5 SHA-256 match |
| HTTP `GET /api/lfs/` (10 MB × 5 runs) | `/usb1` (FAT16) | 28.45 s | **359.9 KB/s** | 1,490,224 B (0 B delta) | 5/5 SHA-256 match |
| HTTP `GET` 20× 1 MB Stress (20 MB total) | `/usb0` + `/usb1` | 55.20 s | **362.3 KB/s** | 1,490,224 B (0 B leaked) | 20/20 SHA-256 match |

### B. End-to-End FTP (`drv_ftp.c`) Benchmark (`seq=35`)

All operations executed in a single continuous FTP control session while Free
Heap stayed constant at ~1.42 MB:

| Target Filesystem | FTP Operation | Payload Size | Elapsed Time | Throughput | Verification |
|-------------------|---------------|-------------:|-------------:|-----------:|--------------|
| Root VFS (`/`) | `LIST /` | 5 entries | < 10 ms | — | LittleFS files + virtual `usb0`, `usb1`, `usb2` |
| LittleFS (`/`) | `STOR`, `SIZE`, `RETR`, `RNFR`/`RNTO`, `DELE` | 3,400 B | < 50 ms | — | Byte-for-byte match, `250 Rename successful.` |
| `/usb0` (NTFS) | `LIST /usb0` | 15 entries | < 20 ms | — | Full NTFS directory listing |
| `/usb0` (NTFS) | `STOR /usb0/ftp10m.jpg` | 10,485,760 B | **9.81 s** | **1,044.2 KB/s** | `226 Transfer complete.`, `SIZE` = `10485760` |
| `/usb0` (NTFS) | `RETR /usb0/ftp10m.jpg` | 10,485,760 B | **26.68 s** | **383.8 KB/s** | `226 Transfer complete.`, **SHA-256 match** |
| `/usb1` (FAT16) | `MKD`, `STOR`, `RNFR`/`RNTO`, `RETR`, `DELE`, `RMD` | Dir + file | < 50 ms | — | `250 Rename successful.`, `250 Delete operation successful.` |
| `/usb1` (FAT16) | `STOR /usb1/ftp10m.jpg` | 10,485,760 B | **14.70 s** | **696.4 KB/s** | `226 Transfer complete.`, `SIZE` = `10485760` |
| `/usb1` (FAT16) | `RETR /usb1/ftp10m.jpg` | 10,485,760 B | **30.75 s** | **333.0 KB/s** | `226 Transfer complete.`, **SHA-256 match** |
| `/usb2` (FAT32) | `LIST /usb2` | 5 entries | < 20 ms | — | Lists files & directories cleanly |

---

## 5. Key SDK Networking & Socket Fixes Discovered During Stress Testing

1. **Synchronous `on_tcp_event(TCP_EV_CLOSED)` in `lwip_close()` (`sdk/src/lwip_sockets.c`)**:
   - Calling `tcp_close(c)` on a connection in `SYN_RCVD` or `SYN_SENT`
     immediately invokes `conn_free(c, 1)` -> `on_tcp_event(TCP_EV_CLOSED)`,
     which zeroes `s->conn = 0`.
   - `lwip_close()` previously called `tcp_set_user(s->conn, 0)` *after*
     `tcp_close(s->conn)`, dereferencing `NULL` (`0x00000000` + offset) when
     closing an unestablished connection. Fixed by detaching `tcp_conn_t *c = s->conn; s->conn = 0;`
     and clearing `tcp_set_user(c, 0)` *before* calling `tcp_close(c)`.
2. **Recursive `net_poll()` / `tcp_poll()` Storm on Uncached ARP (`sdk/src/net.c`, `sdk/src/tcp.c`, `sdk/src/lwip_mqtt.c`)**:
   - When `mqtt_client_connect()` attempted to connect to an offline broker IP
     with no ARP cache entry, `tcp_connect()` -> `send_raw()` -> `net_ip_send()`
     called `net_poll()` in a loop while waiting for ARP.
   - Inside `tcp_poll()`, `c->t_rtx = now` was previously updated *after*
     `send_raw()`. When a retransmission triggered `send_raw()` for an uncached
     IP, recursive `net_poll()` -> `tcp_poll()` calls saw `(now - c->t_rtx) > TCP_RTX_MS`
     still true on every recursion level, exhausting all 8 `SYN` retries in a
     single call stack and freeing `tcp_conn_t` while `mqtt_client->tcp` still
     held its pointer. When that `tcp_conn_t` slot was later reused by a
     long-running FTP control connection, the next MQTT reconnect cycle closed
     it via `mqtt_disconnect()`.
   - **Fixes**:
     - Added re-entrancy guards `in_net_poll` (`sdk/src/net.c`) and `in_tcp_poll`
       (`sdk/src/tcp.c`).
     - Updated `c->t_rtx = now` *before* calling `send_raw()` in `tcp_poll()`,
       and verified `c->state != FREE` before returning from `tcp_connect()`.
     - Dedicated a separate `arp_txf[60]` buffer in `sdk/src/net.c` so outgoing
       ARP requests/replies never clobber an IP packet being assembled in
       `txf[1600]`.
     - Hardened `mqtt_disconnect()` (`sdk/src/lwip_mqtt.c`) to verify
       `tcp_user(tc) == client` before closing `tc`.

---

## 6. Cross-Platform OpenBeken SMB2 Server Driver (`drv_smb.c`)

Alongside the FTP server, we implemented a 100% DRY, cross-platform **SMB2
(`MS-SMB2` 2.0.2 / 2.1)** network file share server driver (`startDriver SMB`)
in `OpenBK7231T_App/src/driver/drv_smb.c` (`ENABLE_DRIVER_SMB`) over TCP port
**445**, exposing both LittleFS and any mounted USB volumes as native Windows
network shares:

- `\\192.168.0.134\obk` — unified VFS root `/` (includes LittleFS files plus
  `/usb0`, `/usb1`, `/usb2`)
- `\\192.168.0.134\usb0` — direct share rooted at `/usb0` (NTFS)
- `\\192.168.0.134\usb1` — direct share rooted at `/usb1` (FAT16)
- `\\192.168.0.134\usb2` — direct share rooted at `/usb2` (FAT32)

### 6.1 Windows 10/11 Native `mrxsmb20.sys` Compatibility Details

To work out-of-the-box with unpatched Windows 10 and Windows 11 Explorer /
`net use` / PowerShell without disabling Windows security policies, `drv_smb.c`
implements:

1. **64 KB Minimum Negotiated Buffer Sizes (`MS-SMB2` §3.2.5.2)**:
   - Windows `mrxsmb20.sys` rejects any `SMB2_NEGOTIATE` response where
     `MaxTransactSize`, `MaxReadSize`, or `MaxWriteSize` is `< 65536` (`64 KB`)
     with `ERROR_BAD_NET_NAME` (System error 67). `drv_smb.c` advertises and
     streams full `65536`-byte (`64 KB`) read/write payloads.
2. **Self-Contained NTLMv2 + SPNEGO `mechListMIC` + HMAC-SHA256 Signing**:
   - Embeds compact, dependency-free implementations of `MD4`, `MD5`,
     `HMAC-MD5`, `RC4`, `SHA-256`, and `HMAC-SHA256`.
   - Verifies the client's NTLMv2 `NTProofStr`, decrypts
     `EncryptedRandomSessionKey` with `RC4(SessionBaseKey)` when
     `NTLMSSP_NEGOTIATE_KEY_EXCH` is set, derives the server-to-client signing
     and sealing keys, and emits the 29-byte SPNEGO `NegTokenResp`
     (`accept-completed` + 16-byte NTLMv2 `mechListMIC` over the DER
     `mechTypes` list) required by Windows `secur32.dll` / `spnego.dll`.
   - Signs the final `SMB2_SESSION_SETUP` response and any subsequent request
     carrying `SMB2_FLAGS_SIGNED` using `HMAC-SHA256(ExportedSessionKey, pdu)[0..15]`,
     and never sets `SMB2_SESSION_FLAG_IS_GUEST` (which Windows 10/11 blocks by
     default under `AllowInsecureGuestAuth = 0`).
3. **Secure Dialect Validation (`FSCTL_VALIDATE_NEGOTIATE_INFO`, `0x00140204`)**:
   - When a Windows 10/11 client offers SMB 3.x dialects and the server selects
     SMB 2.1 (`0x0210`), Windows sends a signed `SMB2_IOCTL` with
     `CtlCode = 0x00140204` immediately after `SMB2_TREE_CONNECT`. `drv_smb.c`
     returns the 24-byte `VALIDATE_NEGOTIATE_INFO_RESPONSE` signed with
     `HMAC-SHA256`.
4. **SMB2 Compound Request Chaining (`NextCommand` + `SMB2_FLAGS_RELATED_OPERATIONS`)**:
   - Supports chained `CREATE -> QUERY_INFO -> CLOSE` and
     `CREATE -> QUERY_DIRECTORY -> CLOSE` compound frames with
     `FileId = { -1, -1 }` substitution and 8-byte response alignment.

### 6.2 Live Hardware Verification & 10 MB JPEG Benchmarks (Windows 11 Native SMB2)

Mounting and browsing all volumes from Windows 11 PowerShell:

```powershell
net use \\192.168.0.134\obk admin /user:admin
Get-ChildItem \\192.168.0.134\obk
Get-ChildItem \\192.168.0.134\obk\usb0
Get-ChildItem \\192.168.0.134\obk\usb1
Get-ChildItem \\192.168.0.134\obk\usb2
```

| SMB2 Path & Filesystem | Operation | Payload Size | Elapsed Time | Throughput | SHA-256 Match |
|---|---|---:|---:|---:|---|
| `\\192.168.0.134\obk` (LittleFS + USB root) | `Get-ChildItem` / `Set-Content` / `Get-Content` / `Remove-Item` | 4 entries | < 15 ms | — | PASS |
| `\\192.168.0.134\obk\usb0\smb_10mb.jpg` (**NTFS**) | `SMB2_WRITE` (streamed in 64 KB chunks) | **10,485,760 B** (10 MB) | **10.35 s** | **989.3 KB/s** | **PASS** |
| `\\192.168.0.134\obk\usb0\smb_10mb.jpg` (**NTFS**) | `SMB2_READ` (streamed in 64 KB chunks) | **10,485,760 B** (10 MB) | **31.30 s** | **327.2 KB/s** | **PASS** |
| `\\192.168.0.134\obk\usb1\smb_10mb.jpg` (**FAT16**) | `SMB2_WRITE` (streamed in 64 KB chunks) | **10,485,760 B** (10 MB) | **16.99 s** | **602.9 KB/s** | **PASS** |
| `\\192.168.0.134\obk\usb1\smb_10mb.jpg` (**FAT16**) | `SMB2_READ` (streamed in 64 KB chunks) | **10,485,760 B** (10 MB) | **34.93 s** | **293.2 KB/s** | **PASS** |
| `\\192.168.0.134\obk\usb2` (**FAT32**) | `Get-ChildItem` (4 entries including UTF-8 Polish PDF filenames) | 4 entries | < 20 ms | — | PASS |
