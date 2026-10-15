/*
 * 16_stage2 - Stage-2 A/B slot chooser bootloader (docs/04-flash.md section 9.3)
 *
 * Reads the partition table (0x1F000) and boot state (0x1E000), validates
 * appA (0x020000) and appB (0x220000), picks the highest-seq valid slot
 * (with 3-strike fallback), records BTRY, and jumps to the app via
 * ota_ram_jump().  If no valid slot exists, enters an Ethernet OTA recovery
 * loop on TCP port 9698 and UDP port 9696.
 */
#include "rtl8196c.h"
#include "board_config.h"
#include "uart.h"
#include "timer.h"
#include "spiflash.h"
#include "flash_layout.h"
#include "net.h"
#include "dhcp.h"
#include "ota.h"
#include "console.h"
#include "cli.h"
#include "libc.h"

int main(void)
{
    uart_init();
    timer_delay_init();

    ota_boot_active_slot(1);

    printf("[stage2] no valid app slot found; starting Ethernet OTA recovery on port %u\n",
           BOARD_OTA_PORT);
    static const uint8_t def_ip[4] = BOARD_IP, peer[4] = BOARD_PEER_IP;
    uint8_t mac[6];
    spiflash_factory_mac(mac);
    net_init(mac, def_ip);
    ota_init(BOARD_OTA_PORT);
    console_udp_listen(BOARD_UDP_CONSOLE_PORT);
    dhcp_start("OpenRTL8196C-stage2");
    net_arp_request(peer);


    uint32_t last_arp = micros();
    for (;;) {
        net_poll();
        dhcp_poll();
        ota_poll();
        if (every_ms(&last_arp, 10000))
            net_arp_request(peer);
    }
}
