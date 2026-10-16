# RTL8196C bare-metal hardware notes

Research notes for running our own code on the Realtek RTL8196C from the
Cyfrowy Polsat LT-6408n router. Every fact is tagged:

- **[measured]** – observed on our board over UART
- **[sdk]** – taken from Realtek SDK sources (mostly the RTL8196E BSP, which
  shares the peripheral block layout with the RTL8196C)
- **[3rd-party]** – from other bare-metal projects on the same core family
- **[unverified]** – a guess that must be confirmed with a probe program

## 1. The chip

| Item | Value | Source |
|------|-------|--------|
| SoC | Realtek RTL8196C, chip rev 4 | [measured] kernel log |
| CPU core | Lexra RLX4181, MIPS-I class (R3000-like), big-endian, no FPU | [sdk], [3rd-party] |
| CPU clock | 390 MHz ("390MHz" in loader banner, 388 BogoMIPS) | [measured] |
| Peripheral ("HS1"/lexra bus) clock | 200 MHz on RTL8196E; must be calibrated on 8196C | [sdk] [unverified] |
| DRAM | 32 MB SDRAM at physical 0x00000000 | [measured] "Memory: 26632k/32768k" |
| SPI flash | 8 MB MX25L6405 at CS0, controller 0xB8001200 | [measured] |
| CP0 PRId | 0x0000CD01 (RLX4181 = 0xCD00 family) | [3rd-party] |
| USB | EHCI at 0x18021000, OHCI at 0x18020000, IRQ 10 | [measured] kernel log |
| UART0 | 16550A at 0x18002000 (KSEG1: 0xB8002000), kernel IRQ 23 | [measured] kernel log |

### Lexra RLX4181 quirks (why a generic MIPS32 toolchain is not enough)

| Quirk | Consequence | Source |
|-------|-------------|--------|
| ISA is MIPS-I plus a few extras, `Config.M = 0` | Compile with `-march=mips1` (safe) or `mips32` (works for simple code, risky) | [3rd-party] |
| `lwl/lwr/swl/swr` reportedly unsupported on Lexra, though one RLX4181 probe says they execute | Avoid unaligned access; write byte-wise `memcpy`; prefer `-march=mips1` and let the compiler avoid them | [3rd-party] conflicting |
| Load delay slot (R3000 style) | Vendor toolchain pads load delay slots for `-march=4181`; treat hazard as real until probed | [3rd-party] [unverified] |
| No CP0 `Count`/`Compare` | No free CPU timer; use SoC timer block TC0/TC1 | [3rd-party] |
| General exception vector at **0x80000080** (not 0x80000180) | Install handler trampoline at 0x80000080; `Status.BEV` is already 0 at the loader prompt | [3rd-party] |
| Exception return is `rfe` in a `jr` delay slot (no `eret`) | Hand-written handler epilogue | [3rd-party] |
| CP0 register 20 = `CCTL` cache control (Lexra-specific) | Sequence `mtc0 $0,$20; nop; mtc0 cmd,$20; nop; nop; mtc0 $0,$20` with `cmd=0x002` invalidates I-cache | [3rd-party] |
| I-cache returns stale bytes if code is written and executed without a CCTL invalidate | Either run from KSEG1 (uncached, 0xA0xxxxxx) or invalidate after loading | [3rd-party] |
| **D-cache is write-back.** CCTL `0x001` (DINVAL) drops dirty lines: 64 cached stores followed by DINVAL were all lost, even after a 10 ms wait or an unrelated uncached read. `0x100` writes dirty lines back, `0x200` writes back and invalidates. CPU reads through the KSEG1 alias of a dirty line return the cached value, so "store cached, read uncached" looks like write-through; DMA masters see only RAM. No write-allocate: a store that misses goes straight to RAM, so only lines already in the cache become dirty (OTA test: a 15 KB copy to never-cached 0x8050xxxx followed by DINVAL arrived intact) | Never use DINVAL alone in running code. Before a device reads cached memory: `CCTL_DWB`. Around a device write into cached memory: `CCTL_DWBINVAL` before and after, buffer 32-byte aligned and padded to whole cache lines | [measured, probes/87_cache_policy, 85_usb_zero_copy] |
| Writing `WDTCNR = 0` triggers a watchdog reboot back to the loader | Handy software reset | [3rd-party] |

## 2. Memory map (KSEG1, uncached)

| Base | Block | Source |
|------|-------|--------|
| 0xB8000000 | System control (`PIN_MUX_SEL` at +0x40, strap/clock regs) | article [measured by author] |
| 0xB8001200 | SPI flash controller (SFCSR/SFDR) | [3rd-party] |
| 0xB8002000 | UART0 (16550) | [measured] |
| 0xB8003000 | Interrupt controller (GIMR/GISR/IRR0-3) | [sdk] |
| 0xB8003100 | Timer/counter block (TC0, TC1, CDBR, WDT) | [sdk] |
| 0xB8003500 | GPIO ports A-D | article |
| 0xB8020000 | OHCI, 0xB8021000 EHCI | [measured] |

RAM aliases: physical 0x00500000 is 0x80500000 cached (KSEG0) and
0xA0500000 uncached (KSEG1). The loader stages the Linux image at
0x80500000, so anything we load there is overwritten only on the next
boot, never while running. [measured] "Jump to image start=0x80500000".

## 3. UART0 (16550 compatible)

Register stride is 4 bytes and the register byte sits in the **most
significant byte** of the 32-bit word (big-endian). A byte access at the
word address therefore hits the register directly. [sdk] [article]

| Offset | Read | Write |
|--------|------|-------|
| 0x00 | RBR (receive) / DLL when LCR.DLAB=1 | THR (transmit) / DLL |
| 0x04 | IER / DLM | IER / DLM |
| 0x08 | IIR | FCR |
| 0x0C | LCR | LCR |
| 0x10 | MCR | MCR |
| 0x14 | LSR | - |
| 0x18 | MSR | - |

LSR bits: 0x01 DR (data ready), 0x20 THRE (transmit holding empty),
0x40 TEMT (transmitter empty). IER bits: 0x01 RX data available, 0x02 THR
empty. The loader leaves the port at 38400 8N1. [measured]

Open question from the article: the author lost characters when polling
THRE. A probe should print the raw LSR value to see whether it must be
read as a 32-bit word (`>> 24`). [unverified]

## 4. Interrupt controller (0xB8003000) [sdk, RTL8196E header]

| Reg | Addr | Meaning |
|-----|------|---------|
| GIMR | 0xB8003000 | Global interrupt mask, 1 = enabled |
| GISR | 0xB8003004 | Global interrupt status (pending) |
| IRR0 | 0xB8003008 | Routing for sources 0..7, one nibble each (bits 3:0 = source 0) |
| IRR1 | 0xB800300C | Routing for sources 8..15 |
| IRR2 | 0xB8003010 | Routing for sources 16..23 |
| IRR3 | 0xB8003014 | Routing for sources 24..31 |

Source numbering **[measured, examples/92 and 93]**. Source n is GIMR bit n,
GISR bit n and nibble n of the IRR block (IRRk bits 4(n-8k)+3 : 4(n-8k)).
The 8196C numbering is NOT the RTL8196E one:

| Source | Peripheral | Evidence |
|--------|-----------|----------|
| 7 | UART0 | THRE interrupt lights GISR bit 7, only GIMR bit 7 passes it; Linux log says UART irq 23 = 16 + 7 |
| 14 | TC0 | timer wrap lights GISR bit 14; loader routes nibble 6 of IRR1 to IP4 |
| 15 | TC1 | timer 1 wrap lights GISR bit 15 |
| 1, 12, 13 | unknown, **permanently pending** | present in GISR at all times; must stay masked |
| 8 | unknown (switch core?) | loader routes it to IP3 |

Routing value semantics **[measured]**: writing N = 2..7 into a source's
nibble raises CPU line IPN (Cause bit 8+N) while the source is pending and
its GIMR bit is set. Values 0, 1 and 8..15 raise nothing, so the RLX4181
here has no usable vectored (LOPI) interrupts, only the six classic lines.
GISR is the raw pending status, not masked by GIMR.

## 5. Timers (0xB8003100) [measured with examples/90_timer_probe]

| Reg | Addr | Bits |
|-----|------|------|
| TC0DATA | 0xB8003100 | period << 4 (28-bit) |
| TC1DATA | 0xB8003104 | period << 4 |
| TC0CNT | 0xB8003108 | current count << 4, counts **up** from 0 |
| TC1CNT | 0xB800310C | current count << 4 |
| TCCNR | 0xB8003110 | bit31 TC0EN, bit30 TC0 timer mode (1) / counter mode (0), bit29 TC1EN, bit28 TC1 mode |
| TCIR | 0xB8003114 | bit31 TC0IE, bit30 TC1IE, bit29 TC0IP (write 1 to clear), bit28 TC1IP |
| CDBR | 0xB8003118 | clock divisor << 16 (max 65535) |
| WDTCNR | 0xB800311C | watchdog; 0xA5000000 = stopped pattern, 0 = bite now |

Measured: the counter runs from 0 up to TCxDATA, wraps to 0 and sets TCxIP
on the wrap. The count lives in bits 31:4 (the SDK's "shift 4" variant,
not the older shift 8). With CDBR divisor 200 the tick rate is 1.0014 MHz
against the PC clock, so the **bus clock is 200 MHz**. The loader leaves
TC0 running with divisor 14 and data 0x22E0A, a 10 ms tick, with TC0IE set
and routed through IRR1 nibble 0 = 3. Interrupt acknowledge: write TCxIP
back to TCIR.

**Read quirk [measured, probes/88_timer_glitch]:** reads of TCxCNT taken
close together see the lowest count bit flip, so consecutive values go
+1, -1, +1 ... (about 40% of back-to-back reads step backwards), while
the long-run rate stays exact. With 200 or more nops between reads the
values are clean. Unsigned "now - start" code turns a -1 into a ~268 s
jump and ends waits early, so `micros()` ignores reads up to 63 counts
behind the previous one.

## 6. GPIO (0xB8003500) [article, sdk]

| Reg | Addr | Meaning |
|-----|------|---------|
| PABCD_CNR | 0xB8003500 | 0 = GPIO, 1 = peripheral function, one bit per pin |
| PABCD_PTYPE | 0xB8003504 | 0 = input/output, 1 = open drain (unverified) |
| PABCD_DIR | 0xB8003508 | 1 = output |
| PABCD_DAT | 0xB800350C | data |
| PABCD_ISR | 0xB8003510 | interrupt status |
| PAB_IMR | 0xB8003514 | interrupt mode per pin, 2 bits (00 off, 01 falling, 10 rising, 11 both) |
| PCD_IMR | 0xB8003518 | same for ports C/D |

Pin numbering: bit = port*8 + pin (A0 = bit 0, B3 = bit 11, C6 = bit 22).
The LAN LEDs are on GPIO B2..B5, C6, B7, C0 but only after switching
`PIN_MUX_SEL` (0xB8000040) bits 13:0 to 1 (value 0x3FFF). Bit 22 of that
register would move the UART pins to GPIO, so never set it.

Board pins, from the stock kernel's GPIO driver (`rtl_gpio_init` at
0x8035AAC8 and its `/proc` handlers, disassembled from the flash backup):

| Pin | Direction | Function | `/proc` entry |
|-----|-----------|----------|---------------|
| A2 | output | LED | |
| A3 | input | WPS / second button | `wps_but` |
| A4 | output, high = on | **USB VBUS switch**, off at boot | `usb_pwr` |
| A5 | input | reset button | `reset_but` |
| A6 | output | power LED | `power_led` |
| C0 | output | internet LED | `internet_led` |
| C3 | output, active low | USB LED | |

Port A pins 2..6 only work as GPIO after `PIN_MUX_SEL` bits 21:20 are set
(the stock driver writes 0x00343C00; `gpio_set_dir()` does this). The
loader's `GPIOA4 off/on` line refers to the USB power pin; the earlier note
here calling A4 the reset button was wrong.

## 6b. USB host [measured + stock kernel]

| Block | Address | Facts |
|-------|---------|-------|
| EHCI | 0xB8021000 | Synopsys, version 1.00, 1 port, port power control, 1 companion; registers little-endian |
| OHCI | 0xB8020000 | revision 0x10, 1 root port, little-endian |
| PHY | via 0xB8000034 + 0xB80210A4 | data byte in 0xB8000034 (`0x1F00 \| val`), address nibbles strobed in 0xB80210A4 |
| Clock | 0xB8000010 bit 17 | must be set before the controllers answer |
| VBUS | GPIO A4 | high = 5 V on the port |

Stock init (`ehci_rtl8652_init`): set clock bit 17, register the EHCI,
then patch PHY registers F4=E3, E0=99, E1=A8, E2=98, E3=C1, E5=91. The
stock kernel leaves 0xB8000034 at 0x00001Fxx afterwards. Power is only
switched on when userspace writes `1` to `/proc/usb_pwr`. Driver:
`sdk/src/usb.c`, examples `17_usb_host`, `18_usb_mouse`.

Measured on our board (2026-10-15):

- The port has 5 V all the time. An A4Tech mouse stayed connected through
  A4 high, low and released (`probes/99_usb_vbus_blink`), so A4 does not
  switch VBUS on this unit even though the stock kernel drives it. Its
  LED goes dark a moment after plugging in because an unenumerated device
  enters USB suspend.
- Low-speed device detection: EHCI PORTSC shows line state K (01), the
  driver sets port owner, OHCI resets the port and enumerates. All
  descriptors and strings read correctly.
- Interrupt IN through the OHCI periodic list works: 193 boot-protocol
  reports with movement and all three buttons, no errors. A NAKed
  interrupt TD keeps condition code 0xF, so "not accessed" is normal
  while the device has nothing to send. This mouse ignores SET_IDLE.
- Wheel: the boot protocol has no wheel, so `sdk/src/hid.c` parses the
  HID report descriptor and the mouse stays in report protocol. This
  A4Tech reports ID 1, 8 button bits, X, Y, wheel (8 bits each) plus one
  vendor byte; wheel steps decode in both directions.
- Once, after ~30 s of use, reports stopped while the board kept running
  and the mouse LED went dark (suspend). The likely cause is the
  controller disabling the port; `usb_poll()` now detects a disabled port
  and re-enumerates, and prints the OHCI state when it happens. Not seen
  again since (cause still unknown).
- High speed works: a generic 1 GB pendrive (ffff:5678) is detected on
  EHCI after the port reset (PORTSC 0x1007, enabled), enumerates with a
  64-byte ep0, and serves bulk-only mass storage through EHCI bulk
  transfers (QH keeps the data toggle, one 16 KB qTD per transfer).
  SCSI INQUIRY / READ CAPACITY / READ(10) work, FAT32 mounts and the root
  directory lists with long names (`examples/19_usb_storage`). Read speed
  is ~1 MB/s, limited by our byte-wise copies out of uncached memory and
  one command per 16 KB, not by the bus.
- An SD card reader was never detected (no connect at all) while a
  high-speed pendrive is, so the reader itself is the suspect.

## 6c. PCIe and the Wi-Fi chip [measured, examples/23_pcie_probe]

One PCIe root port. The bring-up sequence was disassembled from the stock
kernel's `PCIE_reset_procedure` (kernel offset 0x80138f94; flash backup,
LZMA kernel at flash 0x12C18) and works as is:

| Step | Register | Value |
|------|----------|-------|
| clock on | `0xB8000010` (CLK_MANAGE) | bit 11 set, 100 ms |
| PHY MDIO reset / load | `0xB8000050` | 8, 9, 0xB |
| PHY reset + LTSSM enable | `0xB8B01008` | 1, then 0x81 |
| PHY parameters | `0xB8B01000` = `reg<<8 \| val<<16 \| 1` | set B, because `0xB8000000` reads `80000002` on this board ("96C - 40MHz Clock Source") |
| device reset | `0xB8000010` bit 12 (PERST) | low 300 ms, PHY reset again, high |
| link | `0xB8B00728 & 0x1F` | `0x11` = L0 (up at once on this board) |
| root port enable | `0xB8B00004` | `0x00100007` |
| power down | `0xB8B10044` D3hot, MDIO reg 15 = 0x708, clock bit 11 off | |

Root port config space at `0xB8B00000` (`10ec:8196`, class 0604), device
config space at `0xB8B10000` (32-bit reads come back in normal order).

Attached device: **`10ec:8191` rev 01, class 0280: RTL8192CE family**
(RTL8188CE / RTL8188RE / RTL8192CE, 802.11n 2.4 GHz). It has BAR0 (I/O),
BAR2 (64-bit memory), and the PM, MSI and PCIe capabilities. The stock
firmware drives it with Realtek's `rtl8192cd` driver (firmware built into
the kernel).

Chip registers: the stock driver sets BAR2 = `0x19000000`, so chip
register N is at **`0xB9000000 + N`**. Words read in normal order, with no
swap, same as config space. Checked against SYS_CFG's clock-valid bits and
`0x0A` = `0x20` (autoload from efuse OK).

The variant check is the stock `check_chipID` (kernel 0x8015ba00):
- SYS_CFG (`0xF0`) bit 27 set means 92C (2T2R);
- otherwise 88C (1T1R), and HPON_FSM (`0xEC`) `>>22 & 3 == 1` means 88R
  (high power);
- bit 23 marks a test chip.

On this board SYS_CFG = `08400339` and HPON_FSM = `98053f15`. **The chip is
an RTL8192CE, 2T2R** (two transmit and two receive paths), A-cut (bits
12-15 = 0), a normal production chip (bit 23 clear), TSMC (bit 19 clear),
with no 1T2R bonding (`0xEC` bits 22-23 = 0).

Access width [measured, examples/24_wifi_fw]: 8-, 16- and 32-bit accesses
at `0xB9000000 + reg` all hit the right bytes (little-endian register
numbering, no address XOR). Firmware start [measured] uses the rtlwifi
sequence:
1. power-on: RSV_CTRL 0, SPS0 0x2B, XTAL 0x0F, set APS_FSMCO bit 8 and
   wait for it to clear, then APS_FSMCO = 0x1012, ISO+1 = 0x82, CR = 0x02FF;
2. firmware download: FEN_CPUEN, MCUFWDL_EN, 4 KB pages (page number in
   MCUFWDL+2 bits 0-2) written as little-endian words at chip address
   0x1000;
3. wait for FWDL_CHKSUM_RPT, set MCUFWDL_RDY, wait for WINTINI_RDY.

With stock firmware 92c2 v69.0 (11,615 bytes) the download takes 17 ms and
the firmware runs 5 ms later (MCUFWDL `000200c6`).

**Receive path** [measured, examples/25_wifi_scan, passive scan works]:
- Bring-up order as Linux `rtl92ce_hw_init`:
  1. power-on, LLT, DMA ring addresses;
  2. firmware;
  3. MAC table (byte writes);
  4. baseband: SYS_FUNC_EN |= bits 13/1/0, **AFE PLL `0x28` = 0x83 and
     `0x29` = 0xDB** (ADC/DAC clock; without it decoding is erratic),
     RF_CTRL = 7, SYS_FUNC_EN = 0xE3, XTAL+1 = 0x80, LEDCFG0 bit 23;
  5. tables: PHY_REG 2T, then the 2-row RX fix, then AGC;
  6. RF paths A and B through the 3-wire interface (0x840/0x844, with the
     RFENV save/restore in 0x870);
  7. CCK/OFDM enable bits in `0x800`, LC calibration, 20 MHz;
  8. the channel is the low 10 bits of RF register 0x18.
- **CR bit 6 (MACTXEN) must stay set.** With it clear the MAC receives
  nothing at all; frames already buffered still drain, which looks like
  "4 frames, then silence". For a listen-only setup, keep TXPAUSE = 0xFF and
  never hand over a TX descriptor. Also set a MAC address (`0x610`); the
  reset value is 00:00:00:00:00:00.
- Interrupt registers are **HIMR `0x120`, HISR `0x124`, HIMRE `0x128`,
  HISRE `0x12C`**. `0x44` is GPIO_PIN_CTRL: writing 0xFFFFFFFF there (an
  old 92S-style HISR address) turns every Wi-Fi-chip GPIO into a high
  output.
- RX descriptor (32 bytes, little-endian words): w0 holds the length (bits
  0-13), CRC error (bit 14), drv_info size in units of 8 bytes (bits 16-19),
  shift (bits 24-25), EOR (bit 30) and OWN (bit 31); w3 bits 0-5 hold the
  rate; w5 holds the TSF; w6 the buffer address. The buffer holds drv_info
  (32 bytes of PHY status: OFDM `pwdb_all` at byte 4, CCK AGC report at byte
  5), then the 802.11 frame with its FCS. Bit 26 (PHY status) stays 0 even
  when drv_info is present.
- Path B's RF read-back is unreliable (register 0x18 returns other
  registers' values); writes work. The channel word is shared from path A.
- Stock 40 MHz-board PCIe tuning (applied in example 25): root-port PHY
  MDIO register 15 = 0x0F0F; Wi-Fi EPHY registers 4/5/3 = 0xC940/0x4270/
  0x019E, written through `0x354` (data) and `0x358` (0x20 | register);
  RC `0x78` = EP `0x78` & 0x1F.
- The baseband CRC counters (`0xF84` CCK, `0xF94` OFDM) were what showed
  that the radio worked while the MAC dropped frames.

**Transmit and AP mode** [measured, examples/26, 29, sdk/src/wl.c]:
- TX descriptors (64-byte stride, 40 used) must be chained into a circular
  list through word 10 (next-descriptor address); the engine stops after one
  frame otherwise. Kick a queue with `0x300` = `1 << queue` (BE = bit 1,
  beacon = bit 4). Management frames: queue_sel 0x12, data 0x00, beacon
  0x10; rate_id 0xC; set BMC (w0 bit 24) for multicast destinations.
- RCR needs **ADF** (bit 11) to receive data frames and ACF (bit 12) for
  control frames; AMF alone passes only management.
- AP mode (MSR = 3) stalls every TX queue unless `0x554` (beacon interval)
  is set and a beacon descriptor is armed. The beacon descriptor at
  `REG_BCNQ_DESA` is persistent: the MAC sends it at each TBTT, sets HISR
  TBDOK (bit 9) and BCNINT (bit 13), and never clears OWN; refill it after
  BCNINT. `TDECTRL[31:24]` counts TBTTs.
- `REG_USTIME_TSF` (0x55C) uses its low 6 bits as MAC clocks per
  microsecond; this board's MAC clock is **40 MHz**, so 0x28 gives a 1 us
  TSF (0x50 is wrong here: it reads as 16 and the TSF runs 2.5x fast).
- Received data from stations: ToDS frames, header 24 bytes (26 with QoS),
  then LLC/SNAP `AA AA 03 00 00 00` + ethertype; Ethernet dst = addr3,
  src = addr2.

**Hardware encryption (AES-CCMP)** [from the Linux driver, handshake and RX
decryption verified on the bench; TX side under test]:
- Key table ("CAM") at `0x670` (command) / `0x674` (write data): 32 entries
  of 8 dwords. Dword 0 = `mac[1]<<24 | mac[0]<<16 | config`, dword 1 =
  `mac[5..2]`, dwords 2-5 = the 128-bit key little-endian, 6-7 zero. Write
  each dword to `0x674`, then `0x670 = BIT31 | BIT16 | (entry*8 + i)`, from
  i = 7 down to 0. Config = `BIT15 (valid) | alg<<2 | keyid`, alg 4 = AES.
  Pairwise key: entry 4, key id 0, MAC = the peer. Group key: entry =
  key id (1..3), MAC = broadcast. `0x670 = BIT31|BIT30` clears all.
- Enable: CR+1 (`0x101`) |= 0x02, SECCFG (`0x680`) = 0xCC (TX encrypt, RX
  decrypt, TX/RX broadcast use the default key).
- TX: driver inserts the 8-byte CCMP header (PN0 PN1 00 0x20|keyid<<6
  PN2..PN5) after the MAC header, sets the Protected bit and TX descriptor
  w1 bits 23:22 = 3; the chip encrypts and appends the 8-byte MIC. RX
  [measured]: the chip decrypts in place and **strips the MIC**; the frame
  still carries the 8-byte CCMP header, so the payload starts at header+8
  and runs to the end of the frame (minus FCS). Verified by a WPA2 station
  getting DHCP, ARP and DNS through a D-Link AP.

## 7. Ethernet switch and CPU port [measured, examples/08, 09, 94, 95]

The switch core lives at 0xBB800000 (register names from Realtek's
boot-code header `include/rtl8196x/asicregs.h` for this exact chip):

| Reg | Addr | Use |
|-----|------|-----|
| MDCIOCR / MDCIOSR | 0xBB804004 / 08 | MDIO: cmd = (write<<31) \| phy<<24 \| reg<<16 \| data, poll bit31 |
| PCRP0..4 | 0xBB804104 + 4n | port config: bit0 EnablePHYIf, bits2:1 max len, bit3 MacSwReset (1 = normal), bits 31:26 PHY id |
| PSRP0..4 | 0xBB804128 + 4n | port status: bit4 link, bit3 full duplex, bits1:0 speed (0=10, 1=100) |
| SIRR | 0xBB804204 | bit0 TRXRDY |
| FFCR | 0xBB804428 | bit1/0 unknown unicast/multicast to CPU (loader sets 3) |
| CPUICR | 0xB8010000 | bit31 TX en, bit30 RX en, bits 26:24 mbuf size (4 = 2048), bit23 TX kick |
| CPURPDCR0..5 | 0xB8010004.. | RX pkthdr ring bases, CPURMDCR0 0xB801001C RX mbuf ring, CPUTPDCR0/1 0xB8010020/24 TX rings |
| CPUIIMR / CPUIISR | 0xB8010028 / 2C | NIC interrupt mask/status (bits 3..8 RX done, 9..10 TX done, 17.. runout) |

Descriptor rings are arrays of 32-bit words: pointer to a `pkthdr` (KSEG1
address works, the vendor code uses them too) OR'ed with bit0 = owned by
switch, bit1 = wrap. The pkthdr/mbuf layout is in `sdk/include/swcore.h`;
on RX `ph_portlist` holds the **source port number**, on TX it is the
destination port mask. The hardware appends the CRC; `ph_len` must be
`len + 4` (64 for short frames).

When booted via the interactive `<RealTek>` console (`ESC` pressed), the
root loader calls `eth_startup()` (`swCore_init()` + PHY analog calibration
+ VLAN 8 with all ports untagged + L2 trap of unknown unicast/multicast to
CPU via `FFCR = 3`), and its `J` command clears `EnablePHYIf` in `PCRP0..4`
before jumping. **Cold-boot trap:** when the root loader boots directly
from SPI flash at power-on without `ESC` (`Jump to image start=0xa0500000...`),
it skips `eth_startup()` completely and leaves the switch ASIC and PHYs
uninitialized (`FFCR = 0`). `eth_init()` in `sdk/src/swcore.c` checks
`SW_FFCR` and runs the full switch ASIC reset, PHY calibration, table
initialization, VLAN 8 / Netif 0 setup, and `FFCR = 3` whenever cold-booted
directly from SPI flash. Note: `our_mac` must NOT be added to L2 `Table 0`
with a LAN portmask, so `FFCR = 3` traps all unicast frames for `our_mac`
to the CPU port.

Board facts: PHYs are Realtek id 001c:c880; switch port 3 is the connector
labelled LAN4 (the stock firmware maps LAN to ports 0..3 and WAN to port
4); interrupt source 8 is the switch NIC (the loader routes it to IP3).
Ping round trip from a PC is under 1 ms with the polled driver.

## 8. Boot loader

Banner [measured]:

```
Booting...
========== SPI =============
---RealTek(RTL8196C)at 2013.05.22-07:15+0800 version v1.1f [16bit](390MHz)
DRAM Spreading Spectrum: [ON] TRX Timing: [T:4 R:6]
<=== GPIOA4 off/on 9/1 2201143 times ===>
Jump to image start=0x80500000...
```

Command line (prompt `<RealTek>`), entered with ESC during the boot
window. Useful commands [article, OpenWrt wiki]:

| Command | Syntax | Use |
|---------|--------|-----|
| EW | `EW <addr> <w0> [w1 ...]` | write 32-bit words |
| DW | `DW <addr> <nwords>` | dump words (rounded up to 4) |
| J | `J <addr>` | jump |
| LOADADDR | `LOADADDR <addr>` | TFTP target address |
| FLR / FLW | | flash read / write (dangerous) |
| HELP | | list |

The loader's own entry status on our board is still open: ESC sent at
"Booting..." and ESC flooded from power-on both failed to stop the boot,
and the running Linux console ignores typed commands as well. Either the
stock firmware has no serial shell and the ESC window is very short, or
the adapter's TX line is not reaching the SoC. The next step is a
hardware check of the TX wire (see docs/02-bring-up-log.md).

## 9. Sources

- Elektroda article (p.kaczmarek2): https://www.elektroda.pl/news/news4179473.html
- Realtek RTL8196E BSP header: https://github.com/yshifeng41/rtl819x/blob/master/rtl819x/boards/rtl8196e/bsp/bspchip.h
- RTL8196E bare-metal probes (Lexra vector, CCTL, rfe): https://github.com/Jhongwe1/router-customFW
- Lexra LX4189 datasheet (no lwl/lwr): https://doc.lagout.org/electronics/doc/lexra/lx4189-datasheet-1.9-c20081030.pdf
- RLX4181 Linux port notes: https://github.com/shibajee/linux-rtl8196e
- OpenWrt wiki, Realtek loader commands: https://oldwiki.archive.openwrt.org/doc/techref/bootloader/realtek
