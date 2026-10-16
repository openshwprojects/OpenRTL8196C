/*
 * wl_sta.h - Wi-Fi station on the RTL8192CE: finds the access point, joins
 * it (open authentication + association), runs the WPA2-PSK 4-way handshake,
 * installs the AES-CCMP keys in the chip's key table and then bridges data
 * frames to net.c as a net_link.  Open (unencrypted) networks work too.
 *
 *   wl_hw_init(&dev, &tables);
 *   wl_sta_start(&dev, "ssid", "passphrase" or "");
 *   net_set_link(wl_sta_link()); net_init(dev.mac, zero_ip); dhcp_start(...);
 *   loop: net_poll(); wl_sta_poll(); dhcp_poll();
 */
#ifndef WL_STA_H
#define WL_STA_H

#include <stdint.h>
#include "wl.h"
#include "net.h"

enum wl_sta_state {
    WL_STA_IDLE = 0, WL_STA_SCANNING, WL_STA_AUTHENTICATING, WL_STA_ASSOCIATING,
    WL_STA_HANDSHAKE, WL_STA_CONNECTED, WL_STA_FAILED,
};

struct wl_sta_info {
    enum wl_sta_state state;
    uint8_t  bssid[6];
    uint8_t  chan;
    int      rssi;
    uint8_t  secured;               /* 1 = WPA2-PSK CCMP keys installed */
    uint16_t aid;
    const char *fail_reason;
    uint32_t data_rx, data_tx, tx_fail, rx_drop, eapol_rx, eapol_tx, retries;
    uint32_t rx_unicast, rx_bcast;  /* decrypted data frames addressed to us / to broadcast-multicast */
    uint32_t rx_undecrypted;        /* protected frames whose payload did not decrypt (RCR accepts ICV errors) */
    uint8_t  ap_group_cipher, ap_pair_ccmp, ap_pair_tkip;   /* from the AP's RSN element (4 = CCMP, 2 = TKIP) */
    uint8_t  gtk_len, gtk_id;       /* from handshake message 3 */
};
extern int wl_sta_trace;            /* 1 = print the first received data frames */

int  wl_sta_start(struct wl_dev *d, const char *ssid, const char *passphrase);
void wl_sta_poll(void);
const struct wl_sta_info *wl_sta_info(void);
const struct net_link *wl_sta_link(void);
const char *wl_sta_state_name(enum wl_sta_state s);

#endif
