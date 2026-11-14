#include "protocol.h"
#include "titankv.h"
#include <arpa/inet.h>
#include <string.h>

int proto_parse_frame(const uint8_t *buf, size_t len, proto_request_t *out_req) {
    if (len < PROTO_REQ_HEADER_LEN) return 0;
    if (buf[0] != PROTO_MAGIC) return -1;

    uint16_t net_klen;
    uint32_t net_vlen;
    memcpy(&net_klen, buf + 2, sizeof(uint16_t));
    memcpy(&net_vlen, buf + 4, sizeof(uint32_t));

    uint16_t klen = ntohs(net_klen);
    uint32_t vlen = ntohl(net_vlen);

    if (buf[1] != OP_PING && klen == 0) return -1;
    if (vlen > TITANKV_MAX_VAL_LEN) return -1;

    size_t total_frame = PROTO_REQ_HEADER_LEN + (size_t)klen + (size_t)vlen;
    if (len < total_frame) return 0;

    out_req->opcode = buf[1];
    out_req->key_len = klen;
    out_req->val_len = vlen;
    out_req->key = buf + PROTO_REQ_HEADER_LEN;
    out_req->val = buf + PROTO_REQ_HEADER_LEN + klen;
    return (int)total_frame;
}

void proto_encode_response_header(uint8_t out_hdr[PROTO_RESP_HEADER_LEN], uint8_t status, uint32_t payload_len) {
    out_hdr[0] = status;
    uint32_t net_len = htonl(payload_len);
    memcpy(out_hdr + 1, &net_len, sizeof(uint32_t));
}