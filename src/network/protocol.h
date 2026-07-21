/*
 * protocol.h — Wire protocol interface
 */
#ifndef NOVDB_PROTOCOL_H
#define NOVDB_PROTOCOL_H

typedef struct {
    char  type[16];
    char *payload;
    int   payload_len;
} ProtoMsg;

int  proto_encode(char *buf, size_t bufsz, const ProtoMsg *msg);
int  proto_decode(const char *raw, int rawlen, ProtoMsg *msg);
void proto_msg_free(ProtoMsg *msg);

#endif /* NOVDB_PROTOCOL_H */
