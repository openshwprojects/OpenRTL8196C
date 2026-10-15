/*
 * 94_eth_probe - why does nothing arrive on the CPU port?
 *
 * 1. dump the CPU interface registers and the loader's own RX ring: pointer
 *    format, own bits, ph_len of each entry (tells whether the loader uses
 *    physical or KSEG1 pointers and whether its ring ran dry)
 * 2. keep the loader's rings but just poll them for a few seconds
 * 3. install our rings, then poll, printing CPUIISR and the ring words
 * 4. transmit a broadcast frame and watch the TX own bit come back
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "swcore.h"
#include "libc.h"

static const uint8_t mac[6] = { 0x02, 0x19, 0x6c, 0x00, 0x00, 0x01 };

static void dump_cpu_regs(const char *tag)
{
    printf("%s CPUIF:", tag);
    for (int i = 0; i < 16; i++)
        printf(" %08x", REG32(CPU_IFACE_BASE + 4 * i));
    printf("\n");
}

static void dump_ring(const char *tag, uint32_t ring_addr, int n)
{
    volatile uint32_t *ring = (volatile uint32_t *)(ring_addr | 0xA0000000u);
    printf("%s ring @%08x:\n", tag, ring_addr);
    for (int i = 0; i < n; i++) {
        uint32_t d = ring[i];
        volatile struct pkthdr *ph = (volatile struct pkthdr *)((d & ~3u) | 0xA0000000u);
        printf("  [%d] %08x own=%s wrap=%u  ph_len=%u flags=%04x portlist=%02x mbuf=%08x",
               i, d, (d & 1) ? "SW" : "CPU", (d >> 1) & 1, ph->ph_len, ph->ph_flags,
               ph->ph_portlist, (uint32_t)ph->ph_mbuf);
        if (ph->ph_mbuf) {
            volatile struct mbuf *m = (volatile struct mbuf *)((uint32_t)ph->ph_mbuf | 0xA0000000u);
            printf(" m_data=%08x m_len=%u m_ext=%u", (uint32_t)m->m_data, m->m_len, m->m_extsize);
        }
        printf("\n");
        if (d & 2) break;
    }
}

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\neth probe\n");
    dump_cpu_regs("loader");
    uint32_t loader_rx = REG32(CPU_RPDCR(0));
    uint32_t loader_mb = REG32(CPU_RMDCR0);
    uint32_t loader_tx = REG32(CPU_TPDCR0);
    dump_ring("loader RX", loader_rx, 8);
    dump_ring("loader TX", loader_tx, 8);
    printf("loader mbuf ring @%08x:", loader_mb);
    for (int i = 0; i < 6; i++) {
        uint32_t d = ((volatile uint32_t *)(loader_mb | 0xA0000000u))[i];
        printf(" %08x", d);
        if (d & 2) break;
    }
    printf("\n");

    /* phase 2: leave everything as the loader had it, give its ring back to HW and poll it */
    printf("phase2: polling the loader's ring for 6 s (ping me now)\n");
    volatile uint32_t *lring = (volatile uint32_t *)(loader_rx | 0xA0000000u);
    volatile uint32_t *lmb = (volatile uint32_t *)(loader_mb | 0xA0000000u);
    int n = 0;
    for (int i = 0; i < 8; i++) {              /* return all entries to the switch */
        lring[i] |= 1;
        lmb[i] |= 1;
        n++;
        if (lring[i] & 2) break;
    }
    REG32(CPU_IISR) = 0xFFFFFFFFu;
    uint32_t t0 = micros();
    int idx = 0, got = 0;
    while (((micros() - t0) & TC_DATA_MAX) < 6000000u) {
        if ((lring[idx] & 1) == 0) {
            volatile struct pkthdr *ph = (volatile struct pkthdr *)((lring[idx] & ~3u) | 0xA0000000u);
            printf("  loader ring got frame: idx %d len %u port %u\n", idx, ph->ph_len, ph->ph_portlist);
            got++;
            lring[idx] |= 1;
            lmb[idx] |= 1;
            idx = (idx + 1) % n;
        }
    }
    printf("phase2: %d frames, IISR=%08x ICR=%08x\n", got, REG32(CPU_IISR), REG32(CPU_ICR));

    /* phase 3: our rings */
    eth_init(mac);
    REG32(CPU_IISR) = 0xFFFFFFFFu;
    dump_cpu_regs("ours  ");
    dump_ring("our RX", REG32(CPU_RPDCR(0)), ETH_RX_RING);
    printf("phase3: polling our ring for 8 s (ping me now)\n");
    t0 = micros();
    uint32_t last = t0;
    got = 0;
    while (((micros() - t0) & TC_DATA_MAX) < 8000000u) {
        uint8_t *d; uint32_t len; int port;
        while (eth_recv(&d, &len, &port)) {
            printf("  our ring got frame: len %u port %d type %02x%02x\n", len, port, d[12], d[13]);
            got++;
            eth_recv_done();
        }
        if (((micros() - last) & TC_DATA_MAX) >= 1000000u) {
            last = micros();
            printf("  IISR=%08x ICR=%08x rx0=%08x rx1=%08x mb0=%08x\n", REG32(CPU_IISR), REG32(CPU_ICR),
                   ((volatile uint32_t *)REG32(CPU_RPDCR(0)))[0], ((volatile uint32_t *)REG32(CPU_RPDCR(0)))[1],
                   ((volatile uint32_t *)REG32(CPU_RMDCR0))[0]);
        }
    }
    printf("phase3: %d frames\n", got);

    /* phase 4: transmit a broadcast ARP request for 192.168.0.1 to all ports */
    uint8_t f[42];
    memset(f, 0xFF, 6);
    memcpy(f + 6, mac, 6);
    f[12] = 0x08; f[13] = 0x06;
    uint8_t *a = f + 14;
    a[0] = 0; a[1] = 1; a[2] = 8; a[3] = 0; a[4] = 6; a[5] = 4; a[6] = 0; a[7] = 1;
    memcpy(a + 8, mac, 6);
    a[14] = 192; a[15] = 168; a[16] = 0; a[17] = 196;
    memset(a + 18, 0, 6);
    a[24] = 192; a[25] = 168; a[26] = 0; a[27] = 1;
    for (int i = 0; i < 3; i++) {
        int r = eth_send(f, 42, ALL_PORT_MASK);
        delay_ms(20);
        eth_tx_reclaim();
        printf("tx %d: send=%d tx0=%08x IISR=%08x\n", i, r,
               ((volatile uint32_t *)REG32(CPU_TPDCR0))[i % ETH_TX_RING], REG32(CPU_IISR));
    }
    t0 = micros();
    got = 0;
    while (((micros() - t0) & TC_DATA_MAX) < 3000000u) {
        uint8_t *d; uint32_t len; int port;
        while (eth_recv(&d, &len, &port)) {
            printf("  reply? len %u port %d type %02x%02x\n", len, port, d[12], d[13]);
            got++;
            eth_recv_done();
        }
    }
    printf("phase4: %d frames after our ARP request\n", got);
    printf("done\n");
    return 0;
}
