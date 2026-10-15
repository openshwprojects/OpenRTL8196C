/*
 * board_config.h - the few things that are specific to your bench, in one place.
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* locally administered MAC for the bare-metal firmware */
#define BOARD_MAC       { 0x02, 0x19, 0x6c, 0x00, 0x00, 0x01 }
/* static IPv4 address until DHCP exists */
#define BOARD_IP        { 192, 168, 0, 196 }
/* a host to ARP for at start so that it learns our MAC right away (0.0.0.0 = none) */
#define BOARD_PEER_IP   { 192, 168, 0, 114 }

#define BOARD_UDP_CONSOLE_PORT  9696
#define BOARD_OTA_PORT          9698
#define BOARD_TCP_CONSOLE_PORT  23
#define BOARD_HTTP_PORT         80

/* switch port that is wired to the connector labelled LAN4 on the LT-6408n */
#define BOARD_PORT_LAN4         3

#endif
