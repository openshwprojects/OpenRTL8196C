/*
 * 17_usb_host - bring up the USB host, power the port and report every
 * device that is plugged in or unplugged (hotplug by polling).
 */
#include "rtl8196c.h"
#include "uart.h"
#include "timer.h"
#include "usb.h"

int main(void)
{
    uart_init();
    timer_delay_init();
    printf("\n\nusb host example\n");
    usb_debug = 1;

    if (usb_host_init() < 0) {
        printf("usb: controller does not respond\n");
        return 1;
    }
    const struct usb_host_info *hi = usb_info();
    printf("EHCI %x.%02x, %u port(s), %u companion(s); OHCI rev %02x, %u port(s); PHY patch %s\n",
           hi->ehci_version >> 8, hi->ehci_version & 0xFF, hi->ehci_hcsparams & 0xF,
           (hi->ehci_hcsparams >> 12) & 0xF, hi->ohci_revision & 0xFF, hi->ohci_rhdesca & 0xFF,
           hi->phy_ok ? "ok" : "MISMATCH");
    usb_print_ports();

    /* power-cycle VBUS so an already attached device starts from scratch */
    usb_vbus(0);
    delay_ms(500);
    usb_vbus(1);
    printf("VBUS on, waiting for devices (plug/unplug any time)\n");

    uint32_t last = micros();
    unsigned fails = 0;
    for (;;) {
        int ev = usb_poll();
        if (ev == USB_EV_CONNECTED) {
            fails = 0;
            usb_print_dev(usb_dev());
        } else if (ev == USB_EV_DISCONNECTED) {
            printf("usb device removed\n");
        } else if (ev == USB_EV_ENUM_FAILED) {
            printf("usb: device attached but enumeration failed (try %u), power-cycling port\n", ++fails);
            usb_print_ports();
            usb_vbus(0);
            delay_ms(1000);
            usb_vbus(1);
        }
        if (every_ms(&last, 5000))
            usb_print_ports();
        delay_ms(20);
    }
}
