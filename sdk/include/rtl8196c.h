/*
 * rtl8196c.h - register map of the Realtek RTL8196C SoC (Lexra RLX4181 core)
 *
 * All addresses are KSEG1 (uncached) virtual addresses: physical 0x18xxxxxx
 * is reached through 0xB8xxxxxx.  Register bits marked UNVERIFIED come from
 * the RTL8196E BSP header and still have to be confirmed on the 8196C.
 */
#ifndef RTL8196C_H
#define RTL8196C_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#define REG32(addr) (*(volatile uint32_t *)(addr))
#define REG8(addr)  (*(volatile uint8_t *)(addr))
#endif

#define KSEG0(phys) ((uint32_t)(phys) | 0x80000000u)   /* cached   */
#define KSEG1(phys) ((uint32_t)(phys) | 0xA0000000u)   /* uncached */

/* ---------------------------------------------------------------- system */
#define SYS_BASE        0xB8000000u
#define PIN_MUX_SEL     (SYS_BASE + 0x40)   /* bits 13:0 = LED->GPIO, bits 21:20 = PA2..6->GPIO, bit 22 = UART->GPIO */
#define PIN_MUX_LEDS_AS_GPIO  0x00003FFFu
#define PIN_MUX_PORTA_AS_GPIO 0x00300000u

/* ------------------------------------------------------------------ UART0 */
/* 16550 compatible, 4-byte register stride, register byte in the MSB of the word */
#define UART0_BASE      0xB8002000u
#define UART_RBR        (UART0_BASE + 0x00)  /* R  receive buffer           */
#define UART_THR        (UART0_BASE + 0x00)  /* W  transmit holding         */
#define UART_DLL        (UART0_BASE + 0x00)  /* RW divisor LSB (LCR.DLAB=1) */
#define UART_IER        (UART0_BASE + 0x04)  /* RW interrupt enable         */
#define UART_DLM        (UART0_BASE + 0x04)  /* RW divisor MSB (LCR.DLAB=1) */
#define UART_IIR        (UART0_BASE + 0x08)  /* R  interrupt identification */
#define UART_FCR        (UART0_BASE + 0x08)  /* W  FIFO control             */
#define UART_LCR        (UART0_BASE + 0x0C)  /* RW line control             */
#define UART_MCR        (UART0_BASE + 0x10)  /* RW modem control            */
#define UART_LSR        (UART0_BASE + 0x14)  /* R  line status              */
#define UART_MSR        (UART0_BASE + 0x18)  /* R  modem status             */
#define UART_SCR        (UART0_BASE + 0x1C)  /* RW scratch                  */

#define UART_LSR_DR     0x01  /* receive data ready            */
#define UART_LSR_OE     0x02  /* overrun error                 */
#define UART_LSR_THRE   0x20  /* transmit holding reg empty    */
#define UART_LSR_TEMT   0x40  /* transmitter empty             */

#define UART_IER_RDA    0x01  /* received data available       */
#define UART_IER_THRE   0x02  /* transmit holding empty        */
#define UART_IER_RLS    0x04  /* receiver line status          */

#define UART_IIR_NOINT  0x01  /* no interrupt pending          */
#define UART_IIR_ID     0x0E  /* interrupt id mask             */
#define UART_IIR_RDA    0x04
#define UART_IIR_THRE   0x02
#define UART_IIR_CTO    0x0C  /* character timeout (FIFO)      */

#define UART_FCR_EN     0x01
#define UART_FCR_RXCLR  0x02
#define UART_FCR_TXCLR  0x04

#define UART_LCR_8N1    0x03
#define UART_LCR_DLAB   0x80

/* ---------------------------------------------------- interrupt controller */
#define INTC_BASE       0xB8003000u
#define INTC_GIMR       (INTC_BASE + 0x00)  /* global interrupt mask, 1 = enabled */
#define INTC_GISR       (INTC_BASE + 0x04)  /* global interrupt status (pending)  */
#define INTC_IRR0       (INTC_BASE + 0x08)  /* routing, sources 0..7, 4 bits each */
#define INTC_IRR1       (INTC_BASE + 0x0C)  /* routing, sources 8..15             */
#define INTC_IRR2       (INTC_BASE + 0x10)  /* routing, sources 16..23            */
#define INTC_IRR3       (INTC_BASE + 0x14)  /* routing, sources 24..31            */

/* Interrupt source numbers = bit index in GIMR/GISR = nibble index in IRR0-3
 * (nibble n of IRRk is source 8k+n, bits 4n+3:4n).  MEASURED on the 8196C
 * with examples/93_route_probe; they differ from the RTL8196E header.
 * Sources 1, 12 and 13 report pending permanently: never unmask them. */
#define IRQ_UART0       7
#define IRQ_TC0         14
#define IRQ_TC1         15
/* still unverified guesses (the loader routes source 8 to IP3): */
#define IRQ_SWCORE      8
#define IRQ_USB_H       10
#define IRQ_GPIO_ABCD   16
#define IRQ_ALWAYS_PENDING_MASK ((1u << 1) | (1u << 12) | (1u << 13))

/* Routing value written to the IRRx nibble: N = 2..7 raises CPU line IPN
 * (Cause bit 8+N), 0/1 and 8..15 raise nothing (measured). */
#define IRQ_RS_NONE     0
#define IRQ_RS_IP2      2
#define IRQ_RS_IP3      3
#define IRQ_RS_IP4      4
#define IRQ_RS_IP5      5
#define IRQ_RS_IP6      6
#define IRQ_RS_IP7      7

/* ------------------------------------------------------------------ timers */
#define TC_BASE         0xB8003100u
/* Measured on the 8196C (rev 4): the counters count UP from 0, wrap back to
 * 0 when they reach TCxDATA and set TCxIP on the wrap.  Count and data sit in
 * bits 31:4 (28 bits).  Tick = bus clock / CDBR divisor, bus clock = 200 MHz
 * (divisor 200 gave 1.0014 MHz against the PC clock).  The loader leaves TC0
 * at divisor 14 / data 0x22E0A = 10 ms. */
#define TC0DATA         (TC_BASE + 0x00)    /* period << TC_DATA_SHIFT */
#define TC1DATA         (TC_BASE + 0x04)
#define TC0CNT          (TC_BASE + 0x08)    /* current count << TC_DATA_SHIFT */
#define TC1CNT          (TC_BASE + 0x0C)
#define TCCNR           (TC_BASE + 0x10)    /* control */
#define TCIR            (TC_BASE + 0x14)    /* interrupt enable / pending */
#define CDBR            (TC_BASE + 0x18)    /* clock divisor << 16 */
#define WDTCNR          (TC_BASE + 0x1C)    /* watchdog */

#define TC_DATA_SHIFT   4                   /* 28-bit counters in bits 31:4 */
#define TC_DATA_MAX     0x0FFFFFFFu
#define BUS_CLOCK_HZ    200000000u
#define TCCNR_TC0EN     (1u << 31)
#define TCCNR_TC0_TIMER (1u << 30)          /* 1 = timer (auto reload), 0 = counter */
#define TCCNR_TC1EN     (1u << 29)
#define TCCNR_TC1_TIMER (1u << 28)
#define TCIR_TC0IE      (1u << 31)
#define TCIR_TC1IE      (1u << 30)
#define TCIR_TC0IP      (1u << 29)          /* write 1 to clear */
#define TCIR_TC1IP      (1u << 28)
#define CDBR_DIV_SHIFT  16
#define WDT_STOP        0xA5000000u         /* magic: watchdog stopped */
#define WDT_BITE_NOW    0x00000000u         /* writing 0 reboots the SoC (3rd-party report) */

/* -------------------------------------------------------------------- GPIO */
#define GPIO_BASE       0xB8003500u
#define GPIO_PABCD_CNR  (GPIO_BASE + 0x00)  /* 0 = GPIO, 1 = peripheral function */
#define GPIO_PABCD_PTYPE (GPIO_BASE + 0x04) /* 0 = push-pull, 1 = open drain (UNVERIFIED) */
#define GPIO_PABCD_DIR  (GPIO_BASE + 0x08)  /* 1 = output */
#define GPIO_PABCD_DAT  (GPIO_BASE + 0x0C)
#define GPIO_PABCD_ISR  (GPIO_BASE + 0x10)  /* write 1 to clear */
#define GPIO_PAB_IMR    (GPIO_BASE + 0x14)  /* 2 bits per pin: 00 off 01 falling 10 rising 11 both */
#define GPIO_PCD_IMR    (GPIO_BASE + 0x18)

#define GPIO_PIN(port, pin) (((port) - 'A') * 8 + (pin))  /* GPIO_PIN('B',3) */
#define GPIO_A2_POWER_LED    GPIO_PIN('A', 2)
#define GPIO_A3_WPS_BUTTON   GPIO_PIN('A', 3)
#define GPIO_A4_USB_POWER    GPIO_PIN('A', 4)
#define GPIO_A5_RESET_BUTTON GPIO_PIN('A', 5)
#define GPIO_A6_WLAN_LED     GPIO_PIN('A', 6)
#define GPIO_C3_USB_LED      GPIO_PIN('C', 3)
#define GPIO_A4_RESET_BUTTON GPIO_A5_RESET_BUTTON /* legacy alias */

/* LAN LED pins once PIN_MUX_SEL[13:0] = 0x3FFF */
#define GPIO_LED_PORT0  GPIO_PIN('B', 2)
#define GPIO_LED_PORT1  GPIO_PIN('B', 3)
#define GPIO_LED_PORT2  GPIO_PIN('B', 4)
#define GPIO_LED_PORT3  GPIO_PIN('B', 5)
#define GPIO_LED_PORT4  GPIO_PIN('C', 6)
#define GPIO_LED_P1     GPIO_PIN('B', 7)
#define GPIO_LED_P2     GPIO_PIN('C', 0)

/* ---------------------------------------------------------------- CPU / CP0 */
#define CP0_STATUS      12
#define CP0_CAUSE       13
#define CP0_EPC         14
#define CP0_PRID        15
#define CP0_CCTL        20                  /* Lexra cache control */

/* R3000-style Status register bits */
#define ST_IEC          (1u << 0)           /* current interrupt enable */
#define ST_KUC          (1u << 1)
#define ST_IM_SHIFT     8
#define ST_IM_ALL_HW    0xFC00u             /* IM2..IM7 */
#define ST_BEV          (1u << 22)

#define CAUSE_EXCCODE(c) (((c) >> 2) & 0x1F)
#define CAUSE_IP_SHIFT  8
#define CAUSE_BD        (1u << 31)

/* CP0 $20 cache control.  The D-cache is WRITE-BACK (probes/87_cache_policy):
 * CCTL_DINVAL alone discards dirty lines (stack, globals), so running code
 * must use CCTL_DWBINVAL.  CPU reads through KSEG1 see dirty cached data,
 * DMA masters do not: write back before a device reads cached memory. */
#define CCTL_DINVAL     0x001               /* invalidate, DIRTY DATA IS LOST */
#define CCTL_IINVAL     0x002
#define CCTL_DWB        0x100               /* write dirty lines back, keep them */
#define CCTL_DWBINVAL   0x200               /* write back, then invalidate */

#define EXC_VECTOR_PHYS 0x00000080u         /* Lexra general exception vector (not 0x180) */

/* RAM layout used by the SDK */
#define LOAD_ADDR       0xA0500000u         /* uncached alias of 0x00500000 */
#define STACK_TOP       0xA0600000u

#endif /* RTL8196C_H */
