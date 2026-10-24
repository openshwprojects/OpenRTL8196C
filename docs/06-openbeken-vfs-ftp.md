# OpenBeken Wi-Fi, Unified USB/LittleFS VFS, Streaming HTTP, FTP & SMB2 Server

State as of 2026-10-23 (`seq=45`, cached `KSEG0` `0x80500000` execution). Documents
the dual-homed OpenBeken Wi-Fi integration, the unified LittleFS + USB
(FAT16/FAT32/exFAT/NTFS) Virtual Filesystem (`hal_vfs_rtl8196c.c`),
zero-RAM-buffering HTTP/VFS streaming, the cross-platform OpenBeken FTP
(`drv_ftp.c`) and SMB2 (`drv_smb.c`) server drivers, cached `KSEG0` SDRAM
execution, and benchmark results on the Edimax / Cyfrowy Polsat LT-6408n
(Realtek RTL8196C + RTL8192CE).

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

### 1.1 Web UI, Wi-Fi Scanner, MQTT & Home Assistant Screenshots

- **Main Dashboard, Configuration, GPIO & OTA Pages**:
  ![OpenBeken Home](../screenshots/01_home.png)
  ![Configuration Menu](../screenshots/02_config_menu.png)
  ![GPIO Pins Configuration](../screenshots/03_gpio_pins.png)
  ![Startup Configuration](../screenshots/06_startup_config.png)
  ![General Flags](../screenshots/09_general_flags.png)
  ![OTA Update](../screenshots/08_ota_update.png)
  ![About Page](../screenshots/07_about.png)

- **Wi-Fi Site Survey, Network, IP & MAC Configuration**:
  ![Wi-Fi Site Survey Scan](../screenshots/11_wifi_scan.png)
  ![Network Configuration](../screenshots/05_network_config.png)
  ![IP Configuration](../screenshots/12_ip_config.png)
  ![MAC Configuration](../screenshots/13_mac_config.png)

- **MQTT 3.1.1, Home Assistant Auto-Discovery & Dual-Net WebApp**:
  ![MQTT Configuration](../screenshots/04_mqtt_config.png)
  ![MQTT Connected Configuration](../screenshots/15_mqtt_connected_cfg.png)
  ![Home Page with MQTT Connected](../screenshots/16_home_mqtt_connected.png)
  ![Home Assistant Discovery Page](../screenshots/10_ha_discovery.png)
  ![Home Assistant MQTT Devices](../screenshots/18_ha_mqtt_devices.png)
  ![Home Assistant Device Page](../screenshots/17_ha_device_page.png)
  ![Home Assistant Device Toggled](../screenshots/19_ha_device_toggled.png)
  ![OpenBeken WebApp](../screenshots/14_webapp.png)
  ![WebApp Dual-Homed Network Logs](../screenshots/20_webapp_logs_dual_net.png)

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

### 2.1 OpenBeken WebApp Unified VFS Screenshots (`/`, `/usb0` NTFS, `/usb1` FAT16)

- **WebApp Filesystem Tab — Root `/` (`autoexec.bat` + `/usb0`, `/usb1`, `/usb2`)**:
  ![WebApp VFS Root and USB Mounts](../screenshots/21_webapp_lfs_root_usb.png)

- **WebApp Filesystem Tab — `/usb0` (NTFS Volume) & `/usb1` (FAT16 Volume)**:
  ![WebApp VFS USB0 NTFS](../screenshots/22_webapp_lfs_usb0_ntfs.png)
  ![WebApp VFS USB1 FAT16](../screenshots/23_webapp_lfs_usb1_fat16.png)

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

### 3.1 FileZilla FTP Client & Side-by-Side WebApp Screenshots

- **Root VFS (`/`) — LittleFS + Mounted USB Drives (`usb0`, `usb1`, `usb2`)**:
  ![FileZilla Root VFS](../screenshots/25_filezilla_root.png)
  ![FileZilla vs WebApp Root VFS](../screenshots/30_compare_root_ftp_vs_webapp.png)

- **`/usb0` (NTFS Volume) & `/usb0/boot`**:
  ![FileZilla USB0 NTFS](../screenshots/26_filezilla_usb0_ntfs.png)
  ![FileZilla USB0 Boot](../screenshots/28_filezilla_usb0_boot.png)
  ![FileZilla vs WebApp USB0 NTFS](../screenshots/31_compare_usb0_ftp_vs_webapp.png)

- **`/usb1` (FAT16 Volume)**:
  ![FileZilla USB1 FAT16](../screenshots/27_filezilla_usb1_fat16.png)
  ![FileZilla vs WebApp USB1 FAT16](../screenshots/32_compare_usb1_ftp_vs_webapp.png)

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
  `/usb0`, `/usb1`, `/usb2`, `/usb3`)
- `\\192.168.0.134\usb0` — direct share rooted at `/usb0` (58 GB NTFS `"Windows 10"`, `090c:2000`)
- `\\192.168.0.134\usb1` — direct share rooted at `/usb1` (942 MB FAT16 `"PHONE CARD"`, `05e3:0751`)
- `\\192.168.0.134\usb2` — direct share rooted at `/usb2` (961 MB FAT32 `"SLITAZ CORE"`, `ffff:5678`)
- `\\192.168.0.134\usb3` — direct share rooted at `/usb3` (**500 GB / `476,940 MB` NTFS HDD** `"Windows"`, JMicron `152d:0578 External USB 3.0`)

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
5. **Unsigned 32-Bit File Size Handling for Large Files (`drv_smb.c`)**:
   - Uses `uint32_t` file sizes across `smb_stat_vfs()` and `smb2_handle_read()`
     so multi-gigabyte system files on large NTFS drives (`pagefile.sys`
     `1.44 GB`, `hiberfil.sys` `4.03 GB`) never wrap negative in signed 32-bit
     arithmetic.

### 6.2 Live Hardware Verification & 10 MB JPEG Benchmarks (Windows 11 Native SMB2)

Mounting and browsing all 4 simultaneous USB volumes from Windows 11 PowerShell:

```powershell
net use \\192.168.0.134\obk admin /user:admin
net use Z: \\192.168.0.134\usb3 admin /user:admin /persistent:no
net use Y: \\192.168.0.134\usb0 admin /user:admin /persistent:no
net use X: \\192.168.0.134\usb1 admin /user:admin /persistent:no
net use W: \\192.168.0.134\usb2 admin /user:admin /persistent:no
Get-ChildItem Z:\
```

| SMB2 Path & Filesystem | Operation | Payload Size | Elapsed Time | Throughput | SHA-256 Match |
|---|---|---:|---:|---:|---|
| `\\192.168.0.134\obk` (LittleFS + 4× USB root) | `Get-ChildItem` / `Set-Content` / `Get-Content` / `Remove-Item` | 5 entries (`usb0..3` + `autoexec.bat`) | < 15 ms | — | PASS |
| `\\192.168.0.134\obk\usb0\smb_10mb.jpg` (**NTFS 58 GB**, mapped `Y:`) | `SMB2_WRITE` (streamed in 64 KB chunks) | **10,485,760 B** (10 MB) | **10.35 s** | **989.3 KB/s** | **PASS** |
| `\\192.168.0.134\obk\usb0\smb_10mb.jpg` (**NTFS 58 GB**, mapped `Y:`) | `SMB2_READ` (streamed in 64 KB chunks) | **10,485,760 B** (10 MB) | **31.30 s** | **327.2 KB/s** | **PASS** |
| `\\192.168.0.134\obk\usb1\smb_10mb.jpg` (**FAT16 942 MB**, mapped `X:`) | `SMB2_WRITE` (streamed in 64 KB chunks) | **10,485,760 B** (10 MB) | **16.99 s** | **602.9 KB/s** | **PASS** |
| `\\192.168.0.134\obk\usb1\smb_10mb.jpg` (**FAT16 942 MB**, mapped `X:`) | `SMB2_READ` (streamed in 64 KB chunks) | **10,485,760 B** (10 MB) | **34.93 s** | **293.2 KB/s** | **PASS** |
| `\\192.168.0.134\obk\usb2` (**FAT32 961 MB**, mapped `W:`) | `Get-ChildItem` (4 entries including UTF-8 Polish PDF filenames) | 4 entries | < 20 ms | — | PASS |
| `Z:\` -> `\\192.168.0.134\usb3` (**NTFS 500 GB HDD**, `476,940 MB` / `465 GB`) | `net use Z:` / `dir Z:\` / `SMB2_0_INFO_FILESYSTEM` (`Volume="Windows"`, `Serial=8196-C103`, `18` entries including `hiberfil.sys` `4.13 GB`, `pagefile.sys` `1.48 GB`) | 18 entries | < 25 ms | — | PASS |

#### CrystalDiskMark 8.0.5 x64 Benchmark on Mapped SMB2 Share (`Y:` -> `\\192.168.0.134\usb0`, 58 GiB USB 2.0 NTFS, `FILE_FLAG_NO_BUFFERING`)

Running **CrystalDiskMark 8.0.5 x64** (`diskspd64.exe -W0 -S`, unbuffered direct I/O bypassing the Windows SMB client cache) against the mapped OpenBeken SMB2 drive `Y:` (`\\192.168.0.134\usb0`) with cached `KSEG0` (`0x80500000`) execution and the 64 KB VFS streaming window:

| CrystalDiskMark Test | Block Size & Queue/Threads | Read Throughput (`MB/s`) | Write Throughput (`MB/s`) | Read IOPS (`diskspd64`) | Write IOPS (`diskspd64`) |
|---|---|---:|---:|---:|---:|
| **`SEQ1M Q8T1`** | `1 MiB` sequential (`16 × 64 KB` SMB2 PDUs), `Q=8, T=1` | **3.55 MB/s** | **2.10 MB/s** | ~54.2 IOPS (`64K`) | ~32.1 IOPS (`64K`) |
| **`SEQ1M Q1T1`** | `1 MiB` sequential (`16 × 64 KB` SMB2 PDUs), `Q=1, T=1` | **3.77 MB/s** | **2.72 MB/s** | ~57.7 IOPS (`64K`) | ~41.5 IOPS (`64K`) |
| **`RND4K Q32T1`** | `4 KiB` random (`-r`), `Q=32, T=1` | **1.13 MB/s** | **1.06 MB/s** | **277.96 IOPS** | **275.25 IOPS** |
| **`RND4K Q1T1`** | `4 KiB` random (`-r`), `Q=1, T=1` | **0.49 MB/s** | **0.45 MB/s** | **120.12 IOPS** | **110.35 IOPS** |

### 6.3 Windows Explorer SMB2 Share, Mapped Network Drives (`W:`..`Z:`) & Comparison Screenshots

- **CrystalDiskMark 8.0.5 x64 Benchmark on Mapped OpenBeken SMB2 Share (`Y:` -> `\\192.168.0.134\usb0`, 58 GiB NTFS)**:
  ![CrystalDiskMark 8.0.5 x64 Benchmark on OpenBeken SMB2 Share](../screenshots/47_crystaldiskmark_smb_usb.png)

- **OpenBeken Main Web UI Status (FTP + SMB2 Active)**:
  ![OpenBeken FTP and SMB2 Status](../screenshots/29_obk_main_ftp_smb_status.png)

- **Windows 11 "This PC" (`Ten komputer`) with All 4 USB Drives Mapped via SMB2 (`W:`, `X:`, `Y:`, `Z:`) & Mapped `Z:\` (`500 GB NTFS HDD`)**:
  ![Windows Explorer This PC Mapped SMB Drives](../screenshots/44_explorer_this_pc_mapped_smb_drives.png)
  ![Windows Explorer Mapped Drive Z 500GB NTFS HDD](../screenshots/45_explorer_mapped_z_usb3_500gb_hdd.png)
  ![Windows Explorer This PC vs Mapped Drive Z 500GB HDD](../screenshots/46_compare_this_pc_and_mapped_z_500gb.png)

- **Windows Explorer SMB2 Root Share (`\\192.168.0.134\obk`, 4 USB Drives `usb0`..`usb3`) & WebApp Comparison**:
  ![Windows Explorer SMB2 Root](../screenshots/33_explorer_smb_root.png)
  ![Windows Explorer SMB2 vs WebApp Root](../screenshots/37_compare_root_smb_vs_webapp.png)

- **Windows Explorer SMB2 `/usb3` — 500 GB External USB 3.0 NTFS HDD (`\\192.168.0.134\obk\usb3`, `476,940 MB`) & OpenBeken `usb_status` Logs**:
  ![Windows Explorer SMB2 USB3 500GB NTFS HDD](../screenshots/41_explorer_smb_usb3_500gb_hdd.png)
  ![OpenBeken WebApp Logs USB3 500GB HDD and 6-Device USB Topology](../screenshots/42_webapp_logs_usb3_500gb_hdd.png)
  ![Windows Explorer SMB2 vs OpenBeken Logs USB3 500GB HDD](../screenshots/43_compare_usb3_500gb_smb_vs_logs.png)

- **Windows Explorer SMB2 `/usb0` (`\\192.168.0.134\obk\usb0`, 58 GB NTFS) & `/usb0/boot`**:
  ![Windows Explorer SMB2 USB0 NTFS](../screenshots/34_explorer_smb_usb0_ntfs.png)
  ![Windows Explorer SMB2 USB0 Boot](../screenshots/36_explorer_smb_usb0_boot.png)
  ![Windows Explorer SMB2 vs WebApp USB0 NTFS](../screenshots/38_compare_usb0_smb_vs_webapp.png)
  ![Windows Explorer SMB2 vs FileZilla FTP USB0 NTFS](../screenshots/40_compare_smb_vs_ftp_usb0_ntfs.png)

- **Windows Explorer SMB2 `/usb1` (`\\192.168.0.134\obk\usb1`, 942 MB FAT16)**:
  ![Windows Explorer SMB2 USB1 FAT16](../screenshots/35_explorer_smb_usb1_fat16.png)
  ![Windows Explorer SMB2 vs WebApp USB1 FAT16](../screenshots/39_compare_usb1_smb_vs_webapp.png)

---

## 7. Cached `KSEG0` (`0x80500000`) Execution & 12-Step Hardware Verification (`seq=47`)

Switching OpenBeken (`tools/build_obk.py`, `tools/flash_obk.py`, `sdk/src/start.S`)
from uncached `KSEG1` (`0xA0500000`) to cached `KSEG0` (`0x80500000`, 16 KB
I-cache + 8 KB write-through D-cache via Lexra CP0 `$20` `CCTL`) accelerates CPU,
crypto, and network throughput across the board:

| Workload / Benchmark | Uncached `KSEG1` (`0xA0500000`) | Cached `KSEG0` (`0x80500000`) | Speedup |
|----------------------|--------------------------------:|------------------------------:|--------:|
| `30_ram_exec` Tight ALU + Branch Loop | `19,340 us` (`11.2x` vs SPI XIP) | `3,104 us` (`69.9x` vs SPI XIP) | **6.23x** |
| WPA2-PSK `PBKDF2-SHA1` (256 iter) | `424,522 us` | `17,976 us` | **23.62x** (`43.0x` vs SPI XIP) |
| HTTP `/api/lfs/usb0/bootmgr` (`397,752 B`) | `~360 KB/s` | **3,686 KB/s (`3.60 MB/s`)** | **10.2x** |
| FTP `RETR /usb0/bootmgr` (`397,752 B`) | `~384 KB/s` | **2,214.6 KB/s (`2.21 MB/s`)** | **5.77x** |
| SMB2 `READ \\<ip>\obk\usb0\bootmgr` (`397,752 B`, HMAC-SHA256 signed) | `~327 KB/s` | **1,189.5 KB/s (`1.19 MB/s`)** | **3.64x** |
| CrystalDiskMark 8.0.5 `SEQ1M Q1T1` (`Y:` -> `\\<ip>\usb0`, `16 MiB` unbuffered) | `~327 KB/s` R / `~600 KB/s` W | **3.77 MB/s Read / 2.72 MB/s Write** | **11.5x R / 4.5x W** |

### Hardware & DMA Coherency Details for `KSEG0`
- **Startup Cache Invalidation (`sdk/src/start.S`)**: Pulses CP0 `$20` (`CCTL`)
  `0 -> 3 -> 0` (`CCTL_DINVAL | CCTL_IINVAL`) when booted in `KSEG0` before
  zeroing `.bss`.
- **Switch Core, Wi-Fi & USB DMA Buffers (`sdk/src/swcore.c`, `sdk/src/wl.c`, `sdk/src/usb.c`)**:
  All DMA descriptor rings and packet buffers are accessed through their
  `KSEG1` (`| 0xA0000000`) aliases, with `CCTL_DWBINVAL` barriers around USB
  EHCI/OHCI transfers (`usb.c`, `usb_msc.c`, `hal_vfs_rtl8196c.c`).
- **PCIe Bus-Master Quiesce on RAM Hot-Reload (`sdk/src/ota.c`)**: Clears PCIe
  root-port command register (`REG32(0xB8B00004) = 0`) and asserts `PERST#`
  (`REG32(SYS_BASE + 0x10) &= ~(1u << 12)`) before copying a new image over
  `.text`/`.data`/`.bss`.
- **Fast SDRAM Snapshot Reboot (`hal_generic_rtl8196c.c`)**: `HAL_RebootModule()`
  checks `SNAPSHOT_BASE_ADDR` (`0xA0900000`) first for sub-second SDRAM soft
  reboot before falling back to `ota_boot_active_slot(0)`.
- **Automated 12-Step Hardware Suite (`tools/test_obk.py`)**: Verifies HTTP UI,
  7 LED channels, LittleFS R/W, USB VFS (`/usb0` NTFS & `/usb1` FAT16 R/W +
  `3.60 MB/s` streaming), FTP `:21`, SMB2 `:445` (NTLMv2 + compound `CREATE` +
  `QUERY_DIRECTORY` + `CLOSE`), SSDP `:1900`, Wi-Fi site survey, Berry VM, TCP
  OTA `:9698`, and HTTP `/ota` with cold-boot persistence (`12/12 PASS`).
