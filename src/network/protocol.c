/*
 * protocol.c — NovaDB wire protocol
 *
 * Simple text-framed protocol:
 *   <TYPE> <PAYLOAD_LEN>\r\n
 *   <PAYLOAD>
 *
 * Types: PING, PONG, QRY, RES, ERR, BYE
 * Query results are JSON-encoded for easy parsing by clients.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "protocol.h"
#include "memory.h"
#include "novadb/error.h"

int proto_encode(char *buf, size_t bufsz, const ProtoMsg *msg) {
    return snprintf(buf, bufsz, "%s %d\r\n%s",
                    msg->type, (int)strlen(msg->payload), msg->payload);
}

static const char *find_crlf(const char *haystack, int haylen) {
    for (int i = 0; i < haylen - 1; i++) {
        if (haystack[i] == '\r' && haystack[i + 1] == '\n')
            return haystack + i;
    }
    return NULL;
}

int proto_decode(const char *raw, int rawlen, ProtoMsg *msg) {
    /* Find the first space and \r\n */
    const char *sp = memchr(raw, ' ', (size_t)rawlen);
    const char *rn = find_crlf(raw, rawlen);

    if (!sp || !rn || sp >= rn) return -1;

    size_t typelen = (size_t)(sp - raw);
    if (typelen >= sizeof(msg->type)) return -1;

    memcpy(msg->type, raw, typelen);
    msg->type[typelen] = '\0';

    /* Parse payload length */
    int paylen = 0;
    for (const char *p = sp + 1; p < rn; p++) {
        if (*p < '0' || *p > '9') return -1;
        paylen = paylen * 10 + (*p - '0');
    }

    const char *payload_start = rn + 2;
    int remaining = rawlen - (int)(payload_start - raw);

    if (remaining < paylen) return -1;

    msg->payload = nvdb_malloc((size_t)(paylen + 1));
    memcpy(msg->payload, payload_start, (size_t)paylen);
    msg->payload[paylen] = '\0';
    msg->payload_len = paylen;

    return (int)(payload_start - raw) + paylen;
}

void proto_msg_free(ProtoMsg *msg) {
    if (!msg) return;
    free(msg->payload);
    msg->payload = NULL;
}
