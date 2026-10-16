/*
 * pcie.h - RTL8196C PCIe root port (one port, one device: on the LT-6408n
 * the RTL8192CE Wi-Fi chip).  Bring-up follows the stock firmware's
 * PCIE_reset_procedure; see docs/01-hardware-notes.md section 6c.
 */
#ifndef PCIE_H
#define PCIE_H

#include <stdint.h>

#define PCIE_EP_CFG   0xB8B10000u      /* device configuration space (bus 1, device 0) */
#define PCIE_MEM      0xB9000000u      /* memory window (KSEG1); PCIe address 0x19000000 */

/* clock, PHY, device reset, link training.  Returns 0 with the link in L0,
 * -1 if nothing trains (port powered down again).  verbose prints the LTSSM states. */
int  pcie_init(int verbose);
void pcie_power_down(void);            /* device to D3hot, PHY parked, clock off */

static inline uint32_t pcie_cfg_read(uint32_t off) { return *(volatile uint32_t *)(PCIE_EP_CFG + off); }
static inline void pcie_cfg_write(uint32_t off, uint32_t v) { *(volatile uint32_t *)(PCIE_EP_CFG + off) = v; }

/* BAR2 -> PCIE_MEM, memory + I/O + bus master on.  Returns the window address. */
uint32_t pcie_map_bar2(void);

#endif
