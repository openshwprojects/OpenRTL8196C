/*
 * cli.h - small text helpers shared by the consoles and the web server
 */
#ifndef CLI_H
#define CLI_H

#include <stdint.h>
#include <stdarg.h>

/* string builder over a caller-supplied buffer, always NUL terminated */
struct sb {
    char *buf;
    uint32_t len, max;
    uint32_t overflow;        /* bytes that did not fit */
};
void sb_init(struct sb *b, char *buf, uint32_t max);
void sb_put(struct sb *b, const char *s);
void sb_putn(struct sb *b, const char *s, uint32_t n);
void sb_putf(struct sb *b, const char *fmt, ...);
void sb_vputf(struct sb *b, const char *fmt, va_list ap);

int  parse_hex(const char *s, uint32_t *v);      /* "b8003100" or "0xb8003100", 1 on success */
int  parse_u32(const char *s, uint32_t *v);      /* decimal */
int  split_args(char *s, char *argv[], int max); /* in place, returns argc */

/* "key=val&key2=val2": copy the value of key into out, returns out or 0 */
const char *query_get(const char *query, const char *key, char *out, uint32_t max);

void format_mac(char out[18], const uint8_t mac[6]);
void format_ip(char out[16], const uint8_t ip[4]);

#endif
