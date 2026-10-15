#include "httpd.h"
#include "uart.h"
#include "libc.h"

#define HTTPD_MAX_ROUTES 12
#define HTTPD_REQ_MAX    1024

struct route { const char *pattern; http_handler_t handler; };
static struct route routes[HTTPD_MAX_ROUTES];
static int nroutes;

/* per-connection request accumulation, indexed by tcp_index() */
static struct { uint16_t len; char buf[HTTPD_REQ_MAX]; } reqs[TCP_MAX_CONN];

static char body_buf[HTTPD_BODY_MAX];
static char head_buf[256];
uint32_t httpd_requests;

static const struct route *match(const char *path)
{
    for (int i = 0; i < nroutes; i++) {
        const char *p = routes[i].pattern;
        uint32_t n = strlen(p);
        if (n && p[n - 1] == '*') { if (strncmp(path, p, n - 1) == 0) return &routes[i]; }
        else if (strcmp(path, p) == 0) return &routes[i];
    }
    return 0;
}

void httpd_redirect(struct http_response *resp, const char *location)
{
    resp->status = 303; resp->status_text = "See Other"; resp->location = location;
    resp->content_type = "text/plain";
}

void httpd_not_found(struct http_response *resp, const char *path)
{
    resp->status = 404; resp->status_text = "Not Found"; resp->content_type = "text/plain";
    sb_putf(&resp->body, "no such page: %s\n", path);
}

static void serve(tcp_conn_t *c, char *req, uint32_t len)
{
    (void)len;
    struct http_request r;
    struct http_response resp;
    char path[HTTPD_PATH_MAX];
    char *query = "";
    const char *method = "GET";
    const char *p = req;

    httpd_requests++;
    if (strncmp(p, "GET ", 4) == 0) p += 4;
    else if (strncmp(p, "HEAD ", 5) == 0) { p += 5; method = "HEAD"; }
    else method = 0;

    uint32_t i = 0;
    if (method) {
        while (*p && *p != ' ' && *p != '\r' && i < sizeof path - 1) path[i++] = *p++;
    }
    path[i] = 0;
    for (char *q = path; *q; q++)
        if (*q == '?') { *q = 0; query = q + 1; break; }
    if (!path[0]) { path[0] = '/'; path[1] = 0; }

    r.method = method ? method : "?"; r.path = path; r.query = query; r.conn = c;
    resp.status = 200; resp.status_text = "OK"; resp.content_type = "text/html; charset=utf-8"; resp.location = 0;
    sb_init(&resp.body, body_buf, sizeof body_buf);

    if (!method) {
        resp.status = 405; resp.status_text = "Method Not Allowed"; resp.content_type = "text/plain";
        sb_put(&resp.body, "only GET and HEAD\n");
    } else {
        const struct route *rt = match(path);
        if (rt) rt->handler(&r, &resp);
        else httpd_not_found(&resp, path);
    }

    struct sb h;
    sb_init(&h, head_buf, sizeof head_buf);
    sb_putf(&h, "HTTP/1.1 %d %s\r\nServer: OpenRTL8196C\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n",
            resp.status, resp.status_text, resp.content_type, resp.body.len);
    if (resp.location) sb_putf(&h, "Location: %s\r\n", resp.location);
    sb_put(&h, "\r\n");
    tcp_send(c, h.buf, h.len);
    if (strcmp(r.method, "HEAD") != 0 && resp.body.len)
        tcp_send(c, resp.body.buf, resp.body.len);
    tcp_close(c);
}

static void on_data(tcp_conn_t *c, const uint8_t *data, uint32_t len)
{
    int i = tcp_index(c);
    uint32_t room = HTTPD_REQ_MAX - 1 - reqs[i].len;
    uint32_t n = len < room ? len : room;
    memcpy(reqs[i].buf + reqs[i].len, data, n);
    reqs[i].len += n;
    reqs[i].buf[reqs[i].len] = 0;
    char *end = 0;
    for (uint32_t k = 3; k < reqs[i].len; k++)
        if (reqs[i].buf[k - 3] == '\r' && reqs[i].buf[k - 2] == '\n' && reqs[i].buf[k - 1] == '\r' && reqs[i].buf[k] == '\n') {
            end = reqs[i].buf + k + 1; break;
        }
    if (!end && reqs[i].len < HTTPD_REQ_MAX - 1)
        return;                                 /* wait for the rest of the header */
    serve(c, reqs[i].buf, reqs[i].len);
    reqs[i].len = 0;
}

static void on_event(tcp_conn_t *c, int ev)
{
    int i = tcp_index(c);
    if (ev == TCP_EV_ACCEPT) reqs[i].len = 0;
    else if (ev == TCP_EV_PEER_FIN) tcp_close(c);
    else if (ev == TCP_EV_CLOSED) reqs[i].len = 0;
}

int httpd_init(uint16_t port)
{
    nroutes = 0;
    return tcp_listen(port, on_data, on_event);
}

int httpd_route(const char *pattern, http_handler_t handler)
{
    if (nroutes >= HTTPD_MAX_ROUTES) return -1;
    routes[nroutes].pattern = pattern;
    routes[nroutes].handler = handler;
    nroutes++;
    return 0;
}
