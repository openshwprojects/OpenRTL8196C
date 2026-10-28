/*
 * 31_uart_flasher - SPI flash read/write stub for BK7231GUIFlashTool.
 *
 * Loaded with EW through the <RealTek> loader console and started with J,
 * like every other example.  It then speaks the GUI flasher's common stub
 * protocol (the one ECRBaseFlasher.cs implements for LN882H, TR6260, W800 ...)
 * on UART0:
 *
 *   request  A5 type len_lo len_hi params[len] sum8
 *   reply    5A type len_lo len_hi data[len] status sum8
 *
 * sum8 is the byte sum of everything before it, params and data are little
 * endian.  Bulk transfers follow the reply as XMODEM-1K (CRC16): the stub
 * sends for reads and receives for writes.  The first 64 KB (loader, H601/6V03
 * settings) can never be erased or written: spiflash.c refuses it and the
 * stub answers ADDR_ERROR.  PC side for testing: python tools/uartflash.py
 *
 * Build it cached, uncached code is too slow to receive above 230400 baud:
 *   python tools/build.py examples/31_uart_flasher --load 0x80500000 --stack 0x80600000
 * then EW it to the uncached alias 0xA0500000 (so it reaches RAM, not the
 * D-cache) and start it with J 80500000.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "spiflash.h"
#include "libc.h"

#define CMD_FLASH_ERASE   0x04
#define CMD_BAUD          0x07
#define CMD_CHIP_INFO     0x20
#define CMD_CRC32         0x8F
#define CMD_FLASH_ID      0x90
#define CMD_XMODEM_WRITE  0x91
#define CMD_XMODEM_READ   0x92
#define CMD_GET_MAC       0x95
#define CMD_REBOOT        0x9F

#define ST_OK             0x00
#define ST_ERROR          0x01
#define ST_ADDR_ERROR     0x02
#define ST_LEN_ERROR      0x04
#define ST_CRC_ERROR      0x05
#define ST_NOT_SUPPORTED  0x10

#define PLATFORM_NAME     "RTL8196C"      /* the GUI's platform id is crc32 of the name */
#define UART_LCR_DLAB     0x80

#define X_SOH 0x01
#define X_STX 0x02
#define X_EOT 0x04
#define X_ACK 0x06
#define X_NAK 0x15
#define X_CAN 0x18
#define X_C   0x43

static uint32_t uart_clk16;               /* UART clock / 16, from the divisor the loader set for 38400 */
static uint8_t  xbuf[1024 + 5];
static uint32_t rx_overrun, rx_parity, rx_framing, rx_break;   /* LSR error bits seen, reported by CHIP_INFO */
static int      baud_probation;           /* new rate not confirmed by a good request yet */
static uint32_t baud_switch_us;

#define IDLE_BAUD         115200u         /* the rate both PC tools return to after every operation */
#define PROBATION_US      2000000u

/* ------------------------------------------------------------------ UART */

static int rx_byte(void)
{
    uint8_t lsr = REG8(UART_LSR);              /* reading LSR clears the error bits */
    if (lsr & 0x1E) {
        if (lsr & 0x02) rx_overrun++;
        if (lsr & 0x04) rx_parity++;
        if (lsr & 0x08) rx_framing++;
        if (lsr & 0x10) rx_break++;
    }
    return (lsr & UART_LSR_DR) ? REG8(UART_RBR) : -1;
}

static int getc_timeout(uint32_t ms)
{
    uint32_t t0 = micros();
    do {
        int c = rx_byte();
        if (c >= 0) return c;
    } while (elapsed_us(t0) < ms * 1000u);
    return -1;
}

static void put_bytes(const uint8_t *p, uint32_t n)
{
    while (n--) uart_putc((char)*p++);
}

static void drain_tx(void)
{
    while (!(REG8(UART_LSR) & UART_LSR_TEMT))
        ;
}

static void flush_rx(uint32_t quiet_ms)
{
    while (getc_timeout(quiet_ms) >= 0)
        ;
}

static uint32_t get_divisor(void)
{
    uint8_t lcr = REG8(UART_LCR);
    REG8(UART_LCR) = lcr | UART_LCR_DLAB;
    uint32_t div = REG8(UART_DLL) | ((uint32_t)REG8(UART_DLM) << 8);
    REG8(UART_LCR) = lcr;
    return div;
}

static void set_divisor(uint32_t div)
{
    uint8_t lcr = REG8(UART_LCR);
    REG8(UART_LCR) = lcr | UART_LCR_DLAB;
    REG8(UART_DLL) = (uint8_t)div;
    REG8(UART_DLM) = (uint8_t)(div >> 8);
    REG8(UART_LCR) = lcr;
}

/* -------------------------------------------------------------- framing */

static uint32_t rd32le(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void reply(uint8_t type, uint8_t status, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[4] = { 0x5A, type, (uint8_t)len, (uint8_t)(len >> 8) };
    uint8_t sum = 0;
    for (int i = 0; i < 4; i++) sum += hdr[i];
    for (uint32_t i = 0; i < len; i++) sum += data[i];
    sum += status;
    put_bytes(hdr, 4);
    put_bytes(data, len);
    uart_putc((char)status);
    uart_putc((char)sum);
}

/* Wait for one request.  Returns its type, -1 on a broken frame (already answered).
 * After a baud change that the PC cannot use, no good request arrives: the stub
 * then falls back to IDLE_BAUD by itself, where the PC will look for it. */
static int get_request(uint8_t *params, uint32_t *plen)
{
    int c;
    while ((c = rx_byte()) != 0xA5) {
        if (baud_probation && elapsed_us(baud_switch_us) > PROBATION_US) {
            set_divisor((uart_clk16 + IDLE_BAUD / 2) / IDLE_BAUD);
            baud_probation = 0;
        }
    }
    uint8_t hdr[3];
    for (int i = 0; i < 3; i++) {
        if ((c = getc_timeout(200)) < 0) return -1;
        hdr[i] = (uint8_t)c;
    }
    uint32_t len = hdr[1] | ((uint32_t)hdr[2] << 8);
    if (len > 64) {
        flush_rx(20);
        reply(hdr[0], ST_LEN_ERROR, 0, 0);
        return -1;
    }
    uint8_t sum = (uint8_t)(0xA5 + hdr[0] + hdr[1] + hdr[2]);
    for (uint32_t i = 0; i < len; i++) {
        if ((c = getc_timeout(200)) < 0) return -1;
        params[i] = (uint8_t)c;
        sum += (uint8_t)c;
    }
    if ((c = getc_timeout(200)) < 0) return -1;
    if ((uint8_t)c != sum) {
        reply(hdr[0], ST_CRC_ERROR, 0, 0);
        return -1;
    }
    baud_probation = 0;
    *plen = len;
    return hdr[0];
}

/* ---------------------------------------------------------------- XMODEM */

static uint16_t crc16_xmodem(const uint8_t *p, uint32_t n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void send_cancel(void)
{
    for (int i = 0; i < 5; i++) uart_putc(X_CAN);
}

/* Send [addr, addr+len) from the flash window as XMODEM-1K. 0 = done. */
static int xmodem_send(uint32_t addr, uint32_t len)
{
    int c;
    uint32_t t0 = micros();
    do {                                          /* receiver asks with 'C' (or NAK) */
        c = getc_timeout(100);
        if (c == X_CAN) return -1;
        if (elapsed_us(t0) > 10000000u) return -1;
    } while (c != X_C && c != X_NAK);

    uint8_t blk = 1;
    for (uint32_t done = 0; done < len; done += 1024, blk++) {
        uint32_t n = len - done < 1024 ? len - done : 1024;
        xbuf[0] = X_STX;
        xbuf[1] = blk;
        xbuf[2] = (uint8_t)~blk;
        spiflash_read(addr + done, xbuf + 3, n);
        memset(xbuf + 3 + n, 0xFF, 1024 - n);
        uint16_t crc = crc16_xmodem(xbuf + 3, 1024);
        xbuf[1027] = (uint8_t)(crc >> 8);
        xbuf[1028] = (uint8_t)crc;
        for (int tries = 0;; tries++) {
            if (tries == 10) { send_cancel(); return -1; }
            put_bytes(xbuf, sizeof xbuf);
            do c = getc_timeout(3000);           /* stray 'C' from the start phase are skipped */
            while (c == X_C);
            if (c == X_ACK) break;
            if (c == X_CAN) return -1;
        }
    }
    for (int tries = 0; tries < 10; tries++) {
        uart_putc(X_EOT);
        do c = getc_timeout(1000);
        while (c == X_C);
        if (c == X_ACK) return 0;
    }
    return -1;
}

/* Receive XMODEM-1K (or 128-byte packets) and program it at addr; addr is
 * sector aligned, each sector is erased when the first byte for it arrives.
 * Bytes past len (XMODEM padding) are dropped. 0 = done. */
static int xmodem_receive(uint32_t addr, uint32_t len)
{
    int c = -1;
    for (int tries = 0; tries < 60 && c < 0; tries++) {
        uart_putc(X_C);
        c = getc_timeout(500);
    }
    uint8_t expect = 1;
    uint32_t done = 0;
    for (;;) {
        if (c < 0) c = getc_timeout(3000);
        if (c == X_EOT) { uart_putc(X_ACK); return done >= len ? 0 : -1; }
        if (c == X_CAN) return -1;
        uint32_t size = c == X_STX ? 1024 : c == X_SOH ? 128 : 0;
        if (!size) {                              /* timeout or noise: resync */
            flush_rx(50);
            uart_putc(X_NAK);
            c = -1;
            continue;
        }
        int ok = 1;
        for (uint32_t i = 0; i < size + 4; i++) {
            if ((c = getc_timeout(1000)) < 0) { ok = 0; break; }
            xbuf[i] = (uint8_t)c;
        }
        c = -1;
        uint8_t *d = xbuf + 2;
        if (!ok || (uint8_t)(xbuf[0] + xbuf[1]) != 0xFF ||
            crc16_xmodem(d, size) != (uint16_t)((d[size] << 8) | d[size + 1])) {
            flush_rx(50);
            uart_putc(X_NAK);
            continue;
        }
        if (xbuf[0] == (uint8_t)(expect - 1)) {   /* our ACK was lost */
            uart_putc(X_ACK);
            continue;
        }
        if (xbuf[0] != expect) {
            flush_rx(50);
            uart_putc(X_NAK);
            continue;
        }
        uint32_t n = len - done < size ? len - done : size;
        uint32_t off = addr + done;
        for (uint32_t s = off; s < off + n; s = (s | (SPIFLASH_SECTOR - 1)) + 1)
            if ((s & (SPIFLASH_SECTOR - 1)) == 0 && spiflash_erase_sector(s) < 0) { send_cancel(); return -1; }
        if (n && (spiflash_write(off, d, n) < 0 || spiflash_verify(off, d, n) < 0)) {
            send_cancel();
            return -1;
        }
        done += n;
        expect++;
        uart_putc(X_ACK);
    }
}

/* -------------------------------------------------------------- commands */

static int range_ok(uint32_t addr, uint32_t len, int writing)
{
    if (len == 0 || addr >= SPIFLASH_SIZE || len > SPIFLASH_SIZE - addr) return 0;
    if (writing && (addr < SPIFLASH_PROTECTED_END || (addr & (SPIFLASH_SECTOR - 1)))) return 0;
    return 1;
}

static void do_baud(uint8_t type, uint32_t baud)
{
    uint32_t div = baud ? (uart_clk16 + baud / 2) / baud : 0;
    uint32_t real = div ? uart_clk16 / div : 0;
    uint32_t err = real > baud ? real - baud : baud - real;
    if (!div || div > 0xFFFF || err * 100u > baud * 5u) {
        reply(type, ST_NOT_SUPPORTED, 0, 0);      /* PC has switched already, it will time out */
        return;
    }
    drain_tx();
    delay_ms(30);                                 /* the PC sets its new rate after sending */
    set_divisor(div);
    flush_rx(5);
    reply(type, ST_OK, 0, 0);
    baud_probation = 1;
    baud_switch_us = micros();
}

static void handle(int type, const uint8_t *p, uint32_t plen)
{
    uint8_t out[24];
    uint32_t addr = plen >= 4 ? rd32le(p) : 0;
    uint32_t len  = plen >= 8 ? rd32le(p + 4) : 0;

    switch (type) {
    case CMD_FLASH_ID: {
        uint32_t id = spiflash_jedec_id();
        out[0] = (uint8_t)(id >> 16); out[1] = (uint8_t)(id >> 8); out[2] = (uint8_t)id; out[3] = 0;
        reply(type, ST_OK, out, 4);
        break;
    }
    case CMD_CHIP_INFO:
        wr32le(out, crc32(PLATFORM_NAME, sizeof PLATFORM_NAME - 1));
        wr32le(out + 4, uart_clk16);
        wr32le(out + 8, rx_overrun);
        wr32le(out + 12, rx_parity);
        wr32le(out + 16, rx_framing);
        wr32le(out + 20, rx_break);
        reply(type, ST_OK, out, 24);
        break;
    case CMD_GET_MAC:
        spiflash_factory_mac(out);
        reply(type, ST_OK, out, 6);
        break;
    case CMD_BAUD:
        do_baud((uint8_t)type, addr);
        break;
    case CMD_CRC32:
        if (!range_ok(addr, len, 0)) { reply(type, ST_ADDR_ERROR, 0, 0); break; }
        wr32le(out, crc32((const void *)(SPIFLASH_MMAP + addr), len));
        reply(type, ST_OK, out, 4);
        break;
    case CMD_FLASH_ERASE:
        if (!range_ok(addr, len, 1)) { reply(type, ST_ADDR_ERROR, 0, 0); break; }
        reply(type, spiflash_erase_range(addr, len) < 0 ? ST_ERROR : ST_OK, 0, 0);
        break;
    case CMD_XMODEM_READ:
        if (!range_ok(addr, len, 0)) { reply(type, ST_ADDR_ERROR, 0, 0); break; }
        reply(type, ST_OK, 0, 0);
        xmodem_send(addr, len);
        break;
    case CMD_XMODEM_WRITE:
        if (!range_ok(addr, len, 1)) { reply(type, ST_ADDR_ERROR, 0, 0); break; }
        reply(type, ST_OK, 0, 0);
        xmodem_receive(addr, len);
        break;
    case CMD_REBOOT:
        reply(type, ST_OK, 0, 0);
        drain_tx();
        wdt_reboot();
        break;
    default:
        reply(type, ST_NOT_SUPPORTED, 0, 0);
        break;
    }
}

int main(void)
{
    uint8_t params[64];
    uint32_t plen = 0;

    uart_init();
    timer_delay_init();
    uart_clk16 = get_divisor() * 38400u;
    uart_puts("\r\nRTL8196C flasher stub ready\r\n");

    for (;;) {
        int type = get_request(params, &plen);
        if (type >= 0)
            handle(type, params, plen);
    }
}
