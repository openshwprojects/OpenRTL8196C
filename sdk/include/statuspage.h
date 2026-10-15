/*
 * statuspage.h - the board's web pages, registered on the httpd:
 *   /            status page with LED buttons
 *   /led/<hex>   set LEDs, redirect to /
 *   /regs?addr=<hex>&n=<dec>   register dump (text)
 *   /api/status  JSON
 */
#ifndef STATUSPAGE_H
#define STATUSPAGE_H

void statuspage_register(void);

#endif
