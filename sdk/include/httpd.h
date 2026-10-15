/*
 * httpd.h - small HTTP/1.1 server on top of tcp.c (GET/HEAD, one response per connection)
 */
#ifndef HTTPD_H
#define HTTPD_H

#include <stdint.h>
#include "cli.h"
#include "tcp.h"

#define HTTPD_PATH_MAX   128
#define HTTPD_BODY_MAX   6000

struct http_request {
    const char *method;      /* "GET" or "HEAD" */
    const char *path;        /* "/regs" (query stripped) */
    const char *query;       /* "addr=b8003100&n=8" or "" */
    tcp_conn_t *conn;
};

struct http_response {
    int         status;      /* 200 */
    const char *status_text; /* "OK" */
    const char *content_type;/* "text/html; charset=utf-8" */
    const char *location;    /* for 3xx, else 0 */
    struct sb   body;        /* fill with sb_put/sb_putf */
};

/* A handler fills resp; defaults are 200 / text/html.  Routes match the
 * exact path, or a prefix when the pattern ends with '*'. */
typedef void (*http_handler_t)(const struct http_request *req, struct http_response *resp);

int  httpd_init(uint16_t port);
int  httpd_route(const char *pattern, http_handler_t handler);  /* up to 12 routes */
void httpd_redirect(struct http_response *resp, const char *location);
void httpd_not_found(struct http_response *resp, const char *path);

extern uint32_t httpd_requests;

#endif
