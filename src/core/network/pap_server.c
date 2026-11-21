// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pap_server.c
// The PAP server pieces both printers share -- see pap_server.h.

#include "pap_server.h"

void pap_status_prefix(uint8_t out[4], uint8_t socket, uint8_t quantum, uint16_t result) {
    out[0] = socket;
    out[1] = quantum;
    out[2] = (uint8_t)(result >> 8);
    out[3] = (uint8_t)result;
}

bool pap_open_answerable(bool advertised, const atp_packet_t *open) {
    return advertised && open && open->data && open->data_len >= 4;
}

uint8_t pap_open_quantum(const atp_packet_t *open) {
    uint8_t q = open->data[1];
    return (q == 0 || q > PAP_MAX_FLOW_QUANTUM) ? PAP_MAX_FLOW_QUANTUM : q;
}

uint16_t pap_take_seq(uint16_t *next) {
    uint16_t seq = (*next)++;
    if (*next == 0)
        *next = 1;
    return seq;
}

bool pap_request_is_session(bool active, uint8_t conn_id, const atalk_socket_addr_t *client, const ddp_header_t *ddp,
                            const atp_packet_t *atp) {
    return active && atp->user[0] == conn_id && ddp->llap.src == client->node && ddp->src_net == client->net;
}

atp_request_handle_t *pap_submit_senddata(const atalk_socket_addr_t *client, uint8_t src_socket, uint8_t conn_id,
                                          uint8_t quantum, uint16_t seq, int retry_limit,
                                          const atp_request_callbacks_t *cb, void *ctx) {
    // One response packet per unit of the flow quantum, at most eight
    uint8_t q = quantum ? quantum : PAP_MAX_FLOW_QUANTUM;
    if (q > PAP_MAX_FLOW_QUANTUM)
        q = PAP_MAX_FLOW_QUANTUM;
    uint8_t bitmap = q >= 8 ? 0xFF : (uint8_t)((1u << q) - 1u);
    atp_request_params_t params = {.dest = *client,
                                   .src_socket = src_socket,
                                   .bitmap = bitmap,
                                   .mode = ATP_TRANSACTION_XO,
                                   .trel_timer_hint = 2,
                                   .retry_timeout_ms = PAP_SENDDATA_RETRY_MS,
                                   .retry_limit = retry_limit};
    params.user[0] = conn_id;
    params.user[1] = PAP_FUNC_SENDDATA;
    params.user[2] = (uint8_t)(seq >> 8);
    params.user[3] = (uint8_t)seq;
    return atp_request_submit(&params, cb, ctx);
}

atp_request_handle_t *pap_submit_close_conn(const atalk_socket_addr_t *client, uint8_t src_socket, uint8_t conn_id,
                                            const atp_request_callbacks_t *cb, void *ctx) {
    atp_request_params_t params = {.dest = *client,
                                   .src_socket = src_socket,
                                   .bitmap = 0x01,
                                   .mode = ATP_TRANSACTION_XO,
                                   .trel_timer_hint = 2,
                                   .retry_timeout_ms = 2000,
                                   .retry_limit = 5};
    params.user[0] = conn_id;
    params.user[1] = PAP_FUNC_CLOSE;
    return atp_request_submit(&params, cb, ctx);
}
