/*
 * console.h - the board's command set, transport independent.
 * Used by the UDP console, the TCP console and (if you like) the UART.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include "cli.h"

#define CONSOLE_OK      0
#define CONSOLE_REBOOT  1      /* caller should flush its reply, then wdt_reboot() */

/* Executes one command line (modified in place) and appends the reply to out. */
int  console_exec(char *line, struct sb *out);
void console_set_tag(const char *tag);

/* Shared TCP/UDP console & echo listeners used across examples */
int  console_tcp_listen(uint16_t port);
int  console_udp_listen(uint16_t port);
int  echo_tcp_listen(uint16_t port);
int  echo_udp_listen(uint16_t port);

/* current LED mask as last set through the console / web page */
extern uint32_t console_led_mask;
extern uint32_t console_commands;

#endif

