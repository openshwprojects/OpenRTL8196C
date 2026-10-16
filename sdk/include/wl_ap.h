/*
 * wl_ap.h - soft access point on the RTL8192CE: beacons (hardware TBTT),
 * probe / open-authentication / association responder, client table, and a
 * net_link that bridges 802.11 data frames to and from the Ethernet-shaped
 * frames net.c expects.  Open network only (no encryption).
 *
 *   wl_hw_init(&dev, &tables);
 *   wl_ap_start(&dev, "MySSID", 7);
 *   net_set_link(wl_ap_link());
 *   net_init(dev.mac, ip);          then net_poll() + wl_ap_poll() in the loop
 */
#ifndef WL_AP_H
#define WL_AP_H

#include <stdint.h>
#include "wl.h"
#include "net.h"

#define WL_AP_MAX_CLIENTS 8

struct wl_ap_client { uint8_t mac[6]; uint8_t state; uint8_t aid; uint32_t last_seen_ms; int rssi; };
                                             /* state: 1 authenticated, 2 associated */
struct wl_ap_stats {
    uint32_t beacons, sw_beacons, probe_req, probe_directed, probe_resp, auth, assoc, deauth;
    uint32_t data_rx, data_tx, tx_fail, rx_drop, rx_queue_full;
};
extern struct wl_ap_stats wl_ap_stats;
extern int wl_ap_sw_beacons;                 /* 1 = also send beacons from the normal TX queue (default 0) */
extern int wl_ap_trace;                      /* 1 = print the first frames stations send to us */

/* hw_beacons = 0: no-link mode, beacons from the normal TX queue every 100 ms
 * (the recipe a Windows PC joined); 1: hardware AP mode, TBTT beacon queue
 * (not yet seen by clients, under investigation). */
int  wl_ap_start(struct wl_dev *d, const char *ssid, uint8_t chan, int hw_beacons);
void wl_ap_stop(void);
const struct net_link *wl_ap_link(void);
void wl_ap_poll(void);                       /* keeps the next beacon armed */
int  wl_ap_clients(const struct wl_ap_client **out);   /* number of entries */

#endif
