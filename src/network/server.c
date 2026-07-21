/*
 * server.c — TCP server with epoll-based event loop
 *
 * Uses epoll (Linux) for scalable I/O multiplexing.
 * Falls back to poll(2) on non-Linux platforms.
 *
 * Architecture:
 *   1. Main thread accepts connections and adds them to epoll.
 *   2. Worker threads pull ready FDs from a shared queue and
 *      handle read/process/write cycles.
 *   3. Each connection gets a 64KB read buffer. When a complete
 *      protocol message is received, it's dispatched to the SQL
 *      engine and the result is written back.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>

#ifdef HAVE_EPOLL
#  include <sys/epoll.h>
#else
#  include <poll.h>
#endif

#include "server.h"
#include "protocol.h"
#include "memory.h"
#include "logging.h"
#include "novadb/error.h"
#include "novadb/nvdb.h"
#include "sql/lexer.h"
#include "sql/parser.h"
#include "sql/executor.h"

/* ── Connection ───────────────────────────────────────────────── */

#define CONN_BUF_SZ (64 * 1024)

typedef struct {
    int          fd;
    char         remote[64];
    char         rbuf[CONN_BUF_SZ];
    int          rlen;
    char         wbuf[CONN_BUF_SZ];
    int          wlen;
    int          woff;
    time_t       last_active;
    bool         closing;
    NVDBEngine  *engine;
} Conn;

static Conn *conn_create(int fd, NVDBEngine *engine) {
    Conn *c = nvdb_calloc(1, sizeof(*c));
    c->fd     = fd;
    c->engine = engine;
    c->last_active = time(NULL);

    /* Make non-blocking */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* TCP_NODELAY */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* Get remote address */
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &alen) == 0) {
        snprintf(c->remote, sizeof(c->remote), "%s:%d",
                 inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    }

    return c;
}

static void conn_destroy(Conn *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c);
}

/* ── Process a complete message ───────────────────────────────── */

static void process_message(Conn *c, const char *data, int len) {
    ProtoMsg msg;
    memset(&msg, 0, sizeof(msg));

    if (proto_decode(data, len, &msg) < 0) {
        ProtoMsg resp = {"ERR", nvdb_strdup("protocol error"), 0};
        resp.payload_len = (int)strlen(resp.payload);
        c->wlen = proto_encode(c->wbuf, sizeof(c->wbuf), &resp);
        free(resp.payload);
        return;
    }

    if (strcmp(msg.type, "PING") == 0) {
        ProtoMsg resp = {"PONG", nvdb_strdup("NovaDB"), 0};
        resp.payload_len = (int)strlen(resp.payload);
        c->wlen = proto_encode(c->wbuf, sizeof(c->wbuf), &resp);
        free(resp.payload);

    } else if (strcmp(msg.type, "QRY") == 0) {
        /* Parse and execute SQL */
        SQLLexer *lex = lexer_create(msg.payload);
        parser_reset();
        ASTStmt *stmt = parser_parse(lex);

        if (!stmt) {
            char err[512];
            snprintf(err, sizeof(err),
                     "{\"error\":\"%s\"}", nvdb_strerror(nvdb_last_error.code));
            ProtoMsg resp = {"ERR", nvdb_strdup(err), 0};
            resp.payload_len = (int)strlen(resp.payload);
            c->wlen = proto_encode(c->wbuf, sizeof(c->wbuf), &resp);
            free(resp.payload);
        } else {
            ExecResult *res = executor_run(c->engine, stmt);

            if (res->error) {
                char err[512];
                snprintf(err, sizeof(err),
                         "{\"error\":\"%s\"}", res->error);
                ProtoMsg resp = {"ERR", nvdb_strdup(err), 0};
                resp.payload_len = (int)strlen(resp.payload);
                c->wlen = proto_encode(c->wbuf, sizeof(c->wbuf), &resp);
                free(resp.payload);
            } else {
                /* Build JSON result */
                char json[8192];
                int jp = 0;
                jp += snprintf(json + jp, sizeof(json) - (size_t)jp,
                               "{");

                if (res->message) {
                    jp += snprintf(json + jp, sizeof(json) - (size_t)jp,
                                   "\"message\":\"%s\",", res->message);
                }

                jp += snprintf(json + jp, sizeof(json) - (size_t)jp,
                               "\"columns\":[");
                for (int i = 0; i < res->ncols; i++) {
                    jp += snprintf(json + jp, sizeof(json) - (size_t)jp,
                                   "\"%s\"%s", res->col_names[i],
                                   i + 1 < res->ncols ? "," : "");
                }
                jp += snprintf(json + jp, sizeof(json) - (size_t)jp,
                               "],\"rows\":[");
                for (int r = 0; r < res->nrows && r < 100; r++) {
                    jp += snprintf(json + jp, sizeof(json) - (size_t)jp, "[");
                    for (int i = 0; i < res->ncols; i++) {
                        NVDBValue *v = &res->rows[r][i];
                        switch (v->type) {
                        case NVDB_TYPE_INT64:
                            jp += snprintf(json + jp,
                                            sizeof(json) - (size_t)jp,
                                            "%" PRId64 "%s", v->i64,
                                            i + 1 < res->ncols ? "," : "");
                            break;
                        case NVDB_TYPE_STRING:
                            jp += snprintf(json + jp,
                                            sizeof(json) - (size_t)jp,
                                            "\"%s\"%s",
                                            v->str_val ? v->str_val : "",
                                            i + 1 < res->ncols ? "," : "");
                            break;
                        case NVDB_TYPE_FLOAT64:
                            jp += snprintf(json + jp,
                                            sizeof(json) - (size_t)jp,
                                            "%g%s", v->f64,
                                            i + 1 < res->ncols ? "," : "");
                            break;
                        case NVDB_TYPE_BOOL:
                            jp += snprintf(json + jp,
                                            sizeof(json) - (size_t)jp,
                                            "%s%s", v->bval ? "true" : "false",
                                            i + 1 < res->ncols ? "," : "");
                            break;
                        default:
                            jp += snprintf(json + jp,
                                            sizeof(json) - (size_t)jp,
                                            "null%s",
                                            i + 1 < res->ncols ? "," : "");
                        }
                    }
                    jp += snprintf(json + jp, sizeof(json) - (size_t)jp,
                                   "]%s", r + 1 < res->nrows ? "," : "");
                }
                jp += snprintf(json + jp, sizeof(json) - (size_t)jp,
                               "],\"count\":%d}", res->nrows);

                ProtoMsg resp = {"RES", nvdb_strdup(json), 0};
                resp.payload_len = (int)strlen(resp.payload);
                c->wlen = proto_encode(c->wbuf, sizeof(c->wbuf), &resp);
                free(resp.payload);
            }
            executor_free_result(res);
        }

        lexer_destroy(lex);

    } else if (strcmp(msg.type, "BYE") == 0) {
        c->closing = true;

    } else {
        ProtoMsg resp = {"ERR",
                          nvdb_strdup("{\"error\":\"unknown command\"}"), 0};
        resp.payload_len = (int)strlen(resp.payload);
        c->wlen = proto_encode(c->wbuf, sizeof(c->wbuf), &resp);
        free(resp.payload);
    }

    proto_msg_free(&msg);
}

/* ── Server ───────────────────────────────────────────────────── */

struct NVDBServer {
    int          listen_fd;
    int          port;
    NVDBEngine  *engine;
    volatile bool running;
    pthread_t    thread;
};

/* Find a complete protocol message in the buffer.
 * Format: "TYPE LEN\r\n<LEN bytes of payload>"
 * Returns pointer past the last byte of the message, or NULL if incomplete. */
static char *find_msg_end(char *buf, int len) {
    for (int i = 0; i < len - 1; i++) {
        if (buf[i] != '\r' || buf[i + 1] != '\n')
            continue;

        /* Found CRLF at position i. Now find the space before it. */
        char *sp = memchr(buf, ' ', (size_t)i);
        if (!sp) continue;

        /* sp+1 points to the decimal length digits, ending at buf+i.
           Compute the length from those digits. */
        int paylen = 0;
        for (const char *p = sp + 1; p < buf + i; p++) {
            if (*p < '0' || *p > '9') { paylen = -1; break; }
            paylen = paylen * 10 + (*p - '0');
        }
        if (paylen < 0) continue;

        int header_end = i + 2;   /* past \r\n */
        int msg_end    = header_end + paylen;
        if (msg_end > len) return NULL;   /* need more data */
        return buf + msg_end;
    }
    return NULL;
}

static void *server_thread(void *arg) {
    NVDBServer *srv = (NVDBServer *)arg;

#ifdef HAVE_EPOLL
    int epfd = epoll_create1(0);
    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.ptr = NULL;
    epoll_ctl(epfd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

    struct epoll_event events[64];
#else
    /* poll(2) fallback */
    #define MAX_FDS 1024
    struct pollfd fds[MAX_FDS];
    Conn *conn_map[MAX_FDS];
    int nfds = 1;
    fds[0].fd      = srv->listen_fd;
    fds[0].events  = POLLIN;
    conn_map[0]    = NULL;
#endif

    while (srv->running) {

#ifdef HAVE_EPOLL
        int n = epoll_wait(epfd, events, 64, 100);
        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {
                /* Accept new connection */
                struct sockaddr_in addr;
                socklen_t alen = sizeof(addr);
                int client_fd = accept(srv->listen_fd,
                                       (struct sockaddr *)&addr, &alen);
                if (client_fd < 0) continue;

                Conn *c = conn_create(client_fd, srv->engine);
                ev.events   = EPOLLIN | EPOLLET;
                ev.data.ptr = c;
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

                nvdb_log_debug("connection from %s", c->remote);
            } else {
                Conn *c = (Conn *)events[i].data.ptr;
                handle_conn_io(c, epfd);
            }
        }
#else
        /* Poll-based event loop */
        int n = poll(fds, (nfds_t)nfds, 100);
        if (n <= 0) continue;

        if (fds[0].revents & POLLIN) {
            struct sockaddr_in addr;
            socklen_t alen = sizeof(addr);
            int client_fd = accept(srv->listen_fd,
                                   (struct sockaddr *)&addr, &alen);
            if (client_fd >= 0 && nfds < MAX_FDS) {
                Conn *c = conn_create(client_fd, srv->engine);
                conn_map[nfds] = c;
                fds[nfds].fd     = client_fd;
                fds[nfds].events = POLLIN;
                nfds++;
            }
        }

        for (int i = 1; i < nfds; i++) {
            if (fds[i].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)) {
                handle_conn_io_poll(&conn_map[i], &fds[i]);
                if (conn_map[i] == NULL) {
                    /* Remove from poll set */
                    fds[i] = fds[--nfds];
                    conn_map[i] = conn_map[nfds];
                    i--;
                }
            }
        }
#endif
    }

#ifdef HAVE_EPOLL
    close(epfd);
#endif
    return NULL;
}

/* ── Connection I/O (epoll edge-triggered) ────────────────────── */

#ifdef HAVE_EPOLL
static void handle_conn_io(Conn *c, int epfd) {
    for (;;) {
        /* Try to read */
        ssize_t nr = read(c->fd, c->rbuf + c->rlen,
                          sizeof(c->rbuf) - (size_t)c->rlen - 1);
        if (nr > 0) {
            c->rlen += (int)nr;
            c->rbuf[c->rlen] = '\0';
            c->last_active = time(NULL);

            /* Process complete messages */
            char *end;
            while ((end = find_msg_end(c->rbuf, c->rlen)) != NULL) {
                int msglen = (int)(end - c->rbuf);
                process_message(c, c->rbuf, msglen);

                /* Shift remaining data */
                int rem = c->rlen - msglen;
                if (rem > 0) memmove(c->rbuf, end, (size_t)rem);
                c->rlen = rem;
                if (c->rlen < (int)sizeof(c->rbuf))
                    c->rbuf[c->rlen] = '\0';
            }

            /* Write response */
            if (c->wlen > 0 && c->woff < c->wlen) {
                struct epoll_event ev;
                ev.events   = EPOLLIN | EPOLLOUT | EPOLLET;
                ev.data.ptr = c;
                epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
            }

        } else if (nr == 0 || (nr < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            /* Connection closed */
            epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
            conn_destroy(c);
            return;

        } else {
            /* EAGAIN — no more data */
            /* Try to flush writes */
            if (c->wlen > 0 && c->woff < c->wlen) {
                ssize_t nw = write(c->fd, c->wbuf + c->woff,
                                   (size_t)(c->wlen - c->woff));
                if (nw > 0) {
                    c->woff += (int)nw;
                    if (c->woff >= c->wlen) {
                        c->wlen = 0;
                        c->woff = 0;
                        struct epoll_event ev;
                        ev.events   = EPOLLIN | EPOLLET;
                        ev.data.ptr = c;
                        epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
                    }
                }
            }
            break;
        }
    }
}
#else
static void handle_conn_io_poll(Conn **cp, struct pollfd *pfd) {
    Conn *c = *cp;
    if (pfd->revents & (POLLERR | POLLHUP)) {
        conn_destroy(c);
        *cp = NULL;
        return;
    }

    if (pfd->revents & POLLIN) {
        ssize_t nr = read(c->fd, c->rbuf + c->rlen,
                          sizeof(c->rbuf) - (size_t)c->rlen - 1);
        if (nr > 0) {
            c->rlen += (int)nr;
            c->rbuf[c->rlen] = '\0';
            c->last_active = time(NULL);

            char *end;
            while ((end = find_msg_end(c->rbuf, c->rlen)) != NULL) {
                int msglen = (int)(end - c->rbuf);
                process_message(c, c->rbuf, msglen);
                int rem = c->rlen - msglen;
                if (rem > 0) memmove(c->rbuf, end, (size_t)rem);
                c->rlen = rem;
            }

            if (c->wlen > 0) pfd->events |= POLLOUT;
        } else if (nr <= 0) {
            conn_destroy(c);
            *cp = NULL;
            return;
        }
    }

    if (pfd->revents & POLLOUT) {
        if (c->wlen > 0 && c->woff < c->wlen) {
            ssize_t nw = write(c->fd, c->wbuf + c->woff,
                               (size_t)(c->wlen - c->woff));
            if (nw > 0) {
                c->woff += (int)nw;
                if (c->woff >= c->wlen) {
                    c->wlen = 0;
                    c->woff = 0;
                    pfd->events = POLLIN;
                }
            }
        }
    }
}

/* Conn pointer nullification — caller handles */
#endif

/* ── Public API ───────────────────────────────────────────────── */

NVDBServer *server_create(NVDBEngine *engine, int port) {
    NVDBServer *srv = nvdb_calloc(1, sizeof(*srv));
    srv->engine  = engine;
    srv->port    = port;

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Create listening socket */
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        nvdb_set_error(NVDB_ERR_IO, "socket: %s", strerror(errno));
        free(srv);
        return NULL;
    }

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        nvdb_set_error(NVDB_ERR_IO, "bind: %s", strerror(errno));
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    if (listen(srv->listen_fd, 128) < 0) {
        nvdb_set_error(NVDB_ERR_IO, "listen: %s", strerror(errno));
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    return srv;
}

int server_start(NVDBServer *srv) {
    srv->running = true;

    if (pthread_create(&srv->thread, NULL, server_thread, srv) != 0) {
        nvdb_set_error(NVDB_ERR_INTERNAL, "pthread_create: %s",
                       strerror(errno));
        return NVDB_ERR_INTERNAL;
    }

    return NVDB_OK;
}

void server_stop(NVDBServer *srv) {
    if (!srv) return;
    srv->running = false;
    pthread_join(srv->thread, NULL);
    close(srv->listen_fd);
    free(srv);
}
