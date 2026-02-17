// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk.c
// AppleTalk networking protocol stack implementation.

// ============================================================================
// Includes
// ============================================================================

#include "appletalk.h"

#include "appletalk_internal.h"
#include "common.h"
#include "log.h"
#include "scc.h"
#include "scheduler.h"
#include "shell.h"
#include "system.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// ============================================================================
// Constants and Macros
// ============================================================================

// Module-level state: dependencies passed at init time (Pattern B)
static scc_t *g_scc = NULL;
static scheduler_t *g_scheduler = NULL;

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

#define LLAP_DATA_MAX_SIZE 600

#define LLAP_DDP_SHORT    0x01
#define LLAP_DDP_EXTENDED 0x02

#define LLAP_ENQ 0x81
#define LLAP_ACK 0x82
#define LLAP_RTS 0x84
#define LLAP_CTS 0x85

// ============================================================================
// Forward Declarations
// ============================================================================

// Lower layers at top of file, higher at bottom. These prototypes resolve circular references.
static void ddp_short_in(llap_header_t *llap, uint8_t *buf, size_t len);
static void ddp_in(ddp_header_t *ddp, uint8_t *buf, size_t len);
static void nbp_in(ddp_header_t *ddp, uint8_t *buf, size_t len);
static void atp_in(const ddp_header_t *ddp, const uint8_t *buf, int len);
static void asp_in(const ddp_header_t *ddp, atp_packet_t *atp, void *ctx);
// AFP handler implemented in server.c
extern uint32_t afp_handle_command(uint8_t opcode, const uint8_t *in, int in_len, uint8_t *out, int out_max,
                                   int *out_len);
// Logging category function used by LOG() macro; provided by LOG_USE_CATEGORY_NAME later
static log_category_t *_log_get_local_category(void);
// Hex dump helper forward declaration
static void log_hex(int level, const char *tag, const uint8_t *data, size_t len);

// ============================================================================
// Operations
// ============================================================================

// =============================== LLAP (LocalTalk) - lowest layer ===============================
static void llap_send(const llap_header_t *llap, const uint8_t *data, size_t len) {
    uint8_t buf[LLAP_HEADER_SIZE + LLAP_DATA_MAX_SIZE];

    buf[0] = llap->dst;
    buf[1] = llap->src;
    buf[2] = llap->type;

    if (len > LLAP_DATA_MAX_SIZE) {
        // Defensive cap to prevent overflow in case callers miscompute
        len = LLAP_DATA_MAX_SIZE;
    }
    if (len > 0 && data) {
        memcpy((char *)(buf + 3), data, len);
    }

    size_t total = len + LLAP_HEADER_SIZE;
    // Full LLAP tx hexdump at high verbosity
    log_hex(11, "LLAP tx dump", buf, total);
    if (g_scc)
        scc_sdlc_send(g_scc, buf, total);
}

void llap_in(uint8_t *buf, size_t len) {
    llap_header_t header;

    assert(len >= LLAP_HEADER_SIZE);

    header.dst = buf[0];
    header.src = buf[1];
    header.type = buf[2];

    // LLAP rx hexdump at high verbosity
    log_hex(11, "LLAP rx dump", buf, len);

    switch (header.type) {

    case LLAP_ENQ:
        assert(len == LLAP_HEADER_SIZE);
        LOG(11, "LLAP ENQ src=%02X dst=%02X", (unsigned)header.src, (unsigned)header.dst);
        // Reply with ACK if this ENQ targets our node (dynamic node ID probe/ack).
        if (header.dst == LLAP_HOST_NODE) {
            llap_header_t ack;
            ack.dst = header.src;
            ack.src = LLAP_HOST_NODE;
            ack.type = LLAP_ACK;
            LOG(11, "LLAP send ACK to=%02X", (unsigned)ack.dst);
            llap_send(&ack, NULL, 0);
        }
        break;

    case LLAP_RTS:
        assert(len == LLAP_HEADER_SIZE);
        LOG(11, "LLAP RTS src=%02X dst=%02X", (unsigned)header.src, (unsigned)header.dst);
        // Respond with CTS for directed traffic addressed to us so the sender may transmit.
        if (header.dst == LLAP_HOST_NODE) {
            llap_header_t cts;
            cts.dst = header.src;
            cts.src = LLAP_HOST_NODE;
            cts.type = LLAP_CTS;
            LOG(11, "LLAP send CTS to=%02X", (unsigned)cts.dst);
            llap_send(&cts, NULL, 0);
        }
        break;

    case LLAP_CTS:
        // We may see CTS in response to our own RTS when we originate frames; ignore (optional trace).
        LOG(8, "LLAP CTS src=%02X dst=%02X", (unsigned)header.src, (unsigned)header.dst);
        break;

    case LLAP_DDP_SHORT:
        LOG(11, "LLAP DDP_SHORT rx len=%zu", len - 3);
        ddp_short_in(&header, buf + 3, len - 3);
        break;

    case LLAP_DDP_EXTENDED:
        assert(0);
        break;

    default:
        assert(0);
        break;
    }
}

void process_packet(uint8_t *buf, size_t size) {
    llap_in(buf, size);
}

// =============================== DDP (Datagram Delivery Protocol) ===============================

// [1]: ddp type field values
#define DDP_RTMP_RESPONSE 0x01
#define DDP_NBP           0x02
#define DDP_ATP           0x03
#define DDP_AEP           0x04
#define DDP_RTMP_REQUEST  0x05
#define DDP_ZIP           0x06
#define DDP_ADSP          0x07

// Returns a short mnemonic name for a DDP protocol type or NULL if unknown.
static const char *ddp_type_name(uint8_t type) {
    switch (type) {
    case DDP_RTMP_RESPONSE:
        return "RTMP-Resp";
    case DDP_NBP:
        return "NBP";
    case DDP_ATP:
        return "ATP";
    case DDP_AEP:
        return "AEP";
    case DDP_RTMP_REQUEST:
        return "RTMP-Req";
    case DDP_ZIP:
        return "ZIP";
    case DDP_ADSP:
        return "ADSP";
    default:
        return NULL;
    }
}

// Emits a consistent one-line DDP summary including direction and payload size.
static void ddp_log_summary(int level, const char *direction, const ddp_header_t *ddp, uint16_t total_len,
                            size_t payload_len) {
    if (!ddp || !direction)
        return;
    const char *type_name = ddp_type_name(ddp->type);
    if (type_name) {
        LOG(level, "DDP %s [%s] type=0x%02X total=%u payload=%zu dstSock=%u srcSock=%u", direction, type_name,
            (unsigned)ddp->type, (unsigned)total_len, payload_len, (unsigned)ddp->dst_socket,
            (unsigned)ddp->src_socket);
    } else {
        LOG(level, "DDP %s type=0x%02X total=%u payload=%zu dstSock=%u srcSock=%u", direction, (unsigned)ddp->type,
            (unsigned)total_len, payload_len, (unsigned)ddp->dst_socket, (unsigned)ddp->src_socket);
    }
}

// Construct a DDP reply header by reversing source and destination
static void ddp_setup_reply(const ddp_header_t *request, ddp_header_t *reply) {
    reply->llap.dst = request->llap.src;
    reply->llap.src = LLAP_HOST_NODE;
    reply->llap.type = request->llap.type;

    reply->len = request->len;
    reply->dst_net = request->src_net;
    reply->src_net = request->dst_net;

    reply->dst_socket = request->src_socket;
    reply->src_socket = request->dst_socket;
    reply->type = request->type; // preserve DDP protocol type in reply
}

// ============================================================================
// Shell Commands
// ============================================================================

static int usage_atalk_add(void) {
    printf("Usage: atalk-share-add <name> <path>\n");
    return 0;
}

static uint64_t cmd_atalk_share_add(int argc, char *argv[]) {
    if (argc != 3)
        return usage_atalk_add();
    return atalk_share_add(argv[1], argv[2]);
}

static uint64_t cmd_atalk_share_list(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return atalk_share_list();
}

static int usage_atalk_rm(void) {
    printf("Usage: atalk-share-remove <name>\n");
    return 0;
}

static uint64_t cmd_atalk_share_remove(int argc, char *argv[]) {
    if (argc != 2)
        return usage_atalk_rm();
    return atalk_share_remove(argv[1]);
}

static uint64_t cmd_atalk_printer(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: atalk-printer enable [name] | disable\n");
        return 0;
    }
    if (strcmp(argv[1], "enable") == 0) {
        const char *nm = (argc >= 3) ? argv[2] : NULL;
        return atalk_printer_enable(nm);
    } else if (strcmp(argv[1], "disable") == 0) {
        return atalk_printer_disable();
    } else {
        printf("Unknown subcommand '%s'\n", argv[1]);
        return -1;
    }
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

void appletalk_init(scheduler_t *scheduler, scc_t *scc, checkpoint_t *checkpoint) {
    (void)checkpoint;
    g_scc = scc; // Store SCC dependency for later use
    g_scheduler = scheduler; // Store scheduler for ATP timers
    atalk_server_init();
    register_cmd("atalk-share-add", "AppleTalk",
                 "atalk-share-add <name> <path>  – add a volume to the 'Shared Folders' AFP server from MEMFS path",
                 &cmd_atalk_share_add);
    register_cmd("atalk-share-list", "AppleTalk", "atalk-share-list            – list defined AppleShare volumes",
                 &cmd_atalk_share_list);
    register_cmd("atalk-share-remove", "AppleTalk", "atalk-share-remove <name>   – remove a defined AppleShare volume",
                 &cmd_atalk_share_remove);
    register_cmd("atalk-printer", "AppleTalk",
                 "atalk-printer enable [name] | disable  – control LaserWriter advertisement", &cmd_atalk_printer);
    atalk_printer_register();

    static const atp_socket_handler_t asp_handler = {.handle_request = asp_in};
    atp_register_socket_handler(HOST_AFP_SOCKET, &asp_handler, NULL);
    atp_register_socket_handler(HOST_AFP_COMPAT_SOCKET, &asp_handler, NULL);
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

void appletalk_delete(void) {
    // No persistent global state to clean up
}

// Send a DDP packet to the Mac via LocalTalk
static void ddp_send(const ddp_header_t *header, const uint8_t *data, int size) {
    if (!header || size <= 0 || size > DDP_MAX_DATA_SIZE)
        return;

    uint8_t buffer[DDP_SHORT_HEADER_SIZE + DDP_MAX_DATA_SIZE];
    uint16_t length = (uint16_t)(size + DDP_SHORT_HEADER_SIZE);

    ddp_log_summary(5, "-> Mac", header, length, (size_t)size);

    // Short DDP header: low 2 bits carry the high bits of the 10-bit length.
    buffer[0] = (uint8_t)((length >> 8) & 0x03);
    buffer[1] = length & 0xFF;
    buffer[2] = header->dst_socket;
    buffer[3] = header->src_socket;
    buffer[4] = header->type;
    memcpy(&buffer[5], data, (size_t)size);

    LOG_INDENT(4);
    log_hex(9, "DDP tx dump", buffer, length);
    LOG_INDENT(-4);

    llap_send(&header->llap, buffer, length);
}

//

// Process an incoming DDP packet and dispatch to appropriate protocol handler
void ddp_in(ddp_header_t *ddp, uint8_t *buf, size_t len) {
    switch (ddp->type) {

    case DDP_NBP:
        // [1] each node implements an nbp process on socket number 2
        if (ddp->dst_socket != 2) {
            LOG(4, "NBP rx on unexpected dstSock=%u (expected 2)", (unsigned)ddp->dst_socket);
        }
        nbp_in(ddp, buf, len);
        break;

    case DDP_ATP:
        // Accept ATP for AFP sockets (8/54) and for PAP printer socket
        if (ddp->dst_socket == HOST_AFP_SOCKET || ddp->dst_socket == HOST_AFP_COMPAT_SOCKET ||
            ddp->dst_socket == HOST_PAP_SOCKET) {
            atp_in(ddp, buf, (int)len);
        } else {
            // Ignore non-AFP ATP for now (e.g., PAP to come later) but trace at level 3.
            LOG(3, "ATP rx for dstSock=%u not handled (expect 8/54)", (unsigned)ddp->dst_socket);
        }
        break;

    case DDP_AEP:
        // AppleTalk Echo Protocol – reply by echoing payload
        LOG(3, "AEP rx len=%zu – echoing", len);
        {
            ddp_header_t reply;
            ddp_setup_reply(ddp, &reply);
            reply.type = DDP_AEP; // ensure protocol type preserved
            ddp_send(&reply, buf, (int)len);
        }
        break;

    case DDP_RTMP_REQUEST:
        // Minimal RTMP request handling stub
        if (!(len == 1 && buf[0] == 1)) {
            LOG(3, "RTMP req unexpected payload len=%zu first=0x%02X", len, len ? buf[0] : 0);
        }
        break;

    default:
        LOG(3, "DDP unknown type=0x%02X len=%zu", (unsigned)ddp->type, len);
        break;
    }
}

// Process an incoming short DDP header and dispatch to ddp_in
void ddp_short_in(llap_header_t *llap, uint8_t *buf, size_t len) {
    ddp_header_t ddp;

    assert(len >= DDP_SHORT_HEADER_SIZE);

    // Decode 10-bit length from short header: low 2 bits from first byte, then full second byte.
    ddp.len = (uint16_t)(((buf[0] & 0x03) << 8) | buf[1]);

    assert(ddp.len == len);

    assert(len <= DDP_MAX_DATA_SIZE + DDP_SHORT_HEADER_SIZE);

    ddp.llap = *llap;
    ddp.checksum = 0;
    ddp.dst_net = 0;
    ddp.src_net = 0;
    ddp.dst_socket = buf[2];
    ddp.src_socket = buf[3];
    ddp.type = buf[4];

    size_t payload_len = (len > DDP_SHORT_HEADER_SIZE) ? (len - DDP_SHORT_HEADER_SIZE) : 0;
    ddp_log_summary(4, "<- Mac", &ddp, ddp.len, payload_len);

    LOG_INDENT(4);
    // Full DDP hexdump at high verbosity (includes 5-byte header + payload)
    log_hex(9, "DDP rx dump", buf, len);
    ddp_in(&ddp, buf + 5, len - 5);
    LOG_INDENT(-4);
}

#define NBP_BRRQ       1
#define NBP_LKUP       2
#define NBP_LKUP_REPLY 3
#define NBP_FWDREQ     4

// Returns a terse description of the NBP function code or NULL if unknown.
static const char *nbp_function_name(int function) {
    switch (function) {
    case NBP_BRRQ:
        return "BrRq";
    case NBP_LKUP:
        return "Lookup";
    case NBP_LKUP_REPLY:
        return "LookupReply";
    case NBP_FWDREQ:
        return "FwdReq";
    default:
        return NULL;
    }
}

typedef struct {

    int function : 4;
    int tuple_count : 4;
    uint8_t nbp_id;

} nbp_header_t;

typedef struct {

    uint16_t net;
    uint8_t node;
    uint8_t socket;
    uint8_t enumerator;
    int object_len;
    uint8_t object[33];
    int type_len;
    uint8_t type[33];
    int zone_len;
    uint8_t zone[33];

} nbp_tuple_t;

// Helper: parse an NBP length-prefixed (P-string style) up to 32 bytes.
// Advances *p and reduces *len; writes dst and dst_len on success.
static bool nbp_parse_pstr32(const uint8_t **p, int *len, uint8_t *dst, int *dst_len) {
    if (!p || !*p || !len || *len < 1 || !dst || !dst_len)
        return false;
    uint8_t n = (*p)[0];
    if (n > 32)
        return false;
    if (*len < 1 + (int)n)
        return false;
    *dst_len = n;
    if (n)
        memcpy(dst, (*p) + 1, n);
    dst[n] = '\0';
    *p += 1 + n;
    *len -= 1 + n;
    return true;
}

// ATP layer wrappers (parallel to DDP): parse, setup reply, and send
/* atp_send prototype declared later after atp_packet_t typedef */
// Logging: implicit category for this file
LOG_USE_CATEGORY_NAME("appletalk");

// --------------- Hex dump helper for high-verbosity diagnostics ---------------
// Emit a multi-line hex dump with ASCII gutter to the AppleTalk log category
static void log_hex(int level, const char *tag, const uint8_t *data, size_t len) {
    if (!data || len == 0) {
        LOG(level, "%s: <empty>", tag ? tag : "HEX");
        return;
    }
    // Build per-line hex with ASCII gutter (16 bytes per line)
    char hexbuf[3 * 16 + 2 + 1]; // "XX " * 16 + space after 8 + NUL
    char asciibuf[16 + 1]; // 16 chars + NUL

    for (size_t off = 0; off < len; off += 16) {
        size_t n = (len - off) < 16 ? (len - off) : 16;

        // Hex field
        size_t hp = 0;
        for (size_t i = 0; i < 16; i++) {
            size_t left = sizeof(hexbuf) - hp;
            if (i < n) {
                if (left <= 1) {
                    hexbuf[sizeof(hexbuf) - 1] = '\0';
                    break;
                }
                int wrote = snprintf(&hexbuf[hp], left, "%02X ", data[off + i]);
                if (wrote < 0) {
                    hexbuf[hp] = '\0';
                    break;
                }
                size_t adv = (size_t)wrote;
                if (adv >= left) { // truncated; keep NUL and stop
                    hp = sizeof(hexbuf) - 1;
                    hexbuf[hp] = '\0';
                    break;
                }
                hp += adv;
            } else {
                // Pad alignment when fewer than 16 bytes this row
                if (left >= 4) {
                    hexbuf[hp++] = ' ';
                    hexbuf[hp++] = ' ';
                    hexbuf[hp++] = ' ';
                }
            }
            if (i == 7 && hp < sizeof(hexbuf) - 1)
                hexbuf[hp++] = ' ';
        }
        hexbuf[hp] = '\0';

        // ASCII gutter
        for (size_t i = 0; i < n; i++) {
            unsigned char c = data[off + i];
            asciibuf[i] = (c >= 32 && c <= 126) ? (char)c : '.';
        }
        asciibuf[n] = '\0';

        LOG(level, "%04" PRIx64 ": %s | %s", (uint64_t)off, hexbuf, asciibuf);
    }
}

// =============================== NBP (Name Binding Protocol) ===============================
#define NBP_OBJECT_MAX            32
#define NBP_TYPE_MAX              32
#define NBP_ZONE_MAX              32
#define NBP_MAX_ENTRIES           16
#define NBP_MAX_TUPLES_PER_PACKET 8
#define NBP_APPROX_CHAR           0xC5 // MacRoman "≈" wildcard per Inside AppleTalk

struct atalk_nbp_entry {
    bool in_use;
    char object[NBP_OBJECT_MAX + 1];
    char type[NBP_TYPE_MAX + 1];
    char zone[NBP_ZONE_MAX + 1];
    uint8_t object_len;
    uint8_t type_len;
    uint8_t zone_len;
    uint16_t net;
    uint8_t node;
    uint8_t socket;
    uint8_t enumerator;
};

static atalk_nbp_entry_t g_nbp_entries[NBP_MAX_ENTRIES];
static uint8_t g_nbp_next_enum[256];

static void nbp_send(const ddp_header_t *ddp_header, const nbp_header_t *nbp_header, const nbp_tuple_t *nbp_tuple);

static uint8_t nbp_ascii_fold(uint8_t ch) {
    if (ch >= 'A' && ch <= 'Z')
        return (uint8_t)(ch + ('a' - 'A'));
    return ch;
}

static int nbp_entry_index(const atalk_nbp_entry_t *entry) {
    if (!entry)
        return -1;
    for (int i = 0; i < NBP_MAX_ENTRIES; i++) {
        if (&g_nbp_entries[i] == entry)
            return i;
    }
    return -1;
}

static int nbp_copy_field(char *dst, size_t dst_cap, const char *src, bool allow_empty) {
    if (!dst || dst_cap == 0)
        return -1;
    if (!src)
        src = "";
    size_t len = strlen(src);
    if (!allow_empty && len == 0)
        return -1;
    if (len > dst_cap - 1)
        return -1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return (int)len;
}

static uint8_t nbp_alloc_enumerator(uint8_t socket) {
    uint8_t next = g_nbp_next_enum[socket];
    if (next == 0)
        next = 1;
    for (int attempt = 0; attempt < 255; attempt++) {
        bool collision = false;
        for (int i = 0; i < NBP_MAX_ENTRIES; i++) {
            const atalk_nbp_entry_t *entry = &g_nbp_entries[i];
            if (!entry->in_use)
                continue;
            if (entry->socket == socket && entry->enumerator == next) {
                collision = true;
                break;
            }
        }
        if (!collision) {
            g_nbp_next_enum[socket] = (next == 255) ? 1 : (uint8_t)(next + 1);
            return next;
        }
        next = (next == 255) ? 1 : (uint8_t)(next + 1);
    }
    return next; // fallback, though we should always return earlier
}

static bool nbp_field_equals_ci(const char *lhs, uint8_t lhs_len, const char *rhs, uint8_t rhs_len) {
    if (lhs_len != rhs_len)
        return false;
    for (uint8_t i = 0; i < lhs_len; i++) {
        if (nbp_ascii_fold((uint8_t)lhs[i]) != nbp_ascii_fold((uint8_t)rhs[i]))
            return false;
    }
    return true;
}

static bool nbp_entry_conflicts(const atalk_nbp_entry_t *candidate, int skip_index) {
    for (int i = 0; i < NBP_MAX_ENTRIES; i++) {
        if (i == skip_index)
            continue;
        const atalk_nbp_entry_t *existing = &g_nbp_entries[i];
        if (!existing->in_use)
            continue;
        if (!nbp_field_equals_ci(existing->object, existing->object_len, candidate->object, candidate->object_len))
            continue;
        if (!nbp_field_equals_ci(existing->type, existing->type_len, candidate->type, candidate->type_len))
            continue;
        if (!nbp_field_equals_ci(existing->zone, existing->zone_len, candidate->zone, candidate->zone_len))
            continue;
        return true;
    }
    return false;
}

static int nbp_populate_entry(atalk_nbp_entry_t *dst, const atalk_nbp_service_desc_t *desc) {
    if (!dst || !desc || !desc->object || !desc->type || desc->socket == 0)
        return -1;
    atalk_nbp_entry_t temp;
    memset(&temp, 0, sizeof(temp));
    temp.net = desc->net;
    temp.node = desc->node ? desc->node : LLAP_HOST_NODE;
    temp.socket = desc->socket;

    int obj_len = nbp_copy_field(temp.object, sizeof(temp.object), desc->object, false);
    if (obj_len < 0)
        return -1;
    temp.object_len = (uint8_t)obj_len;

    int type_len = nbp_copy_field(temp.type, sizeof(temp.type), desc->type, false);
    if (type_len < 0)
        return -1;
    temp.type_len = (uint8_t)type_len;

    const char *zone_src = (desc->zone && desc->zone[0]) ? desc->zone : "*";
    int zone_len = nbp_copy_field(temp.zone, sizeof(temp.zone), zone_src, false);
    if (zone_len < 0)
        return -1;
    temp.zone_len = (uint8_t)zone_len;

    *dst = temp;
    return 0;
}

int atalk_nbp_register(const atalk_nbp_service_desc_t *desc, atalk_nbp_entry_t **out_entry) {
    if (!desc)
        return -1;
    int free_slot = -1;
    for (int i = 0; i < NBP_MAX_ENTRIES; i++) {
        if (!g_nbp_entries[i].in_use) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        LOG(1, "NBP: registry full, cannot register '%s:%s'", desc->object ? desc->object : "",
            desc->type ? desc->type : "");
        return -1;
    }

    atalk_nbp_entry_t candidate;
    if (nbp_populate_entry(&candidate, desc) != 0)
        return -1;
    candidate.enumerator = nbp_alloc_enumerator(candidate.socket);
    candidate.in_use = true;

    if (nbp_entry_conflicts(&candidate, -1)) {
        LOG(2, "NBP: name conflict for '%s:%s@%s'", candidate.object, candidate.type, candidate.zone);
        return -1;
    }

    g_nbp_entries[free_slot] = candidate;
    if (out_entry)
        *out_entry = &g_nbp_entries[free_slot];
    LOG(3, "NBP register: object='%s' type='%s' zone='%s' socket=%u enum=%u", candidate.object, candidate.type,
        candidate.zone, (unsigned)candidate.socket, (unsigned)candidate.enumerator);
    return 0;
}

int atalk_nbp_update(atalk_nbp_entry_t *entry, const atalk_nbp_service_desc_t *desc) {
    int idx = nbp_entry_index(entry);
    if (idx < 0 || !desc)
        return -1;

    atalk_nbp_entry_t candidate;
    if (nbp_populate_entry(&candidate, desc) != 0)
        return -1;
    candidate.in_use = true;
    if (candidate.socket == g_nbp_entries[idx].socket)
        candidate.enumerator = g_nbp_entries[idx].enumerator;
    else
        candidate.enumerator = nbp_alloc_enumerator(candidate.socket);

    if (nbp_entry_conflicts(&candidate, idx))
        return -1;

    g_nbp_entries[idx] = candidate;
    return 0;
}

int atalk_nbp_unregister(atalk_nbp_entry_t *entry) {
    int idx = nbp_entry_index(entry);
    if (idx < 0)
        return -1;
    if (!g_nbp_entries[idx].in_use)
        return 0;
    LOG(3, "NBP unregister: object='%s' type='%s'", g_nbp_entries[idx].object, g_nbp_entries[idx].type);
    memset(&g_nbp_entries[idx], 0, sizeof(g_nbp_entries[idx]));
    return 0;
}

static bool nbp_pattern_is_all(const uint8_t *field, int len) {
    return (len == 1 && field[0] == '=');
}

static bool nbp_zone_query_is_wildcard(const nbp_tuple_t *tuple) {
    return (tuple->zone_len == 0) || (tuple->zone_len == 1 && tuple->zone[0] == '*');
}

static bool nbp_glob_match_ci_impl(const char *value, int value_len, const uint8_t *pattern, int pos, int pat_len) {
    while (pos < pat_len) {
        uint8_t folded = nbp_ascii_fold(pattern[pos]);
        if (folded == NBP_APPROX_CHAR) {
            while (pos < pat_len && nbp_ascii_fold(pattern[pos]) == NBP_APPROX_CHAR)
                pos++;
            if (pos == pat_len)
                return true; // trailing wildcard matches rest of string
            for (int i = 0; i <= value_len; i++) {
                if (nbp_glob_match_ci_impl(value + i, value_len - i, pattern, pos, pat_len))
                    return true;
            }
            return false;
        }
        if (value_len == 0)
            return false;
        if (nbp_ascii_fold((uint8_t)value[0]) != folded)
            return false;
        value++;
        value_len--;
        pos++;
    }
    return value_len == 0;
}

static bool nbp_glob_match_ci(const char *value, int value_len, const uint8_t *pattern, int pat_len) {
    return nbp_glob_match_ci_impl(value, value_len, pattern, 0, pat_len);
}

static bool nbp_field_matches(const char *value, uint8_t value_len, const uint8_t *pattern, int pattern_len) {
    if (pattern_len <= 0)
        return value_len == 0;
    if (nbp_pattern_is_all(pattern, pattern_len))
        return true;
    return nbp_glob_match_ci(value, value_len, pattern, pattern_len);
}

static bool nbp_zone_matches(const nbp_tuple_t *query, const atalk_nbp_entry_t *entry) {
    if (nbp_zone_query_is_wildcard(query))
        return true;
    if (entry->zone_len == 1 && entry->zone[0] == '*')
        return true; // default zone matches all queries
    return nbp_field_matches(entry->zone, entry->zone_len, query->zone, query->zone_len);
}

static void nbp_build_tuple_from_entry(const atalk_nbp_entry_t *entry, nbp_tuple_t *tuple) {
    memset(tuple, 0, sizeof(*tuple));
    tuple->net = entry->net;
    tuple->node = entry->node;
    tuple->socket = entry->socket;
    tuple->enumerator = entry->enumerator;
    tuple->object_len = entry->object_len;
    memcpy(tuple->object, entry->object, entry->object_len);
    tuple->type_len = entry->type_len;
    memcpy(tuple->type, entry->type, entry->type_len);
    tuple->zone_len = entry->zone_len;
    memcpy(tuple->zone, entry->zone, entry->zone_len);
}

static void nbp_send_reply_batch(const ddp_header_t *request, uint8_t nbp_id, const nbp_tuple_t *tuples, int count) {
    if (count <= 0)
        return;
    ddp_header_t reply;
    nbp_header_t nbp_header;
    ddp_setup_reply(request, &reply);
    nbp_header.function = NBP_LKUP_REPLY;
    nbp_header.tuple_count = (count > 15) ? 15 : count;
    nbp_header.nbp_id = nbp_id;
    nbp_send(&reply, &nbp_header, tuples);
}

static void nbp_handle_lookup_tuple(const ddp_header_t *request, uint8_t nbp_id, const nbp_tuple_t *query) {
    if (!request || !query)
        return;
    nbp_tuple_t batch[NBP_MAX_TUPLES_PER_PACKET];
    int batch_len = 0;

    for (int i = 0; i < NBP_MAX_ENTRIES; i++) {
        const atalk_nbp_entry_t *entry = &g_nbp_entries[i];
        if (!entry->in_use)
            continue;
        if (!nbp_zone_matches(query, entry))
            continue;
        if (!nbp_field_matches(entry->type, entry->type_len, query->type, query->type_len))
            continue;
        if (!nbp_field_matches(entry->object, entry->object_len, query->object, query->object_len))
            continue;
        nbp_build_tuple_from_entry(entry, &batch[batch_len++]);
        if (batch_len == NBP_MAX_TUPLES_PER_PACKET) {
            nbp_send_reply_batch(request, nbp_id, batch, batch_len);
            batch_len = 0;
        }
    }
    if (batch_len > 0)
        nbp_send_reply_batch(request, nbp_id, batch, batch_len);
}

static void nbp_dispatch(const ddp_header_t *ddp_header, const nbp_header_t *header, nbp_tuple_t *tuples,
                         int tuple_count) {
    if (!header || !ddp_header)
        return;
    switch (header->function) {
    case NBP_BRRQ:
    case NBP_LKUP:
    case NBP_FWDREQ:
        for (int i = 0; i < tuple_count; i++)
            nbp_handle_lookup_tuple(ddp_header, header->nbp_id, &tuples[i]);
        break;
    case NBP_LKUP_REPLY:
        // No local lookups issued; ignore replies
        break;
    default:
        LOG(4, "NBP: unsupported function %d", header->function);
        break;
    }
}

static void nbp_parse_and_dispatch(const ddp_header_t *ddp, const uint8_t *buf, size_t len) {
    if (!ddp || !buf || len < 2)
        return;
    nbp_header_t header;
    nbp_tuple_t tuples[32];
    int parsed = 0;

    const uint8_t *p = buf;
    int rem = (int)len;
    header.function = (p[0] >> 4) & 0x0F;
    header.tuple_count = p[0] & 0x0F;
    header.nbp_id = p[1];
    p += 2;
    rem -= 2;

    for (int i = 0; i < header.tuple_count && parsed < (int)ARRAY_LEN(tuples); i++) {
        if (rem < 8)
            break;
        tuples[parsed].net = (uint16_t)((p[0] << 8) | p[1]);
        tuples[parsed].node = p[2];
        tuples[parsed].socket = p[3];
        tuples[parsed].enumerator = p[4];
        p += 5;
        rem -= 5;
        if (!nbp_parse_pstr32(&p, &rem, tuples[parsed].object, &tuples[parsed].object_len))
            break;
        if (!nbp_parse_pstr32(&p, &rem, tuples[parsed].type, &tuples[parsed].type_len))
            break;
        if (!nbp_parse_pstr32(&p, &rem, tuples[parsed].zone, &tuples[parsed].zone_len))
            break;
        parsed++;
    }

    nbp_dispatch(ddp, &header, tuples, parsed);
}

static void nbp_send(const ddp_header_t *ddp_header, const nbp_header_t *nbp_header, const nbp_tuple_t *nbp_tuple) {
    uint8_t buffer[DDP_MAX_DATA_SIZE];
    int size = 0;

#define NBP_ENSURE(nbytes)                                                                                             \
    do {                                                                                                               \
        if (size + (int)(nbytes) > DDP_MAX_DATA_SIZE) {                                                                \
            LOG(2, "NBP reply too large (%d) – refusing", size + (int)(nbytes));                                       \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

    NBP_ENSURE(2);
    buffer[size++] = (uint8_t)(((nbp_header->function & 0x0F) << 4) | (nbp_header->tuple_count & 0x0F));
    buffer[size++] = nbp_header->nbp_id;

    for (int i = 0; i < nbp_header->tuple_count; i++) {
        NBP_ENSURE(5);
        buffer[size++] = (uint8_t)((nbp_tuple[i].net >> 8) & 0xFF);
        buffer[size++] = (uint8_t)(nbp_tuple[i].net & 0xFF);
        buffer[size++] = nbp_tuple[i].node;
        buffer[size++] = nbp_tuple[i].socket;
        buffer[size++] = nbp_tuple[i].enumerator;

        int o_len = nbp_tuple[i].object_len;
        NBP_ENSURE(1 + o_len);
        buffer[size++] = (uint8_t)o_len;
        if (o_len) {
            memcpy(&buffer[size], nbp_tuple[i].object, (size_t)o_len);
            size += o_len;
        }

        int t_len = nbp_tuple[i].type_len;
        NBP_ENSURE(1 + t_len);
        buffer[size++] = (uint8_t)t_len;
        if (t_len) {
            memcpy(&buffer[size], nbp_tuple[i].type, (size_t)t_len);
            size += t_len;
        }

        int z_len = nbp_tuple[i].zone_len;
        NBP_ENSURE(1 + z_len);
        buffer[size++] = (uint8_t)z_len;
        if (z_len) {
            memcpy(&buffer[size], nbp_tuple[i].zone, (size_t)z_len);
            size += z_len;
        }
    }

    const char *fn_name = nbp_function_name(nbp_header->function);
    if (fn_name) {
        LOG(5, "NBP -> Mac %s: tuples=%d nbpId=%u bytes=%d", fn_name, nbp_header->tuple_count,
            (unsigned)nbp_header->nbp_id, size);
    } else {
        LOG(5, "NBP -> Mac func=%d: tuples=%d nbpId=%u bytes=%d", nbp_header->function, nbp_header->tuple_count,
            (unsigned)nbp_header->nbp_id, size);
    }
    LOG_INDENT(4);
    ddp_send(ddp_header, buffer, size);
    LOG_INDENT(-4);

#undef NBP_ENSURE
}

void nbp_in(ddp_header_t *ddp, uint8_t *buf, size_t len) {
    uint8_t header_byte = (len >= 1) ? buf[0] : 0;
    uint8_t nbp_id = (len >= 2) ? buf[1] : 0;
    int function = (header_byte >> 4) & 0x0F;
    int tuple_count = header_byte & 0x0F;
    const char *fn_name = nbp_function_name(function);
    if (fn_name) {
        LOG(4, "NBP <- Mac %s: tuples=%d nbpId=%u len=%zu", fn_name, tuple_count, (unsigned)nbp_id, len);
    } else {
        LOG(4, "NBP <- Mac func=%d: tuples=%d nbpId=%u len=%zu", function, tuple_count, (unsigned)nbp_id, len);
    }
    LOG_INDENT(4);
    nbp_parse_and_dispatch(ddp, buf, len);
    LOG_INDENT(-4);
}

// =============================== ATP (AppleTalk Transaction Protocol) ===============================

#define ATP_MAX_HANDLERS           8
#define ATP_MAX_OUTGOING           16
#define ATP_MAX_XO_CACHE           16
#define ATP_MAX_RESPONSE_FRAGMENTS 8
#define ATP_MAX_ATP_PAYLOAD        578
#define ATP_DEFAULT_RETRY_MS       2000u

typedef struct {
    bool in_use;
    uint8_t socket;
    atp_socket_handler_t handler;
    void *ctx;
} atp_handler_slot_t;

struct atp_request_handle {
    bool in_use;
    bool xo;
    bool infinite_retries;
    uint8_t src_socket;
    uint16_t tid;
    uint8_t base_ctl;
    uint8_t trel_hint;
    uint8_t initial_bitmap;
    uint8_t pending_bitmap;
    uint8_t user[4];
    uint8_t payload[ATP_MAX_ATP_PAYLOAD];
    int payload_len;
    atalk_socket_addr_t dest;
    uint32_t retry_timeout_ns;
    int retries_remaining;
    uint32_t timer_generation;
    atp_request_callbacks_t callbacks;
    void *cb_ctx;
};

typedef struct {
    bool valid;
    uint8_t seq;
    int len;
    uint8_t bytes[DDP_MAX_DATA_SIZE];
} atp_resp_packet_cache_t;

typedef struct {
    bool in_use;
    bool response_ready;
    uint16_t tid;
    uint8_t requester_node;
    uint8_t requester_socket;
    uint8_t responder_socket;
    uint8_t trel_hint;
    uint32_t release_generation;
    atp_resp_packet_cache_t packets[ATP_MAX_RESPONSE_FRAGMENTS];
} atp_xo_entry_t;

static atp_handler_slot_t g_atp_handlers[ATP_MAX_HANDLERS];
static atp_request_handle_t g_atp_requests[ATP_MAX_OUTGOING];
static atp_xo_entry_t g_xo_entries[ATP_MAX_XO_CACHE];
static uint16_t g_next_tid = 0x2000;
static bool g_atp_events_registered = false;
static int g_atp_retry_event_token;
static int g_atp_release_event_token;

// Utility helpers -----------------------------------------------------------
static void atp_register_scheduler_events(void);
static void atp_retry_timeout_cb(void *source, uint64_t data);
static void atp_release_timeout_cb(void *source, uint64_t data);
static int parse_atp(const uint8_t *buf, int len, atp_packet_t *atp);

static uint64_t atp_ms_to_ns(uint32_t ms) {
    if (ms == 0)
        return (uint64_t)ATP_DEFAULT_RETRY_MS * 1000000ULL;
    return (uint64_t)ms * 1000000ULL;
}

static uint64_t atp_seconds_to_ns(uint32_t seconds) {
    return (uint64_t)seconds * 1000000000ULL;
}

static uint32_t atp_trel_hint_seconds(uint8_t hint) {
    switch (hint & 0x07) {
    case 0:
        return 30;
    case 1:
        return 60;
    case 2:
        return 120;
    case 3:
        return 240;
    case 4:
        return 480;
    default:
        return 30;
    }
}

static uint64_t atp_encode_event_data(uint16_t index, uint32_t generation) {
    return ((uint64_t)generation << 32) | (uint64_t)index;
}

static bool atp_decode_event_data(uint64_t data, uint16_t *index, uint32_t *generation) {
    if (!index || !generation)
        return false;
    *index = (uint16_t)(data & 0xFFFFu);
    *generation = (uint32_t)(data >> 32);
    return true;
}

static void atp_register_scheduler_events(void) {
    if (g_atp_events_registered)
        return;
    if (!g_scheduler)
        return;
    scheduler_new_event_type(g_scheduler, "atp", &g_atp_retry_event_token, "retry_timeout", &atp_retry_timeout_cb);
    scheduler_new_event_type(g_scheduler, "atp", &g_atp_release_event_token, "xo_release", &atp_release_timeout_cb);
    g_atp_events_registered = true;
}

// Handler registry ---------------------------------------------------------
static atp_handler_slot_t *atp_find_handler_slot(uint8_t socket) {
    for (int i = 0; i < ATP_MAX_HANDLERS; i++) {
        if (g_atp_handlers[i].in_use && g_atp_handlers[i].socket == socket)
            return &g_atp_handlers[i];
    }
    return NULL;
}

int atp_register_socket_handler(uint8_t socket, const atp_socket_handler_t *handler, void *ctx) {
    if (!handler || !handler->handle_request)
        return -1;
    atp_handler_slot_t *existing = atp_find_handler_slot(socket);
    if (existing) {
        existing->handler = *handler;
        existing->ctx = ctx;
        return 0;
    }
    for (int i = 0; i < ATP_MAX_HANDLERS; i++) {
        if (!g_atp_handlers[i].in_use) {
            g_atp_handlers[i].in_use = true;
            g_atp_handlers[i].socket = socket;
            g_atp_handlers[i].handler = *handler;
            g_atp_handlers[i].ctx = ctx;
            return 0;
        }
    }
    LOG(1, "ATP: handler table full, cannot register socket %u", (unsigned)socket);
    return -1;
}

void atp_unregister_socket_handler(uint8_t socket) {
    atp_handler_slot_t *slot = atp_find_handler_slot(socket);
    if (slot)
        slot->in_use = false;
}

// Request ID generator -----------------------------------------------------
static uint16_t atp_next_tid(uint8_t src_socket) {
    uint16_t start = g_next_tid;
    while (true) {
        g_next_tid++;
        if (g_next_tid == start)
            break;
        bool in_use = false;
        for (int i = 0; i < ATP_MAX_OUTGOING; i++) {
            if (g_atp_requests[i].in_use && g_atp_requests[i].src_socket == src_socket &&
                g_atp_requests[i].tid == g_next_tid) {
                in_use = true;
                break;
            }
        }
        if (!in_use)
            return g_next_tid;
    }
    return g_next_tid;
}

static atp_request_handle_t *atp_alloc_request_slot(void) {
    for (int i = 0; i < ATP_MAX_OUTGOING; i++) {
        if (!g_atp_requests[i].in_use) {
            memset(&g_atp_requests[i], 0, sizeof(g_atp_requests[i]));
            g_atp_requests[i].in_use = true;
            return &g_atp_requests[i];
        }
    }
    return NULL;
}

static void atp_request_complete(atp_request_handle_t *req, atp_request_result_t result) {
    if (!req || !req->in_use)
        return;
    req->timer_generation++;
    req->in_use = false;
    if (req->callbacks.on_complete)
        req->callbacks.on_complete(req, result, req->cb_ctx);
}

static void atp_send_trel(const atp_request_handle_t *req) {
    if (!req || !req->xo)
        return;
    uint8_t buffer[8] = {0};
    // TRel packets should have the lower 3 bits set to zero (Inside AppleTalk, p. 9-13)
    buffer[0] = ATP_CONTROL_TREL;
    buffer[2] = (uint8_t)((req->tid >> 8) & 0xFF);
    buffer[3] = (uint8_t)(req->tid & 0xFF);
    ddp_header_t ddp;
    memset(&ddp, 0, sizeof(ddp));
    ddp.llap.dst = req->dest.node;
    ddp.llap.src = LLAP_HOST_NODE;
    ddp.llap.type = LLAP_DDP_SHORT;
    ddp.len = (uint16_t)(DDP_SHORT_HEADER_SIZE + sizeof(buffer));
    ddp.dst_socket = req->dest.socket;
    ddp.src_socket = req->src_socket;
    ddp.type = DDP_ATP;
    ddp_send(&ddp, buffer, sizeof(buffer));
    LOG(3, "ATP: sent TRel tid=0x%04X srcSock=%u dstSock=%u", req->tid, (unsigned)req->src_socket,
        (unsigned)req->dest.socket);
}

static void atp_send_request_packets(atp_request_handle_t *req, uint8_t bitmap) {
    uint8_t atp_buf[DDP_MAX_DATA_SIZE];
    atp_buf[0] = req->base_ctl;
    atp_buf[1] = bitmap;
    atp_buf[2] = (uint8_t)((req->tid >> 8) & 0xFF);
    atp_buf[3] = (uint8_t)(req->tid & 0xFF);
    memcpy(&atp_buf[4], req->user, 4);
    if (req->payload_len > 0)
        memcpy(&atp_buf[8], req->payload, (size_t)req->payload_len);
    int total = 8 + req->payload_len;

    ddp_header_t ddp;
    memset(&ddp, 0, sizeof(ddp));
    ddp.llap.dst = req->dest.node;
    ddp.llap.src = LLAP_HOST_NODE;
    ddp.llap.type = LLAP_DDP_SHORT;
    ddp.len = (uint16_t)(total + DDP_SHORT_HEADER_SIZE);
    ddp.dst_socket = req->dest.socket;
    ddp.src_socket = req->src_socket;
    ddp.type = DDP_ATP;
    ddp_send(&ddp, atp_buf, total);
    LOG(3, "ATP: sent TReq tid=0x%04X srcSock=%u dstSock=%u bitmap=0x%02X", req->tid, (unsigned)req->src_socket,
        (unsigned)req->dest.socket, (unsigned)bitmap);
}

static void atp_arm_retry_timer(atp_request_handle_t *req) {
    atp_register_scheduler_events();
    if (!g_scheduler)
        return;
    uint16_t index = (uint16_t)(req - g_atp_requests);
    req->timer_generation++;
    uint64_t data = atp_encode_event_data(index, req->timer_generation);
    scheduler_new_cpu_event(g_scheduler, &atp_retry_timeout_cb, &g_atp_retry_event_token, data, 0,
                            req->retry_timeout_ns);
}

static void atp_retry_request(atp_request_handle_t *req, bool consume_retry) {
    if (!req || req->pending_bitmap == 0)
        return;
    if (!req->infinite_retries && consume_retry) {
        if (req->retries_remaining == 0) {
            LOG(2, "ATP: retries exhausted for tid=0x%04X", req->tid);
            atp_request_complete(req, ATP_REQUEST_RESULT_TIMEOUT);
            return;
        }
        req->retries_remaining--;
    }
    atp_send_request_packets(req, req->pending_bitmap);
    atp_arm_retry_timer(req);
}

static void atp_retry_timeout_cb(void *source, uint64_t data) {
    (void)source;
    uint16_t index;
    uint32_t generation;
    if (!atp_decode_event_data(data, &index, &generation))
        return;
    if (index >= ATP_MAX_OUTGOING)
        return;
    atp_request_handle_t *req = &g_atp_requests[index];
    if (!req->in_use || req->timer_generation != generation)
        return;
    LOG(3, "ATP: retry timeout tid=0x%04X bitmap=0x%02X", req->tid, (unsigned)req->pending_bitmap);
    atp_retry_request(req, true);
}

atp_request_handle_t *atp_request_submit(const atp_request_params_t *params, const atp_request_callbacks_t *callbacks,
                                         void *ctx) {
    if (!params || params->bitmap == 0)
        return NULL;
    if (params->payload_len < 0 || params->payload_len > ATP_MAX_ATP_PAYLOAD)
        return NULL;
    atp_request_handle_t *req = atp_alloc_request_slot();
    if (!req)
        return NULL;

    req->src_socket = params->src_socket;
    req->dest = params->dest;
    req->initial_bitmap = params->bitmap;
    req->pending_bitmap = params->bitmap;
    req->payload_len = params->payload_len;
    if (req->payload_len > 0 && params->payload)
        memcpy(req->payload, params->payload, (size_t)req->payload_len);
    memcpy(req->user, params->user, sizeof(req->user));
    req->retry_timeout_ns = atp_ms_to_ns(params->retry_timeout_ms);
    req->retries_remaining = (params->retry_limit > 0) ? params->retry_limit : 0;
    req->infinite_retries = (params->retry_limit < 0);
    req->callbacks = callbacks ? *callbacks : (atp_request_callbacks_t){0};
    req->cb_ctx = ctx;
    req->xo = (params->mode == ATP_TRANSACTION_XO);
    req->trel_hint = params->trel_timer_hint & 0x07;
    req->base_ctl = ATP_CONTROL_TREQ;
    if (req->xo) {
        req->base_ctl |= ATP_CONTROL_XO;
        req->base_ctl |= req->trel_hint;
    }
    req->tid = atp_next_tid(req->src_socket);

    atp_send_request_packets(req, req->pending_bitmap);
    atp_arm_retry_timer(req);
    return req;
}

void atp_request_cancel(atp_request_handle_t *handle) {
    if (!handle || !handle->in_use)
        return;
    LOG(3, "ATP: cancel request tid=0x%04X", handle->tid);
    atp_request_complete(handle, ATP_REQUEST_RESULT_ABORTED);
}

// XO cache helpers ---------------------------------------------------------
static int atp_xo_find(uint16_t tid, uint8_t requester_node, uint8_t requester_socket, uint8_t responder_socket) {
    for (int i = 0; i < ATP_MAX_XO_CACHE; i++) {
        if (!g_xo_entries[i].in_use)
            continue;
        if (g_xo_entries[i].tid == tid && g_xo_entries[i].requester_node == requester_node &&
            g_xo_entries[i].requester_socket == requester_socket &&
            g_xo_entries[i].responder_socket == responder_socket) {
            return i;
        }
    }
    return -1;
}

// Forward declaration for atp_xo_alloc
static void atp_xo_schedule_release(atp_xo_entry_t *entry);

static int atp_xo_alloc(const ddp_header_t *ddp, const atp_packet_t *atp) {
    for (int i = 0; i < ATP_MAX_XO_CACHE; i++) {
        if (!g_xo_entries[i].in_use) {
            memset(&g_xo_entries[i], 0, sizeof(g_xo_entries[i]));
            g_xo_entries[i].in_use = true;
            g_xo_entries[i].tid = atp->tid;
            g_xo_entries[i].requester_node = ddp->llap.src;
            g_xo_entries[i].requester_socket = ddp->src_socket;
            g_xo_entries[i].responder_socket = ddp->dst_socket;
            g_xo_entries[i].trel_hint = (uint8_t)(atp->ctl & 0x07);
            // Start release timer immediately (Inside AppleTalk p. 9-17)
            atp_xo_schedule_release(&g_xo_entries[i]);
            LOG(10, "ATP: XO alloc slot=%d tid=0x%04X node=%u sock=%u->%u trel=%u", i, atp->tid, ddp->llap.src,
                ddp->src_socket, ddp->dst_socket, atp->ctl & 0x07);
            return i;
        }
    }
    LOG(2, "ATP: XO cache full (tid=0x%04X node=%u sock=%u)", atp->tid, ddp->llap.src, ddp->src_socket);
    return -1;
}

static void atp_xo_free(atp_xo_entry_t *entry) {
    if (!entry)
        return;
    uint16_t index = (uint16_t)(entry - g_xo_entries);
    LOG(10, "ATP: XO free slot=%u tid=0x%04X", index, entry->tid);
    // Cancel pending release timer event
    if (g_scheduler) {
        uint64_t data = atp_encode_event_data(index, entry->release_generation);
        remove_event_by_data(g_scheduler, &atp_release_timeout_cb, NULL, data);
    }
    entry->in_use = false;
    entry->release_generation++;
}

static void atp_xo_store_packet(atp_xo_entry_t *entry, uint8_t seq, const uint8_t *bytes, int len) {
    if (!entry || seq >= ATP_MAX_RESPONSE_FRAGMENTS || len <= 0 || len > DDP_MAX_DATA_SIZE)
        return;
    atp_resp_packet_cache_t *slot = &entry->packets[seq];
    slot->valid = true;
    slot->seq = seq;
    slot->len = len;
    memcpy(slot->bytes, bytes, (size_t)len);
}

static void atp_xo_schedule_release(atp_xo_entry_t *entry) {
    if (!entry)
        return;
    atp_register_scheduler_events();
    if (!g_scheduler)
        return;
    uint16_t index = (uint16_t)(entry - g_xo_entries);
    // Cancel any existing release event for this entry before scheduling a new one
    uint64_t old_data = atp_encode_event_data(index, entry->release_generation);
    remove_event_by_data(g_scheduler, &atp_release_timeout_cb, NULL, old_data);
    entry->release_generation++;
    uint64_t data = atp_encode_event_data(index, entry->release_generation);
    uint32_t seconds = atp_trel_hint_seconds(entry->trel_hint);
    scheduler_new_cpu_event(g_scheduler, &atp_release_timeout_cb, &g_atp_release_event_token, data, 0,
                            atp_seconds_to_ns(seconds));
}

static void atp_release_timeout_cb(void *source, uint64_t data) {
    (void)source;
    uint16_t index;
    uint32_t generation;
    if (!atp_decode_event_data(data, &index, &generation))
        return;
    if (index >= ATP_MAX_XO_CACHE)
        return;
    atp_xo_entry_t *entry = &g_xo_entries[index];
    if (!entry->in_use || entry->release_generation != generation)
        return;
    LOG(3, "ATP: XO release timeout tid=0x%04X", entry->tid);
    atp_xo_free(entry);
}

static void atp_xo_send_cached(atp_xo_entry_t *entry, const ddp_header_t *ddp, uint8_t bitmap) {
    if (!entry || !entry->response_ready)
        return;
    LOG(6, "ATP: XO retransmit cached tid=0x%04X bitmap=0x%02X", entry->tid, bitmap);
    ddp_header_t reply;
    ddp_setup_reply(ddp, &reply);
    reply.type = DDP_ATP;
    for (int i = 0; i < ATP_MAX_RESPONSE_FRAGMENTS; i++) {
        atp_resp_packet_cache_t *slot = &entry->packets[i];
        if (!slot->valid)
            continue;
        if (!(bitmap & (1u << slot->seq)))
            continue;
        ddp_send(&reply, slot->bytes, slot->len);
    }
    atp_xo_schedule_release(entry);
}

// Outgoing response helpers -------------------------------------------------
int atp_responder_send_packets(const ddp_header_t *request_ddp, const atp_packet_t *request_atp,
                               const atp_response_packet_desc_t *packets, size_t packet_count) {
    if (!request_ddp || !request_atp || !packets || packet_count == 0)
        return -1;
    if (packet_count > ATP_MAX_RESPONSE_FRAGMENTS)
        return -1;

    ddp_header_t reply;
    ddp_setup_reply(request_ddp, &reply);
    reply.type = DDP_ATP;
    bool xo = (request_atp->ctl & ATP_CONTROL_XO) != 0;
    int xo_index = -1;
    if (xo) {
        xo_index =
            atp_xo_find(request_atp->tid, request_ddp->llap.src, request_ddp->src_socket, request_ddp->dst_socket);
        if (xo_index < 0)
            xo_index = atp_xo_alloc(request_ddp, request_atp);
    }

    for (size_t i = 0; i < packet_count; i++) {
        const atp_response_packet_desc_t *desc = &packets[i];
        if (desc->payload_len < 0 || desc->payload_len > (DDP_MAX_DATA_SIZE - 8))
            return -1;
        uint8_t buffer[DDP_MAX_DATA_SIZE];
        uint8_t ctl = ATP_CONTROL_TRESP;
        if (xo)
            ctl |= ATP_CONTROL_XO;
        if (desc->sts)
            ctl |= ATP_CONTROL_STS;
        bool is_last = (i == packet_count - 1);
        if (desc->eom || is_last)
            ctl |= ATP_CONTROL_EOM;
        buffer[0] = ctl;
        buffer[1] = (uint8_t)i;
        buffer[2] = (uint8_t)((request_atp->tid >> 8) & 0xFF);
        buffer[3] = (uint8_t)(request_atp->tid & 0xFF);
        if (desc->user)
            memcpy(&buffer[4], desc->user, 4);
        else
            memcpy(&buffer[4], request_atp->user, 4);
        if (desc->payload_len > 0 && desc->payload)
            memcpy(&buffer[8], desc->payload, (size_t)desc->payload_len);
        int total = 8 + desc->payload_len;
        ddp_send(&reply, buffer, total);
        if (xo_index >= 0)
            atp_xo_store_packet(&g_xo_entries[xo_index], (uint8_t)i, buffer, total);
    }
    if (xo_index >= 0) {
        g_xo_entries[xo_index].response_ready = true;
        atp_xo_schedule_release(&g_xo_entries[xo_index]);
    }
    return (int)packet_count;
}

int atp_responder_send_simple(const ddp_header_t *request_ddp, const atp_packet_t *request_atp, const uint8_t user[4],
                              const uint8_t *payload, int payload_len, bool sts) {
    uint8_t fallback_user[4];
    if (!user)
        memcpy(fallback_user, request_atp->user, sizeof(fallback_user));
    atp_response_packet_desc_t desc = {
        .payload = payload, .payload_len = payload_len, .user = user ? user : fallback_user, .sts = sts, .eom = true};
    return atp_responder_send_packets(request_ddp, request_atp, &desc, 1);
}

static int parse_atp(const uint8_t *buf, int len, atp_packet_t *atp) {
    if (!buf || len < 8 || !atp)
        return -1;
    atp->ctl = buf[0];
    atp->bitmap = buf[1];
    atp->tid = (uint16_t)((buf[2] << 8) | buf[3]);
    memcpy(atp->user, &buf[4], 4);
    atp->data = &buf[8];
    atp->data_len = len - 8;
    return 0;
}

// Incoming response handling -----------------------------------------------
static atp_request_handle_t *atp_match_request(const ddp_header_t *ddp, const atp_packet_t *atp) {
    for (int i = 0; i < ATP_MAX_OUTGOING; i++) {
        atp_request_handle_t *req = &g_atp_requests[i];
        if (!req->in_use)
            continue;
        if (req->tid != atp->tid)
            continue;
        if (req->src_socket != ddp->dst_socket)
            continue;
        if (req->dest.socket != ddp->src_socket || req->dest.node != ddp->llap.src || req->dest.net != ddp->src_net)
            continue;
        return req;
    }
    return NULL;
}

static void atp_handle_response(const ddp_header_t *ddp, const atp_packet_t *atp) {
    atp_request_handle_t *req = atp_match_request(ddp, atp);
    if (!req)
        return;
    uint8_t seq = atp->bitmap & 0x07;
    uint8_t mask = (uint8_t)(1u << seq);
    bool duplicate = ((req->pending_bitmap & mask) == 0);
    if (!duplicate) {
        req->pending_bitmap &= (uint8_t)~mask;
        if (atp->ctl & ATP_CONTROL_EOM) {
            uint8_t higher = (uint8_t)(mask - 1u);
            req->pending_bitmap &= higher;
        }
        if (req->callbacks.on_response) {
            atp_response_fragment_t fragment = {.seq = seq,
                                                .duplicate = false,
                                                .eom = (atp->ctl & ATP_CONTROL_EOM) != 0,
                                                .sts = (atp->ctl & ATP_CONTROL_STS) != 0,
                                                .bitmap_remaining = req->pending_bitmap,
                                                .data = atp->data,
                                                .data_len = atp->data_len};
            memcpy(fragment.user, atp->user, sizeof(fragment.user));
            req->callbacks.on_response(&fragment, req->cb_ctx);
        }
    }
    if (atp->ctl & ATP_CONTROL_STS) {
        atp_retry_request(req, false);
        return;
    }
    if (req->pending_bitmap == 0) {
        atp_send_trel(req);
        atp_request_complete(req, ATP_REQUEST_RESULT_OK);
    }
}

static void atp_handle_trel(const ddp_header_t *ddp, const atp_packet_t *atp) {
    int idx = atp_xo_find(atp->tid, ddp->llap.src, ddp->src_socket, ddp->dst_socket);
    LOG(10, "ATP: TRel tid=0x%04X node=%u sock=%u xo_slot=%d", atp->tid, ddp->llap.src, ddp->src_socket, idx);
    if (idx >= 0)
        atp_xo_free(&g_xo_entries[idx]);
}

// Request dispatch ---------------------------------------------------------
static void atp_dispatch_registered_request(const ddp_header_t *ddp, atp_packet_t *atp) {
    atp_handler_slot_t *slot = atp_find_handler_slot(ddp->dst_socket);
    if (!slot) {
        LOG(3, "ATP: unhandled socket %u", (unsigned)ddp->dst_socket);
        return;
    }
    bool xo = (atp->ctl & ATP_CONTROL_XO) != 0;
    if (xo) {
        int existing = atp_xo_find(atp->tid, ddp->llap.src, ddp->src_socket, ddp->dst_socket);
        if (existing >= 0) {
            atp_xo_send_cached(&g_xo_entries[existing], ddp, atp->bitmap);
            return;
        }
        if (atp_xo_alloc(ddp, atp) < 0)
            LOG(2, "ATP: XO cache full, duplicate protection degraded");
    }
    slot->handler.handle_request(ddp, atp, slot->ctx);
}

static void atp_in(const ddp_header_t *ddp, const uint8_t *buf, int len) {
    atp_packet_t atp;
    if (parse_atp(buf, len, &atp) != 0)
        return;
    uint8_t ctl_type = (uint8_t)(atp.ctl & 0xC0);

    if (ctl_type == ATP_CONTROL_TREL) {
        atp_handle_trel(ddp, &atp);
        return;
    }
    if (ctl_type == ATP_CONTROL_TRESP) {
        atp_handle_response(ddp, &atp);
        return;
    }
    if (ctl_type != ATP_CONTROL_TREQ)
        return;

    atp_dispatch_registered_request(ddp, &atp);
}

// =============================== ASP (AppleTalk Session Protocol) ===============================

// ASP Commands / SPFunction (values per docs/appletalk.md)
#define ASP_CLOSE_SESS     1
#define ASP_COMMAND        2
#define ASP_GET_STAT       3
#define ASP_OPEN_SESS      4
#define ASP_TICKLE         5
#define ASP_WRITE          6
#define ASP_WRITE_CONTINUE 7
#define ASP_ATTENTION      8

typedef struct {
    bool in_use;
    uint16_t sess_ref; // 16-bit session reference (internal)
    uint8_t wss; // workstation session socket (from OpenSess request)
    uint8_t sss; // server session socket (returned in reply)
    uint8_t client_node; // LLAP node of the workstation
    uint16_t asp_version; // ASP version from OpenSess
} asp_session_t;

#define MAX_ASP_SESS 4
static asp_session_t g_sessions[MAX_ASP_SESS];
static uint16_t g_next_sess_ref = 0x0021;

static int alloc_session(void) {
    for (int i = 0; i < MAX_ASP_SESS; i++) {
        if (!g_sessions[i].in_use) {
            g_sessions[i].in_use = true;
            g_sessions[i].sess_ref = g_next_sess_ref++;
            g_sessions[i].wss = 0;
            g_sessions[i].sss = HOST_AFP_SOCKET; // default SSS
            g_sessions[i].client_node = 0;
            g_sessions[i].asp_version = 0;
            return g_sessions[i].sess_ref;
        }
    }
    return -1;
}

static void free_session(uint16_t ref) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use && g_sessions[i].sess_ref == ref) {
            g_sessions[i].in_use = false;
            return;
        }
}

static asp_session_t *get_session(uint16_t ref) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use && g_sessions[i].sess_ref == ref)
            return &g_sessions[i];
    return NULL;
}

// Look up session by the 1-byte session ID from ATP UserBytes[1]
static asp_session_t *get_session_by_id(uint8_t session_id) {
    for (int i = 0; i < MAX_ASP_SESS; i++)
        if (g_sessions[i].in_use && (g_sessions[i].sess_ref & 0xFF) == session_id)
            return &g_sessions[i];
    return NULL;
}

// Build an ASP reply payload (to be wrapped inside ATP response)
static int asp_build_reply(uint8_t *out, int out_max, uint8_t func, uint16_t sess_ref, uint16_t req_ref,
                           uint16_t result, const uint8_t *data, int data_len) {
    (void)func; // SPFunction travels in ATP UserBytes in this stack
    if (out_max < 6 + data_len)
        return -1;
    out[0] = (sess_ref >> 8) & 0xFF;
    out[1] = sess_ref & 0xFF;
    out[2] = (req_ref >> 8) & 0xFF;
    out[3] = req_ref & 0xFF;
    out[4] = (result >> 8) & 0xFF;
    out[5] = result & 0xFF;
    if (data_len > 0)
        memcpy(&out[6], data, data_len);
    return 6 + data_len;
}

// ASP SPWrite: pending state for the WriteContinue two-transaction flow
#define ASP_WRITE_QUANTUM (8 * ATP_MAX_ATP_PAYLOAD) // 4624 bytes max per WriteContinue

typedef struct {
    bool in_use;
    // Saved original Write transaction context (for deferred response)
    ddp_header_t orig_ddp;
    atp_packet_t orig_atp;
    uint8_t orig_atp_data[ATP_MAX_ATP_PAYLOAD]; // deep copy of ephemeral ATP data
    // AFP command (opcode separated, params follow)
    uint8_t afp_opcode;
    uint8_t afp_params[ATP_MAX_ATP_PAYLOAD];
    int afp_params_len;
    // Write data accumulated from WriteContinue response
    uint8_t write_buf[ASP_WRITE_QUANTUM];
    int write_len;
} asp_pending_write_t;

static asp_pending_write_t g_pending_write;

// Called for each ATP response fragment from the client's WriteContinueReply
static void asp_wc_on_response(const atp_response_fragment_t *fragment, void *ctx) {
    asp_pending_write_t *pw = (asp_pending_write_t *)ctx;
    if (!pw || !pw->in_use || !fragment || !fragment->data)
        return;
    int avail = ASP_WRITE_QUANTUM - pw->write_len;
    int copy = (fragment->data_len < avail) ? fragment->data_len : avail;
    if (copy > 0) {
        memcpy(pw->write_buf + pw->write_len, fragment->data, (size_t)copy);
        pw->write_len += copy;
    }
}

// Called when WriteContinue transaction completes: process write and reply
static void asp_wc_on_complete(atp_request_handle_t *handle, atp_request_result_t result, void *ctx) {
    (void)handle;
    asp_pending_write_t *pw = (asp_pending_write_t *)ctx;
    if (!pw || !pw->in_use) {
        return;
    }

    uint32_t afp_result;
    uint8_t afp_out[ATP_MAX_ATP_PAYLOAD];
    int afp_len = 0;

    if (result != ATP_REQUEST_RESULT_OK || pw->write_len <= 0) {
        // WriteContinue failed or no data received
        LOG(2, "ASP WriteContinue failed: result=%d write_len=%d", (int)result, pw->write_len);
        afp_result = 0xFFFFEC6Au; // MiscErr
    } else {
        // Build combined buffer: AFP params (11 bytes for FPWrite) + write data
        int combined_len = pw->afp_params_len + pw->write_len;
        uint8_t *combined = (uint8_t *)malloc((size_t)combined_len);
        if (!combined) {
            afp_result = 0xFFFFEC6Au; // MiscErr
        } else {
            memcpy(combined, pw->afp_params, (size_t)pw->afp_params_len);
            memcpy(combined + pw->afp_params_len, pw->write_buf, (size_t)pw->write_len);
            afp_result =
                afp_handle_command(pw->afp_opcode, combined, combined_len, afp_out, (int)sizeof(afp_out), &afp_len);
            free(combined);
        }
    }

    if (afp_result != 0x00000000u)
        afp_len = 0;

    LOG(6, "ASP Write complete: opcode=0x%02X result=0x%08X replyLen=%d dataLen=%d", pw->afp_opcode, afp_result,
        afp_len, pw->write_len);

    // Send deferred response to the original Write ATP transaction
    uint8_t reply_user[4];
    reply_user[0] = (uint8_t)((afp_result >> 24) & 0xFF);
    reply_user[1] = (uint8_t)((afp_result >> 16) & 0xFF);
    reply_user[2] = (uint8_t)((afp_result >> 8) & 0xFF);
    reply_user[3] = (uint8_t)(afp_result & 0xFF);
    atp_responder_send_simple(&pw->orig_ddp, &pw->orig_atp, reply_user, (afp_len > 0) ? afp_out : NULL, afp_len, false);

    pw->in_use = false;
}

static const atp_request_callbacks_t g_asp_wc_callbacks = {
    .on_response = asp_wc_on_response,
    .on_complete = asp_wc_on_complete,
};

// ASP handler: builds replies via ATP responder helpers
static void asp_in(const ddp_header_t *ddp, atp_packet_t *atp, void *ctx) {
    (void)ctx;
    if (!ddp || !atp)
        return;

    uint8_t reply_user[4];
    memcpy(reply_user, atp->user, sizeof(reply_user));

    // SLS GetStatus: no ATP data, SPFunction in UserByte0
    if (atp->data_len == 0 && atp->user[0] == ASP_GET_STAT) {
        LOG(3, "ASP GetStatus: request from node=%u socket=%u", ddp->llap.src, ddp->src_socket);
        const char *server_name = atalk_server_object_name();
        const char *machine_type = "GrannySmith";
        uint8_t *status_block = NULL;
        size_t status_len = 0;
        int sb_rc = atalk_build_status_block(server_name, machine_type, &status_block, &status_len);
        if (sb_rc == 0 && status_block && status_len > 0) {
            reply_user[0] = 0; // zero UserBytes for reply
            int payload_len = (status_len > (size_t)ATP_MAX_ATP_PAYLOAD) ? ATP_MAX_ATP_PAYLOAD : (int)status_len;
            atp_responder_send_simple(ddp, atp, reply_user, status_block, payload_len, false);
        } else {
            reply_user[0] = 0;
            reply_user[1] = reply_user[2] = reply_user[3] = 0;
            atp_responder_send_simple(ddp, atp, reply_user, NULL, 0, false);
        }
        if (status_block)
            free(status_block);
        return;
    }

    // OpenSess handshake on SLS: no ATP payload; parameters in UserBytes
    if (atp->data_len == 0 && atp->user[0] == ASP_OPEN_SESS) {
        uint8_t wss = atp->user[1];
        uint16_t asp_ver = ((uint16_t)atp->user[2] << 8) | (uint16_t)atp->user[3];
        (void)wss;
        (void)asp_ver;
        int new_ref = alloc_session();
        uint8_t sss = HOST_AFP_SOCKET;
        uint8_t session_id = 0;
        uint16_t err = 0x0000;
        if (new_ref < 0) {
            LOG(1, "ASP OpenSess: failed (no free sessions) wss=%u ver=0x%04X", wss, asp_ver);
            err = 0x0001;
            sss = 0;
            session_id = 0;
        } else {
            asp_session_t *s = get_session((uint16_t)new_ref);
            if (s) {
                s->wss = wss;
                s->sss = sss;
                s->client_node = ddp->llap.src;
                s->asp_version = asp_ver;
            }
            session_id = (uint8_t)((uint16_t)new_ref & 0xFF);
            LOG(3, "ASP OpenSess: id=0x%02X sss=%u wss=%u ver=0x%04X", session_id, sss, wss, asp_ver);
        }
        reply_user[0] = sss;
        reply_user[1] = session_id;
        reply_user[2] = (uint8_t)((err >> 8) & 0xFF);
        reply_user[3] = (uint8_t)(err & 0xFF);
        atp_responder_send_simple(ddp, atp, reply_user, NULL, 0, false);
        return;
    }

    // ASP Tickle: one-way keepalive (no response)
    if (atp->user[0] == ASP_TICKLE) {
        LOG(3, "ASP Tickle: received (no response)");
        // TODO: refresh session liveness timer for the indicated SessionID
        return;
    }

    // Extract ASP fields (lenient); SPFunction carried in UserBytes[0]
    uint8_t func = atp->user[0];
    uint16_t sess_ref = (atp->data_len >= 2) ? (uint16_t)((atp->data[0] << 8) | atp->data[1]) : 0;
    uint16_t req_ref = (atp->data_len >= 4) ? (uint16_t)((atp->data[2] << 8) | atp->data[3]) : 0;
    const uint8_t *adata = NULL;
    int adata_len = 0;
    if (atp->data_len >= 6) {
        adata = &atp->data[6];
        adata_len = atp->data_len - 6;
    } else if (atp->data_len > 4) {
        adata = &atp->data[4];
        adata_len = atp->data_len - 4;
    }

    uint8_t asp_reply[ATP_MAX_ATP_PAYLOAD];
    int asp_reply_len = 0;

    switch (func) {
    case ASP_CLOSE_SESS: {
        LOG(3, "ASP CloseSess: sess_ref=0x%04X req_ref=0x%04X", sess_ref, req_ref);
        if (sess_ref != 0) {
            free_session(sess_ref);
        }
        asp_reply_len = asp_build_reply(asp_reply, sizeof(asp_reply), ASP_CLOSE_SESS, sess_ref, req_ref, 0, NULL, 0);
        break;
    }
    case ASP_COMMAND: {
        const uint8_t *cmd = atp->data;
        int cmd_len = atp->data_len;
        uint8_t opcode = (cmd_len > 0) ? cmd[0] : 0;
        LOG(6, "ASP Command: session=0x%02X seq=0x%04X opcode=0x%02X len=%d", atp->user[1],
            (unsigned)((atp->user[2] << 8) | atp->user[3]), opcode, cmd_len);
        // Count consecutive set bits in bitmap to determine max response packets
        int max_packets = 0;
        for (uint8_t bm = atp->bitmap; bm & 1; bm >>= 1)
            max_packets++;
        if (max_packets < 1)
            max_packets = 1;
        if (max_packets > ATP_MAX_RESPONSE_FRAGMENTS)
            max_packets = ATP_MAX_RESPONSE_FRAGMENTS;
        int out_buf_size = max_packets * ATP_MAX_ATP_PAYLOAD;
        uint8_t afp_out[ATP_MAX_RESPONSE_FRAGMENTS * ATP_MAX_ATP_PAYLOAD];
        int afp_len = 0;
        uint32_t afp_result = afp_handle_command(opcode, (cmd_len > 0) ? (cmd + 1) : NULL,
                                                 (cmd_len > 0) ? (cmd_len - 1) : 0, afp_out, out_buf_size, &afp_len);
        LOG(6, "ASP Command result: opcode=0x%02X result=0x%08X replyLen=%d", opcode, afp_result, afp_len);
        reply_user[0] = (uint8_t)((afp_result >> 24) & 0xFF);
        reply_user[1] = (uint8_t)((afp_result >> 16) & 0xFF);
        reply_user[2] = (uint8_t)((afp_result >> 8) & 0xFF);
        reply_user[3] = (uint8_t)(afp_result & 0xFF);
        if (afp_len <= ATP_MAX_ATP_PAYLOAD) {
            atp_responder_send_simple(ddp, atp, reply_user, (afp_len > 0) ? afp_out : NULL, afp_len, false);
        } else {
            // Split reply across multiple ATP response packets
            int num_packets = (afp_len + ATP_MAX_ATP_PAYLOAD - 1) / ATP_MAX_ATP_PAYLOAD;
            if (num_packets > max_packets)
                num_packets = max_packets;
            atp_response_packet_desc_t descs[ATP_MAX_RESPONSE_FRAGMENTS];
            int offset = 0;
            for (int i = 0; i < num_packets; i++) {
                int chunk = afp_len - offset;
                if (chunk > ATP_MAX_ATP_PAYLOAD)
                    chunk = ATP_MAX_ATP_PAYLOAD;
                descs[i].payload = afp_out + offset;
                descs[i].payload_len = chunk;
                descs[i].user = reply_user;
                descs[i].sts = false;
                descs[i].eom = (i == num_packets - 1);
                offset += chunk;
            }
            atp_responder_send_packets(ddp, atp, descs, (size_t)num_packets);
        }
        return;
    }
    case ASP_GET_STAT: {
        uint8_t opcode = (adata_len > 0) ? adata[0] : 0;
        uint8_t afp_out[ATP_MAX_ATP_PAYLOAD];
        int afp_len = 0;
        uint32_t afp_result =
            afp_handle_command(opcode, (adata_len > 0) ? (adata + 1) : NULL, (adata_len > 0) ? (adata_len - 1) : 0,
                               afp_out, sizeof(afp_out), &afp_len);
        uint16_t asp_result = (uint16_t)(afp_result & 0xFFFFu);
        if (afp_result != 0x00000000u) {
            afp_len = 0;
        }
        asp_reply_len = asp_build_reply(asp_reply, sizeof(asp_reply), ASP_GET_STAT, sess_ref, req_ref, asp_result,
                                        (afp_len > 0 && afp_result == 0x00000000u) ? afp_out : NULL,
                                        (afp_result == 0x00000000u) ? afp_len : 0);
        break;
    }
    case ASP_WRITE: {
        // ASP SPWrite: two-transaction protocol via WriteContinue
        const uint8_t *cmd = atp->data;
        int cmd_len = atp->data_len;
        uint8_t opcode = (cmd_len > 0) ? cmd[0] : 0;
        uint8_t session_id = atp->user[1];
        LOG(6, "ASP Write: session=0x%02X seq=0x%04X opcode=0x%02X len=%d", session_id,
            (unsigned)((atp->user[2] << 8) | atp->user[3]), opcode, cmd_len);

        // Look up session for client addressing
        asp_session_t *sess = get_session_by_id(session_id);
        if (!sess) {
            LOG(2, "ASP Write: unknown session 0x%02X", session_id);
            uint32_t err = 0xFFFFEC62u; // SessClosed
            reply_user[0] = (uint8_t)((err >> 24) & 0xFF);
            reply_user[1] = (uint8_t)((err >> 16) & 0xFF);
            reply_user[2] = (uint8_t)((err >> 8) & 0xFF);
            reply_user[3] = (uint8_t)(err & 0xFF);
            atp_responder_send_simple(ddp, atp, reply_user, NULL, 0, false);
            return;
        }

        if (g_pending_write.in_use) {
            // Already processing a write; ignore duplicate (XO handles retransmit)
            LOG(6, "ASP Write: already pending, ignoring");
            return;
        }

        // Save context for deferred response
        asp_pending_write_t *pw = &g_pending_write;
        memset(pw, 0, sizeof(*pw));
        pw->in_use = true;
        pw->orig_ddp = *ddp;
        pw->orig_atp = *atp;
        // Deep-copy ATP data (ephemeral pointer into receive buffer)
        if (atp->data && atp->data_len > 0) {
            int dlen =
                (atp->data_len > (int)sizeof(pw->orig_atp_data)) ? (int)sizeof(pw->orig_atp_data) : atp->data_len;
            memcpy(pw->orig_atp_data, atp->data, (size_t)dlen);
            pw->orig_atp.data = pw->orig_atp_data;
            pw->orig_atp.data_len = dlen;
        }
        pw->afp_opcode = opcode;
        if (cmd_len > 1) {
            pw->afp_params_len = cmd_len - 1;
            if (pw->afp_params_len > (int)sizeof(pw->afp_params))
                pw->afp_params_len = (int)sizeof(pw->afp_params);
            memcpy(pw->afp_params, cmd + 1, (size_t)pw->afp_params_len);
        }

        // Send WriteContinue ATP request to the client's WSS
        uint16_t buf_size = ASP_WRITE_QUANTUM;
        uint8_t wc_data[2];
        wc_data[0] = (uint8_t)((buf_size >> 8) & 0xFF);
        wc_data[1] = (uint8_t)(buf_size & 0xFF);

        atp_request_params_t wc_params = {
            .dest = {.net = 0, .node = sess->client_node, .socket = sess->wss},
            .src_socket = HOST_AFP_SOCKET,
            .bitmap = 0xFF, // request up to 8 response packets
            .mode = ATP_TRANSACTION_XO,
            .trel_timer_hint = 0,
            .user = {ASP_WRITE_CONTINUE, session_id, atp->user[2], atp->user[3]},
            .payload = wc_data,
            .payload_len = 2,
            .retry_timeout_ms = 5000,
            .retry_limit = 10,
        };

        LOG(6, "ASP WriteContinue: node=%u wss=%u bufSize=%u", sess->client_node, sess->wss, buf_size);
        atp_request_handle_t *wc_handle = atp_request_submit(&wc_params, &g_asp_wc_callbacks, pw);
        if (!wc_handle) {
            LOG(2, "ASP WriteContinue: failed to submit ATP request");
            pw->in_use = false;
            uint32_t err = 0xFFFFEC6Au; // MiscErr
            reply_user[0] = (uint8_t)((err >> 24) & 0xFF);
            reply_user[1] = (uint8_t)((err >> 16) & 0xFF);
            reply_user[2] = (uint8_t)((err >> 8) & 0xFF);
            reply_user[3] = (uint8_t)(err & 0xFF);
            atp_responder_send_simple(ddp, atp, reply_user, NULL, 0, false);
        }
        return; // response deferred until WriteContinue completes
    }
    default: {
        LOG(3, "ASP unknown func=0x%02X sess=0x%04X req=0x%04X dataLen=%d", func, sess_ref, req_ref, atp->data_len);
        asp_reply_len =
            asp_build_reply(asp_reply, sizeof(asp_reply), (func == 0xFF) ? 0 : func, sess_ref, req_ref, 0, NULL, 0);
        break;
    }
    }

    if (asp_reply_len < 0)
        asp_reply_len = 0;
    atp_responder_send_simple(ddp, atp, reply_user, asp_reply_len > 0 ? asp_reply : NULL, asp_reply_len, false);
}

// Build an ASP reply payload (to be wrapped inside ATP response)
// Note: In this stack, SPFunction travels in ATP UserBytes[0]; the ASP bytes
// begin with SessionRefNum, ReqRefNum, CmdResult, followed by payload.

// AFP implementation moved to server.c
// Common wire helpers (big-endian readers/writers)

static uint16_t rd16be(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
