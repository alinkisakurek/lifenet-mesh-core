#pragma once
#include <stdint.h>
#include "mesh_packet.h"

enum ControlActionType {
    CTRL_DROP,
    CTRL_FORWARD_BROADCAST,
    CTRL_FORWARD_UNICAST,
    CTRL_GENERATE_RREP,
    CTRL_DELIVER_TO_APP
};

struct ControlAction {
    ControlActionType type;
    uint16_t next_hop;  // valid if type is FORWARD_UNICAST or GENERATE_RREP
};

enum RouteState {
    ROUTE_VALID,
    ROUTE_INVALID,
    ROUTE_REPAIRING
};

struct RouteEntry {
    uint16_t destination;
    uint16_t next_hop;
    uint8_t hop_count;
    uint16_t seq_num;
    uint32_t lifetime; // absolute expiration timestamp in ms
    int8_t link_rssi;
    RouteState state;
    bool in_use;
};

#define ROUTING_TABLE_SIZE 32

void routing_init();
bool routing_lookup(uint16_t dst, RouteEntry& out, uint32_t now_ms);
bool routing_add_or_update(uint16_t dst, uint16_t next_hop, uint8_t hop_count, uint16_t seq_num, uint32_t lifetime_ms, int8_t link_rssi, RouteState state);
void routing_invalidate(uint16_t dst);
void routing_purge_expired(uint32_t now_ms);
void routing_debug_dump(uint32_t now_ms);

// Duplicate Detection
struct DupEntry {
    uint32_t msg_id;
    uint16_t src_addr;
    bool in_use;
};

#define DUP_BUFFER_SIZE 32

void dupdet_init();
bool dupdet_is_duplicate(uint32_t msg_id, uint16_t src_addr);

// AODV Control Handlers
ControlAction handle_rreq(Packet& p, uint16_t local_addr, uint32_t now_ms);
ControlAction handle_rrep(Packet& p, uint16_t local_addr, uint32_t now_ms);
ControlAction handle_rerr(Packet& p, uint16_t local_addr, uint32_t now_ms);
