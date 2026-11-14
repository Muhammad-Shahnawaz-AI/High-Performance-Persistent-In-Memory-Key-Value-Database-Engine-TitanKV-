#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTO_MAGIC 0x54 /* 'T' */
#define PROTO_REQ_HEADER_LEN 8
#define PROTO_RESP_HEADER_LEN 5

typedef enum {
    OP_GET    = 0x01,
    OP_SET    = 0x02,
    OP_DEL    = 0x03,
    OP_EXPIRE = 0x04,
    OP_ZADD   = 0x05,
    OP_ZRANGE = 0x06,
    OP_PING   = 0xFF
} proto_opcode_t;

typedef enum {
    STATUS_OK           = 0x00,
    STATUS_NOT_FOUND    = 0x01,
    STATUS_ERR_SYNTAX   = 0x02,
    STATUS_ERR_OOM      = 0x03,
    STATUS_ERR_INTERNAL = 0x04
} proto_status_t;

typedef struct {
    uint8_t opcode;
    uint16_t key_len;
    uint32_t val_len;
    const uint8_t *key;
    const uint8_t *val;
} proto_request_t;

/* Returns >0 (bytes consumed), 0 (incomplete frame), or -1 (protocol syntax error) */
int proto_parse_frame(const uint8_t *buf, size_t len, proto_request_t *out_req);
void proto_encode_response_header(uint8_t out_hdr[PROTO_RESP_HEADER_LEN], uint8_t status, uint32_t payload_len);

#endif /* PROTOCOL_H */