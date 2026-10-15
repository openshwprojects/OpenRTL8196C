/*
 * 95_eth_bisect - does the CPU port still receive after the loader's J?
 *
 * Read-only phase: watch the loader's RX ring for 20 s, touching nothing
 * but the UART.  Then one write at a time.  Ping the board while it runs.
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "swcore.h"
#include "gpio.h"

static volatile uint32_t *ring, *mb;
static int nring, nmb;

static int poll_phase(const char *name, int tenths, int give_back)
{
    int got = 0;
    uint32_t seen[16] = { 0 };
    for (int i = 0; i < nring; i++) seen[i] = ring[i];
    for (int t = 0; t < tenths; t++) {
        for (int i = 0; i < nring; i++) {
            uint32_t d = ring[i];
            if (d != seen[i]) {
                printf("  %s t=%d.%ds ring[%d] %08x -> %08x\n", name, t / 10, t % 10, i, seen[i], d);
                seen[i] = d;
            }
            if ((d & 1) == 0) {
                volatile struct pkthdr *ph = (volatile struct pkthdr *)((d & ~3u) | 0xA0000000u);
                got++;
                printf("  %s: frame idx %d len %u port %u\n", name, i, ph->ph_len, ph->ph_portlist);
                if (give_back) {
                    ring[i] |= 1;
                    for (int k = 0; k < nmb; k++) mb[k] |= 1;
                    seen[i] = ring[i];
                }
            }
        }
        delay_loop(40000);                      /* ~0.1 s */
    }
    printf("%s: %d frames, IISR=%08x ICR=%08x IIMR=%08x GISR=%08x\n", name, got, REG32(CPU_IISR),
           REG32(CPU_ICR), REG32(CPU_IIMR), REG32(INTC_GISR));
    return got;
}

int main(void)
{
    uart_init();
    printf("\n\neth bisect v2\n");
    ring = (volatile uint32_t *)(REG32(CPU_RPDCR(0)) | 0xA0000000u);
    mb   = (volatile uint32_t *)(REG32(CPU_RMDCR0) | 0xA0000000u);
    nring = nmb = 0;
    for (int i = 0; i < 16; i++) { nring++; if (ring[i] & 2) break; }
    for (int i = 0; i < 16; i++) { nmb++; if (mb[i] & 2) break; }
    printf("loader rings: %d pkthdr, %d mbuf, IISR=%08x GIMR=%08x GISR=%08x status=%08x\n",
           nring, nmb, REG32(CPU_IISR), REG32(INTC_GIMR), REG32(INTC_GISR), 0u);

    poll_phase("A untouched, give back", 200, 1);
    REG32(CPU_IISR) = REG32(CPU_IISR);
    poll_phase("B after IISR=IISR    ", 60, 1);
    REG32(CPU_ICR) = 0;
    delay_loop(100000);
    REG32(CPU_ICR) = ICR_TXCMD | ICR_RXCMD | ICR_MBUF_2048;
    poll_phase("C after ICR toggle   ", 60, 1);
    printf("done\n");
    return 0;
}
