#include "cli.h"
#include "uart.h"
#include "libc.h"

void sb_init(struct sb *b, char *buf, uint32_t max)
{
    b->buf = buf; b->len = 0; b->max = max; b->overflow = 0;
    if (max) buf[0] = 0;
}

void sb_putn(struct sb *b, const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (b->len + 1 < b->max) b->buf[b->len++] = s[i];
        else b->overflow++;
    }
    if (b->max) b->buf[b->len] = 0;
}

void sb_put(struct sb *b, const char *s)
{
    sb_putn(b, s, strlen(s));
}

void sb_vputf(struct sb *b, const char *fmt, va_list ap)
{
    char tmp[256];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    if (n < 0) return;
    if ((uint32_t)n > sizeof tmp - 1) n = sizeof tmp - 1;
    sb_putn(b, tmp, (uint32_t)n);
}

void sb_putf(struct sb *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vputf(b, fmt, ap);
    va_end(ap);
}

int parse_hex(const char *s, uint32_t *v)
{
    uint32_t r = 0; int n = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++, n++) {
        char c = *s;
        if (c >= '0' && c <= '9') r = (r << 4) | (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') r = (r << 4) | (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') r = (r << 4) | (uint32_t)(c - 'A' + 10);
        else break;
    }
    *v = r;
    return n > 0;
}

int parse_u32(const char *s, uint32_t *v)
{
    uint32_t r = 0; int n = 0;
    for (; *s >= '0' && *s <= '9'; s++, n++) r = r * 10 + (uint32_t)(*s - '0');
    *v = r;
    return n > 0;
}

int split_args(char *s, char *argv[], int max)
{
    int argc = 0;
    while (*s && argc < max) {
        while (*s == ' ' || *s == '\r' || *s == '\n' || *s == '\t') s++;
        if (!*s) break;
        argv[argc++] = s;
        while (*s && *s != ' ' && *s != '\r' && *s != '\n' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
    return argc;
}

const char *query_get(const char *q, const char *key, char *out, uint32_t max)
{
    uint32_t kl = strlen(key);
    while (q && *q) {
        if (strncmp(q, key, kl) == 0 && q[kl] == '=') {
            q += kl + 1;
            uint32_t i = 0;
            while (*q && *q != '&' && *q != ' ' && i < max - 1) out[i++] = *q++;
            out[i] = 0;
            return out;
        }
        while (*q && *q != '&' && *q != ' ') q++;
        if (*q == '&') q++;
        else break;
    }
    return 0;
}

void format_mac(char out[18], const uint8_t m[6])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

void format_ip(char out[16], const uint8_t ip[4])
{
    snprintf(out, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}
