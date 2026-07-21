/*
 * server.h — TCP server interface
 */
#ifndef NOVDB_SERVER_H
#define NOVDB_SERVER_H

#include "novadb/types.h"

/* Forward */
typedef struct NVDBEngine NVDBEngine;

/* ── Lifecycle ────────────────────────────────────────────────── */
NVDBServer *server_create(NVDBEngine *engine, int port);
int         server_start(NVDBServer *srv);
void        server_stop(NVDBServer *srv);

#endif /* NOVDB_SERVER_H */
