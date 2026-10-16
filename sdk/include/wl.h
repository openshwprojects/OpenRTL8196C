/*
 * wl.h - RTL8192CE Wi-Fi driver for the RTL8196C (PCIe), 802.11 2.4 GHz.
 *
 * Layered: wl_hw_* brings the chip up (MAC + baseband + RF, firmware, TX/RX
 * rings) and moves raw 802.11 frames; the scan / AP / STA logic lives in the
 * examples on top.  Firmware and register tables come from the user's own
 * flash backup via tools/extract_wifi_blobs.py (git-ignored wifi_blobs.inc),
 * so the caller passes them in.
 *
 * Receive only until wl_hw_tx is called; wl_hw_init leaves every TX queue
 * paused.  Runs uncached (load 0xA0500000): rings need no cache maintenance.
 */
#ifndef WL_H
#define WL_H

#include <stdint.h>

struct wl_tables {                          /* pointers into wifi_blobs.inc */
    const uint8_t *fw;      uint32_t fw_len;
    const uint32_t (*mac)[2];     uint32_t mac_n;
    const uint32_t (*phy)[2];     uint32_t phy_n;        /* PHY_REG 2T */
    const uint32_t (*phy_fix)[2]; uint32_t phy_fix_n;
    const uint32_t (*agc)[2];     uint32_t agc_n;
    const uint32_t (*radio_a)[2]; uint32_t radio_a_n;
    const uint32_t (*radio_b)[2]; uint32_t radio_b_n;
};

struct wl_dev {
    uint32_t mem;                           /* chip register window (0xB9000000) */
    uint8_t  mac[6];                        /* from the fuse, or a default */
    uint8_t  chan;
    int      cck_highpwr;
    uint32_t chnlval[2];                    /* RF reg 0x18 per path */
    uint8_t  pwr_cck[14], pwr_ht40_1s[14];  /* calibrated TX power per channel, path A */
    int      fuse_ok;
};

enum { WL_CH_MIN = 1, WL_CH_MAX = 13 };

/* bring the chip up; returns 0 or negative.  Receives after this, no TX. */
int  wl_hw_init(struct wl_dev *d, const struct wl_tables *t);
void wl_hw_set_channel(struct wl_dev *d, uint8_t ch);

/* received frame, handed to the callback: 802.11 frame (no FCS), length,
 * channel, signal in dBm.  Call wl_hw_rx_poll in a loop. */
typedef void (*wl_rx_fn)(void *ctx, const uint8_t *frame, uint32_t len, uint8_t ch, int rssi);
void wl_hw_rx_poll(struct wl_dev *d, wl_rx_fn cb, void *ctx);

/* queue one management/data frame for transmit at 1 Mbit CCK; blocks until the
 * chip releases the descriptor or the timeout.  Returns 0, -1 timeout.  A beacon
 * (set beacon=1) goes to the beacon queue. */
int  wl_hw_tx(struct wl_dev *d, const uint8_t *frame, uint32_t len, int beacon, uint32_t timeout_ms);

/* enable transmit (clear TXPAUSE for the normal queues).  Until called, the
 * chip is receive-only. */
void wl_hw_tx_enable(struct wl_dev *d);

/* raw register access, for the demos */
uint32_t wl_rd32(struct wl_dev *d, uint32_t r);
void     wl_wr32(struct wl_dev *d, uint32_t r, uint32_t v);
uint8_t  wl_rd8(struct wl_dev *d, uint32_t r);
void     wl_wr8(struct wl_dev *d, uint32_t r, uint8_t v);
void     wl_wr16(struct wl_dev *d, uint32_t r, uint16_t v);

/* set the BSSID filter and network type (0 none/scan, 1 ad-hoc, 2 STA, 3 AP) */
void wl_hw_set_bssid(struct wl_dev *d, const uint8_t *bssid);
void wl_hw_set_opmode(struct wl_dev *d, int type);

/* hardware AP mode: TSF/TBTT running, beacon interval 100 TU, BSSID = our MAC.
 * The hardware sends the armed beacon at every TBTT (and stamps the TSF into
 * it); call wl_hw_beacon_update often, it re-arms the next beacon as soon as
 * the previous one left.  Returns 1 when it armed a beacon, 0 if one is still
 * pending. */
int  wl_hw_ap_start(struct wl_dev *d, uint8_t chan);
int  wl_hw_beacon_update(struct wl_dev *d, const uint8_t *frame, uint32_t len);
uint32_t wl_hw_beacon_desc0(struct wl_dev *d);   /* word 0 of the beacon descriptor (bit 31 = still owned by HW) */

#endif
