/*
 * 30_ram_exec - Execution & Cache Profiling on Realtek RTL8196C (390 MHz RLX4181)
 *
 * Compares code execution and data access speeds across all hardware memory aliases:
 *   1. SPI NOR Flash XIP Uncached (0xBDxxxxxx code, KSEG1 0xA0xxxxxx stack/data)
 *   2. SPI NOR Flash XIP I-Cached (0x9Dxxxxxx code, KSEG1 0xA0xxxxxx stack/data)
 *   3. SPI NOR Flash XIP + D-Cache(0x9Dxxxxxx code, KSEG0 0x80xxxxxx stack/data)
 *   4. SDRAM Uncached (KSEG1)     (0xA050xxxx code, KSEG1 0xA0xxxxxx stack/data) [SDK default]
 *   5. SDRAM I-Cache Only         (0x8050xxxx code, KSEG1 0xA0xxxxxx stack/data)
 *   6. SDRAM D-Cache Only         (0xA050xxxx code, KSEG0 0x80xxxxxx stack/data)
 *   7. SDRAM Full Cached (KSEG0)  (0x8050xxxx code, KSEG0 0x80xxxxxx stack/data) [I$ + D$]
 *
 * Benchmarks:
 *   - Tight Integer ALU / Branch loop (10M iterations)
 *   - Memory copy (memcpy, 4 MB) & fill (memset, 4 MB)
 *   - CRC32 checksum (1 MB)
 *   - SHA-1 hash (256 KB)
 *   - WPA2-PSK PMK derivation (PBKDF2-HMAC-SHA1, 4096 iterations)
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "irq.h"
#include "wpa_crypto.h"
#include "libc.h"

#define PHYS_ADDR(x)  ((uintptr_t)(x) & 0x1FFFFFFFu)
#define TO_KSEG0(x)   (PHYS_ADDR(x) | 0x80000000u)
#define TO_KSEG1(x)   (PHYS_ADDR(x) | 0xA0000000u)

/* Dedicated 64 KB stack & 64 KB scratch buffers in SDRAM */
static uint8_t alt_stack[65536] __attribute__((aligned(32)));
static uint8_t buf_src[32768]   __attribute__((aligned(32)));
static uint8_t buf_dst[32768]   __attribute__((aligned(32)));

static void flush_caches(void)
{
    cctl_write(CCTL_DWBINVAL);
    cctl_write(CCTL_IINVAL);
}

/*
 * Switch $sp to new_sp and invoke fn_addr(a0, a1, a2, a3).
 */
__attribute__((noinline))
static uint32_t call_with_sp(uintptr_t fn_addr, uintptr_t new_sp,
                             uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    uint32_t ret;
    __asm__ volatile (
        ".set push\n\t"
        ".set noreorder\n\t"
        "move   $s0, $sp\n\t"
        "addiu  $sp, %2, -32\n\t"
        "move   $a0, %3\n\t"
        "move   $a1, %4\n\t"
        "move   $a2, %5\n\t"
        "jalr   %1\n\t"
        "move   $a3, %6\n\t"
        "move   $sp, $s0\n\t"
        "move   %0, $v0\n\t"
        ".set pop\n\t"
        : "=r"(ret)
        : "r"(fn_addr), "r"(new_sp), "r"(a0), "r"(a1), "r"(a2), "r"(a3)
        : "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
          "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7", "$t8", "$t9",
          "$s0", "$ra", "memory"
    );
    return ret;
}

/* --------------------------------------------------------------------------
 * Benchmark kernels (self-contained / position-independent where noted)
 * -------------------------------------------------------------------------- */

/* 1. Pure ALU + branch loop (no memory accesses inside the loop) */
__attribute__((noinline))
static uint32_t bench_alu(uintptr_t iters, uintptr_t seed, uintptr_t u2, uintptr_t u3)
{
    (void)u2; (void)u3;
    uint32_t x = (uint32_t)seed ? (uint32_t)seed : 0x12345678u;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < (uint32_t)iters; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        acc += x ^ i;
    }
    return acc;
}

/* 2. Repeated memcpy over 32 KB buffer */
__attribute__((noinline))
static uint32_t bench_memcpy_loop(uintptr_t dst, uintptr_t src, uintptr_t bytes, uintptr_t loops)
{
    for (uint32_t i = 0; i < (uint32_t)loops; i++) {
        memcpy((void *)dst, (const void *)src, (size_t)bytes);
    }
    return ((const uint32_t *)dst)[0] ^ ((const uint32_t *)dst)[(bytes / 4) - 1];
}

/* 3. Repeated memset over 32 KB buffer */
__attribute__((noinline))
static uint32_t bench_memset_loop(uintptr_t dst, uintptr_t val, uintptr_t bytes, uintptr_t loops)
{
    for (uint32_t i = 0; i < (uint32_t)loops; i++) {
        memset((void *)dst, (int)(val + i), (size_t)bytes);
    }
    return ((const uint32_t *)dst)[0];
}

/* 4. Self-contained 16-entry nibble-table CRC32 (table lives on current stack) */
__attribute__((noinline))
static uint32_t bench_crc32_stack(uintptr_t data, uintptr_t bytes, uintptr_t loops, uintptr_t unused)
{
    (void)unused;
    uint32_t tbl[16];
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t c = i;
        for (int b = 0; b < 4; b++)
            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
        tbl[i] = c;
    }

    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t l = 0; l < (uint32_t)loops; l++) {
        const uint32_t *wp = (const uint32_t *)data;
        uint32_t n = (uint32_t)bytes / 4u;
        while (n--) {
            uint32_t w = *wp++;
            uint8_t b0 = (uint8_t)(w >> 24);
            crc = (crc >> 4) ^ tbl[(crc ^ b0) & 0x0Fu];
            crc = (crc >> 4) ^ tbl[(crc ^ (b0 >> 4)) & 0x0Fu];
            uint8_t b1 = (uint8_t)(w >> 16);
            crc = (crc >> 4) ^ tbl[(crc ^ b1) & 0x0Fu];
            crc = (crc >> 4) ^ tbl[(crc ^ (b1 >> 4)) & 0x0Fu];
            uint8_t b2 = (uint8_t)(w >> 8);
            crc = (crc >> 4) ^ tbl[(crc ^ b2) & 0x0Fu];
            crc = (crc >> 4) ^ tbl[(crc ^ (b2 >> 4)) & 0x0Fu];
            uint8_t b3 = (uint8_t)w;
            crc = (crc >> 4) ^ tbl[(crc ^ b3) & 0x0Fu];
            crc = (crc >> 4) ^ tbl[(crc ^ (b3 >> 4)) & 0x0Fu];
        }
    }
    return ~crc;
}

/* 5. Repeated SHA-1 over 32 KB buffer */
__attribute__((noinline))
static uint32_t bench_sha1_loop(uintptr_t data, uintptr_t bytes, uintptr_t loops, uintptr_t out_ptr)
{
    uint8_t *out = (uint8_t *)out_ptr;
    for (uint32_t i = 0; i < (uint32_t)loops; i++) {
        sha1((const uint8_t *)data, (uint32_t)bytes, out);
    }
    return ((uint32_t)out[0] << 24) | ((uint32_t)out[1] << 16) | ((uint32_t)out[2] << 8) | out[3];
}

/* 6. WPA2-PSK PMK (PBKDF2-HMAC-SHA1, 4096 iterations) */
__attribute__((noinline))
static uint32_t bench_wpa_pmk(uintptr_t pass, uintptr_t ssid, uintptr_t ssid_len, uintptr_t out_pmk)
{
    wpa_pmk((const char *)pass, (const uint8_t *)ssid, (uint32_t)ssid_len, (uint8_t *)out_pmk);
    const uint8_t *p = (const uint8_t *)out_pmk;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/*
 * Locate a compiled leaf function in the SPI NOR flash window (0xBD010000..0xBD300000)
 * by matching its first 10 machine-code words (40 bytes).
 */
static uintptr_t find_fn_in_spiflash(const void *ram_fn)
{
    const uint32_t *needle = (const uint32_t *)TO_KSEG1(ram_fn);
    const uint32_t *flash_start = (const uint32_t *)0xBD010000u;
    const uint32_t *flash_end   = (const uint32_t *)0xBD300000u;

    uint32_t w0 = needle[0], w1 = needle[1], w2 = needle[2], w3 = needle[3];
    for (const uint32_t *p = flash_start; p + 16 < flash_end; p++) {
        if (p[0] == w0 && p[1] == w1 && p[2] == w2 && p[3] == w3) {
            int match = 1;
            for (int k = 4; k < 10; k++) {
                if (p[k] != needle[k]) {
                    match = 0;
                    break;
                }
            }
            if (match)
                return (uintptr_t)p;
        }
    }
    return 0;
}

static void print_speedup(uint32_t base_us, uint32_t cur_us)
{
    if (cur_us == 0) cur_us = 1;
    while (base_us > 40000000u) {
        base_us >>= 4;
        cur_us  = (cur_us >> 4) ? (cur_us >> 4) : 1u;
    }
    uint32_t ratio_x100 = (base_us * 100u + (cur_us / 2u)) / cur_us;
    printf("%2u.%02ux", ratio_x100 / 100u, ratio_x100 % 100u);
}

static void print_mbps(uint32_t total_kb, uint32_t elapsed_us)
{
    if (elapsed_us == 0) elapsed_us = 1;
    /* KB/s = (total_kb * 1,000,000) / elapsed_us; scale to fit 32-bit uint */
    uint32_t num = total_kb * 1000u;
    uint32_t den = (elapsed_us + 500u) / 1000u;
    if (den == 0) den = 1;
    uint32_t kbps = num / den;
    printf("%3u.%02u MB/s", kbps / 1024u, ((kbps % 1024u) * 100u) / 1024u);
}

int main(void)
{
    uart_init();
    irq_init();
    timer_delay_init();

    printf("\n==============================================================================\n");
    printf("30_ram_exec: RTL8196C (390 MHz RLX4181) RAM & Cache Execution Profiler\n");
    printf("==============================================================================\n");
    printf("Image linked at: 0x%08x  PRId: 0x%08x  Status: 0x%08x\n",
           (unsigned)&main, cp0_prid_read(), cp0_status_read());

    /* Initialize test buffer in physical RAM and flush to SDRAM */
    for (uint32_t i = 0; i < sizeof(buf_src); i++)
        ((uint8_t *)TO_KSEG1(buf_src))[i] = (uint8_t)(i * 37u + 13u);
    flush_caches();

    uintptr_t sp_kseg1 = TO_KSEG1(alt_stack + sizeof(alt_stack));
    uintptr_t sp_kseg0 = TO_KSEG0(alt_stack + sizeof(alt_stack));

    uintptr_t src_kseg1 = TO_KSEG1(buf_src);
    uintptr_t dst_kseg1 = TO_KSEG1(buf_dst);
    uintptr_t src_kseg0 = TO_KSEG0(buf_src);
    uintptr_t dst_kseg0 = TO_KSEG0(buf_dst);

    /* ----------------------------------------------------------------------
     * Part 1: SPI NOR Flash XIP (0xBD / 0x9D) vs Uncached/Cached SDRAM
     * ---------------------------------------------------------------------- */
    uintptr_t flash_memcpy = find_fn_in_spiflash((const void *)&memcpy);
    printf("\n--- Part 1: SPI Flash XIP (0xBD/0x9D) vs SDRAM Execution (memcpy 1 MB) ---\n");
    if (flash_memcpy) {
        printf("Found position-independent memcpy() in SPI flash at 0x%08x\n", (unsigned)flash_memcpy);
        struct {
            const char *name;
            uintptr_t   fn;
            uintptr_t   sp;
            uintptr_t   dst;
            uintptr_t   src;
        } xip_modes[] = {
            { "1. SPI Flash Uncached (0xBD code, KSEG1 data)", flash_memcpy,                       sp_kseg1, dst_kseg1, src_kseg1 },
            { "2. SPI Flash I-Cached (0x9D code, KSEG1 data)", (flash_memcpy & ~0x20000000u),      sp_kseg1, dst_kseg1, src_kseg1 },
            { "3. SPI Flash + D-Cache(0x9D code, KSEG0 data)", (flash_memcpy & ~0x20000000u),      sp_kseg0, dst_kseg0, src_kseg0 },
            { "4. RAM Uncached KSEG1 (0xA0 code, KSEG1 data)", TO_KSEG1(&memcpy),                  sp_kseg1, dst_kseg1, src_kseg1 },
            { "5. RAM I-Cache Only   (0x80 code, KSEG1 data)", TO_KSEG0(&memcpy),                  sp_kseg1, dst_kseg1, src_kseg1 },
            { "6. RAM D-Cache Only   (0xA0 code, KSEG0 data)", TO_KSEG1(&memcpy),                  sp_kseg0, dst_kseg0, src_kseg0 },
            { "7. RAM Full Cached    (0x80 code, KSEG0 data)", TO_KSEG0(&memcpy),                  sp_kseg0, dst_kseg0, src_kseg0 },
        };
        uint32_t base_us = 0;
        for (unsigned m = 0; m < sizeof(xip_modes) / sizeof(xip_modes[0]); m++) {
            flush_caches();
            uint32_t t0 = micros();
            for (int r = 0; r < 32; r++) {
                call_with_sp(xip_modes[m].fn, xip_modes[m].sp,
                             xip_modes[m].dst, xip_modes[m].src, 32768u, 0);
            }
            uint32_t dt = micros() - t0;
            flush_caches();
            if (m == 0) base_us = dt;
            printf("  %-46s : %7u us  (", xip_modes[m].name, dt);
            print_mbps(1024u, dt);
            printf(", ");
            print_speedup(base_us, dt);
            printf(")\n");
        }
    } else {
        printf("  (memcpy pattern not found in SPI flash window)\n");
    }

    /* ----------------------------------------------------------------------
     * Part 2: 4-Mode SDRAM Execution Matrix across 6 Workloads
     *   Mode A: Uncached KSEG1 (0xA0 code + 0xA0 stack/data) [Default]
     *   Mode B: I-Cache Only   (0x80 code + 0xA0 stack/data)
     *   Mode C: D-Cache Only   (0xA0 code + 0x80 stack/data)
     *   Mode D: Full KSEG0     (0x80 code + 0x80 stack/data) [I$ + D$]
     * ---------------------------------------------------------------------- */
    printf("\n--- Part 2: SDRAM Execution Matrix (KSEG1 Uncached vs KSEG0 Cached) ---\n");
    printf("%-28s | %-12s | %-12s | %-12s | %-12s | %-8s\n",
           "Workload", "KSEG1 (Unc)", "I-Cache Only", "D-Cache Only", "Full KSEG0", "Speedup");
    printf("-----------------------------+--------------+--------------+--------------+--------------+---------\n");

    uint32_t us[4];
    uint32_t res[4];

    /* 2.1: ALU Loop (10,000,000 iterations) */
    for (int m = 0; m < 4; m++) {
        uintptr_t fn = (m & 1) ? TO_KSEG0(&bench_alu) : TO_KSEG1(&bench_alu);
        uintptr_t sp = (m & 2) ? sp_kseg0 : sp_kseg1;
        flush_caches();
        uint32_t t0 = micros();
        res[m] = call_with_sp(fn, sp, 10000000u, 0x13572468u, 0, 0);
        us[m] = micros() - t0;
        flush_caches();
    }
    printf("%-28s | %9u us | %9u us | %9u us | %9u us | ",
           "ALU/Branch (10M iters)", us[0], us[1], us[2], us[3]);
    print_speedup(us[0], us[3]);
    printf("  [chk=%08x]\n", res[3]);

    /* 2.2: memcpy 4 MB (32 KB x 128) */
    for (int m = 0; m < 4; m++) {
        uintptr_t fn  = (m & 1) ? TO_KSEG0(&bench_memcpy_loop) : TO_KSEG1(&bench_memcpy_loop);
        uintptr_t sp  = (m & 2) ? sp_kseg0 : sp_kseg1;
        uintptr_t dst = (m & 2) ? dst_kseg0 : dst_kseg1;
        uintptr_t src = (m & 2) ? src_kseg0 : src_kseg1;
        flush_caches();
        uint32_t t0 = micros();
        res[m] = call_with_sp(fn, sp, dst, src, 32768u, 128u);
        us[m] = micros() - t0;
        flush_caches();
    }
    printf("%-28s | %9u us | %9u us | %9u us | %9u us | ",
           "memcpy (4 MB)", us[0], us[1], us[2], us[3]);
    print_speedup(us[0], us[3]);
    printf("  [");
    print_mbps(4096u, us[0]);
    printf(" -> ");
    print_mbps(4096u, us[3]);
    printf("]\n");

    /* 2.3: memset 4 MB (32 KB x 128) */
    for (int m = 0; m < 4; m++) {
        uintptr_t fn  = (m & 1) ? TO_KSEG0(&bench_memset_loop) : TO_KSEG1(&bench_memset_loop);
        uintptr_t sp  = (m & 2) ? sp_kseg0 : sp_kseg1;
        uintptr_t dst = (m & 2) ? dst_kseg0 : dst_kseg1;
        flush_caches();
        uint32_t t0 = micros();
        res[m] = call_with_sp(fn, sp, dst, 0x5Au, 32768u, 128u);
        us[m] = micros() - t0;
        flush_caches();
    }
    printf("%-28s | %9u us | %9u us | %9u us | %9u us | ",
           "memset (4 MB)", us[0], us[1], us[2], us[3]);
    print_speedup(us[0], us[3]);
    printf("  [");
    print_mbps(4096u, us[0]);
    printf(" -> ");
    print_mbps(4096u, us[3]);
    printf("]\n");

    /* 2.4: CRC32 1 MB (32 KB x 32) */
    for (int m = 0; m < 4; m++) {
        uintptr_t fn  = (m & 1) ? TO_KSEG0(&bench_crc32_stack) : TO_KSEG1(&bench_crc32_stack);
        uintptr_t sp  = (m & 2) ? sp_kseg0 : sp_kseg1;
        uintptr_t src = (m & 2) ? src_kseg0 : src_kseg1;
        flush_caches();
        uint32_t t0 = micros();
        res[m] = call_with_sp(fn, sp, src, 32768u, 32u, 0);
        us[m] = micros() - t0;
        flush_caches();
    }
    printf("%-28s | %9u us | %9u us | %9u us | %9u us | ",
           "CRC32 (1 MB)", us[0], us[1], us[2], us[3]);
    print_speedup(us[0], us[3]);
    printf("  [crc=%08x match=%d]\n", res[3], (res[0] == res[3]));

    /* 2.5: SHA-1 256 KB (32 KB x 8) */
    for (int m = 0; m < 4; m++) {
        uintptr_t fn  = (m & 1) ? TO_KSEG0(&bench_sha1_loop) : TO_KSEG1(&bench_sha1_loop);
        uintptr_t sp  = (m & 2) ? sp_kseg0 : sp_kseg1;
        uintptr_t src = (m & 2) ? src_kseg0 : src_kseg1;
        uintptr_t out = (m & 2) ? dst_kseg0 : dst_kseg1;
        flush_caches();
        uint32_t t0 = micros();
        res[m] = call_with_sp(fn, sp, src, 32768u, 8u, out);
        us[m] = micros() - t0;
        flush_caches();
    }
    printf("%-28s | %9u us | %9u us | %9u us | %9u us | ",
           "SHA-1 (256 KB)", us[0], us[1], us[2], us[3]);
    print_speedup(us[0], us[3]);
    printf("  [");
    print_mbps(256u, us[0]);
    printf(" -> ");
    print_mbps(256u, us[3]);
    printf("]\n");

    /* 2.6: WPA2-PSK PMK (PBKDF2-HMAC-SHA1, 4096 iterations) */
    const char *test_pw   = "password";
    const char *test_ssid = "IEEE";
    memcpy((void *)dst_kseg1, test_pw, 9);
    memcpy((void *)(dst_kseg1 + 64), test_ssid, 5);
    flush_caches();

    for (int m = 0; m < 4; m++) {
        uintptr_t fn   = (m & 1) ? TO_KSEG0(&bench_wpa_pmk) : TO_KSEG1(&bench_wpa_pmk);
        uintptr_t sp   = (m & 2) ? sp_kseg0 : sp_kseg1;
        uintptr_t base = (m & 2) ? dst_kseg0 : dst_kseg1;
        flush_caches();
        uint32_t t0 = micros();
        res[m] = call_with_sp(fn, sp, base, base + 64, 4u, base + 128);
        us[m] = micros() - t0;
        flush_caches();
    }
    /* IEEE 802.11i test vector: passphrase="password", ssid="IEEE" -> f42c6fc52df0ebef... */
    printf("%-28s | %8u ms  | %8u ms  | %8u ms  | %8u ms  | ",
           "WPA2 PMK (PBKDF2 x4096)",
           us[0] / 1000u, us[1] / 1000u, us[2] / 1000u, us[3] / 1000u);
    print_speedup(us[0], us[3]);
    printf("  [pmk=%08x ok=%d]\n", res[3], (res[3] == 0xf42c6fc5u && res[0] == res[3]));

    printf("==============================================================================\n");
    printf("DONE! Rebooting back to SPI flash (OpenBeken) in 1 second...\n");
    delay_ms(1000);
    wdt_reboot();
    return 0;
}
