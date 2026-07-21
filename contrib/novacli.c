/*
 * novacli.c — Interactive CLI client for NovaDB
 *
 * Connects to a running NovaDB server via TCP and provides
 * an interactive SQL shell.
 *
 * Usage: novadb-cli [-h host] [-p port]
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <ctype.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 9876
#define BUF_SZ       (64 * 1024)

static int connect_to(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct hostent *he = gethostbyname(host);
    if (!he) { fprintf(stderr, "unknown host: %s\n", host); close(fd); return -1; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
    };
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect to %s:%d failed: %s\n", host, port, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int send_query(int fd, const char *sql) {
    char buf[BUF_SZ];
    int len = snprintf(buf, sizeof(buf), "QRY %zu\r\n%s",
                       strlen(sql), sql);
    return (int)write(fd, buf, (size_t)len);
}

static int read_response(int fd) {
    char buf[BUF_SZ];
    int total = 0;

    /* Read the header line: TYPE LEN\r\n */
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return -1;

    buf[n] = '\0';
    total = n;

    /* Find \r\n */
    char *rn = NULL;
    for (int i = 0; i < n - 1; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') { rn = buf + i; break; }
    }
    if (!rn) { printf("(malformed response)\n"); return -1; }

    /* Parse type and payload length */
    char type[8] = {0};
    int paylen = 0;
    char *sp = memchr(buf, ' ', (size_t)n);
    if (sp) {
        size_t tl = (size_t)(sp - buf);
        if (tl < sizeof(type)) memcpy(type, buf, tl);
        paylen = atoi(sp + 1);
    }

    /* Read remaining payload if needed */
    int have = total - (int)(rn + 2 - buf);
    while (have < paylen) {
        n = (int)read(fd, buf + total, sizeof(buf) - (size_t)total - 1);
        if (n <= 0) break;
        total += n;
        have  += n;
    }

    /* Null-terminate the payload */
    char *payload = rn + 2;
    if (have >= paylen) payload[paylen] = '\0';

    /* Display */
    printf("[%s] %s\n", type, payload);
    return 0;
}

static void repl(int fd) {
    char line[BUF_SZ];

    printf("Connected. Type SQL or \\q to quit, \\help for commands.\n\n");

    for (;;) {
        printf("novadb> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        /* Trim trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0) continue;

        /* Meta-commands */
        if (strcmp(line, "\\q") == 0 || strcmp(line, "\\quit") == 0
            || strcmp(line, "exit") == 0) {
            /* Send BYE */
            char bye[] = "BYE 3\r\nbye";
            write(fd, bye, sizeof(bye) - 1);
            printf("bye.\n");
            return;
        }

        if (strcmp(line, "\\help") == 0) {
            printf("Commands:\n");
            printf("  SQL           — any SQL statement\n");
            printf("  \\q, exit     — quit\n");
            printf("  \\help         — this help\n");
            continue;
        }

        if (strcmp(line, "\\ping") == 0) {
            char ping[] = "PING 0\r\n";
            write(fd, ping, sizeof(ping) - 1);
            read_response(fd);
            continue;
        }

        /* Send query */
        if (send_query(fd, line) <= 0) {
            printf("connection lost.\n");
            return;
        }

        /* Read result */
        if (read_response(fd) < 0) {
            printf("connection lost.\n");
            return;
        }
    }
}

int main(int argc, char **argv) {
    const char *host = DEFAULT_HOST;
    int         port = DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
    }

    printf("NovaDB CLI — connecting to %s:%d...\n", host, port);

    int fd = connect_to(host, port);
    if (fd < 0) return 1;

    repl(fd);
    close(fd);
    return 0;
}
