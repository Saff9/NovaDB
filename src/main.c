/*
 * main.c — NovaDB server entry point
 *
 * Thin wrapper: opens the engine, starts the TCP server,
 * waits for Ctrl-C, then shuts down cleanly.
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include "novadb/nvdb.h"
#include "common/logging.h"
#include "network/server.h"

static void print_banner(void) {
    printf("\n"
           "  NovaDB — Open-Source SQL Database Engine\n"
           "  B+Tree · WAL · MVCC · Replication-Ready\n"
           "  Version %s\n\n", nvdb_version());
}

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

int main(int argc, char **argv) {
    const char *data_dir = "./novadb-data";
    int         port     = 9876;

    static struct option long_opts[] = {
        {"data-dir", required_argument, 0, 'd'},
        {"port",     required_argument, 0, 'p'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:p:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': data_dir = optarg; break;
        case 'p': port     = atoi(optarg); break;
        case 'h':
            printf("Usage: novadb-server [--data-dir DIR] [--port PORT]\n");
            return 0;
        default:
            return 1;
        }
    }

    print_banner();

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    nvdb_log_notice("opening database at %s", data_dir);
    NVDBEngine *engine = nvdb_open(data_dir);
    if (!engine) {
        fprintf(stderr, "FATAL: cannot open database at %s\n", data_dir);
        return 1;
    }

    nvdb_log_notice("starting server on port %d", port);
    NVDBServer *server = server_create(engine, port);
    if (!server) {
        fprintf(stderr, "FATAL: cannot create server on port %d\n", port);
        nvdb_close(engine);
        return 1;
    }

    if (server_start(server) != NVDB_OK) {
        fprintf(stderr, "FATAL: cannot start server\n");
        server_stop(server);
        nvdb_close(engine);
        return 1;
    }

    printf("NovaDB listening on port %d. Press Ctrl-C to stop.\n\n", port);

    while (g_running) sleep(1);

    printf("\nShutting down...\n");
    server_stop(server);
    nvdb_close(engine);
    printf("NovaDB stopped. Goodbye.\n");
    return 0;
}
