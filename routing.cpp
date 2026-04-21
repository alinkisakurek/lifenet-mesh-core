#include "routing.h"
#include "debug.h"

static RouteEntry routing_table[ROUTING_TABLE_SIZE];

void routing_init() {
    for (int i = 0; i < ROUTING_TABLE_SIZE; ++i) {
        routing_table[i].in_use = false;
    }
}

void routing_purge_expired(uint32_t now_ms) {
    for (int i = 0; i < ROUTING_TABLE_SIZE; ++i) {
        if (routing_table[i].in_use) {
            // Check if absolute lifetime has passed.
            // Using signed subtraction cleanly handles the 49-day millis() wraparound.
            if ((int32_t)(now_ms - routing_table[i].lifetime) >= 0) {
                routing_table[i].in_use = false;
            }
        }
    }
}

bool routing_lookup(uint16_t dst, RouteEntry& out, uint32_t now_ms) {
    routing_purge_expired(now_ms); // Ensure we don't return expired routes
    
    for (int i = 0; i < ROUTING_TABLE_SIZE; ++i) {
        if (routing_table[i].in_use && routing_table[i].destination == dst) {
            if (routing_table[i].state == ROUTE_VALID) {
                out = routing_table[i];
                return true;
            }
        }
    }
    return false;
}

bool routing_add_or_update(uint16_t dst, uint16_t next_hop, uint8_t hop_count, uint16_t seq_num, uint32_t lifetime_ms, int8_t link_rssi, RouteState state) {
    int update_idx = -1;
    int free_idx = -1;
    int invalid_idx = -1;

    // Scan table to find matching destination, a free slot, or an invalid slot we can evict
    for (int i = 0; i < ROUTING_TABLE_SIZE; ++i) {
        if (routing_table[i].in_use) {
            if (routing_table[i].destination == dst) {
                update_idx = i;
                break; // Found the exact route to update
            } else if (routing_table[i].state == ROUTE_INVALID && invalid_idx == -1) {
                invalid_idx = i; // Keep track of the first invalid entry just in case
            }
        } else if (free_idx == -1) {
            free_idx = i; // First completely free slot
        }
    }

    int target_idx = -1;
    if (update_idx != -1) {
        // Overwrite existing route for this destination
        target_idx = update_idx;
    } else if (free_idx != -1) {
        // Take a free slot
        target_idx = free_idx;
    } else if (invalid_idx != -1) {
        // Table is full, evict an invalid route
        target_idx = invalid_idx;
    } else {
        // Table is completely full with valid, different routes. Cannot add.
        return false; 
    }

    routing_table[target_idx].destination = dst;
    routing_table[target_idx].next_hop = next_hop;
    routing_table[target_idx].hop_count = hop_count;
    routing_table[target_idx].seq_num = seq_num;
    routing_table[target_idx].lifetime = lifetime_ms; // Expected to be absolute expiration time
    routing_table[target_idx].link_rssi = link_rssi;
    routing_table[target_idx].state = state;
    routing_table[target_idx].in_use = true;

    return true;
}

void routing_invalidate(uint16_t dst) {
    for (int i = 0; i < ROUTING_TABLE_SIZE; ++i) {
        if (routing_table[i].in_use && routing_table[i].destination == dst) {
            routing_table[i].state = ROUTE_INVALID;
            break;
        }
    }
}

// ==========================================
// Duplicate Detection Implementation
// ==========================================

static DupEntry dup_buffer[DUP_BUFFER_SIZE];
static int dup_head = 0; // Circular buffer insertion index

void dupdet_init() {
    for (int i = 0; i < DUP_BUFFER_SIZE; ++i) {
        dup_buffer[i].in_use = false;
    }
    dup_head = 0;
}

bool dupdet_is_duplicate(uint32_t msg_id, uint16_t src_addr) {
    // 1. Check if the message was already seen
    for (int i = 0; i < DUP_BUFFER_SIZE; ++i) {
        if (dup_buffer[i].in_use && dup_buffer[i].msg_id == msg_id && dup_buffer[i].src_addr == src_addr) {
            return true; // Duplicate!
        }
    }

    // 2. Not seen. Record it at the current head index.
    dup_buffer[dup_head].msg_id = msg_id;
    dup_buffer[dup_head].src_addr = src_addr;
    dup_buffer[dup_head].in_use = true;

    // 3. Advance the head circularly
    dup_head = (dup_head + 1) % DUP_BUFFER_SIZE;

    return false; // Was not a duplicate
}

// ==========================================
// AODV Control Message Handlers
// ==========================================

#define ROUTE_LIFETIME_MS 120000

ControlAction handle_rreq(Packet& p, uint16_t local_addr, uint32_t now_ms) {
    ControlAction action;
    action.type = CTRL_DROP;
    action.next_hop = 0;

    if (dupdet_is_duplicate(p.msg_id, p.src_addr)) {
        return action; // Drop duplicate
    }

    // Insert or update reverse route for SRC_ADDR
    uint8_t new_hop_count = p.hop_count + 1;
    routing_add_or_update(p.src_addr, p.prev_hop, new_hop_count, p.seq_num, now_ms + ROUTE_LIFETIME_MS, 0, ROUTE_VALID);

    if (p.dst_addr == local_addr) {
        // We are the destination, generate RREP
        action.type = CTRL_GENERATE_RREP;
        action.next_hop = p.prev_hop;
        return action;
    }

    RouteEntry route;
    if (routing_lookup(p.dst_addr, route, now_ms)) {
        // We have a valid route to the destination, generate intermediate RREP
        action.type = CTRL_GENERATE_RREP;
        action.next_hop = p.prev_hop;
        return action;
    }

    // No route, we must rebroadcast
    if (p.ttl <= 1) {
        return action; // Drop, TTL expired
    }

    p.ttl -= 1;
    p.prev_hop = local_addr;
    p.hop_count = new_hop_count; // Increment hop count in the packet too

    action.type = CTRL_FORWARD_BROADCAST;
    return action;
}

ControlAction handle_rrep(Packet& p, uint16_t local_addr, uint32_t now_ms) {
    ControlAction action;
    action.type = CTRL_DROP;
    action.next_hop = 0;

    if (dupdet_is_duplicate(p.msg_id, p.src_addr)) {
        return action; // Drop duplicate
    }

    // Install/update forward route to the RREP source (the original RREQ destination)
    uint8_t new_hop_count = p.hop_count + 1;
    routing_add_or_update(p.src_addr, p.prev_hop, new_hop_count, p.seq_num, now_ms + ROUTE_LIFETIME_MS, 0, ROUTE_VALID);

    if (p.dst_addr == local_addr) {
        // We are the originator of the RREQ. We consume this RREP.
        // The route is now established, the app can send queued DATA.
        return action; 
    }

    // We are an intermediate node, forward the RREP to the original RREQ source
    RouteEntry reverse_route;
    if (routing_lookup(p.dst_addr, reverse_route, now_ms)) {
        if (p.ttl <= 1) {
            return action; // TTL expired
        }
        p.ttl -= 1;
        p.prev_hop = local_addr;
        p.hop_count = new_hop_count;

        action.type = CTRL_FORWARD_UNICAST;
        action.next_hop = reverse_route.next_hop;
        return action;
    }

    return action; // No route to forward RREP, drop
}

ControlAction handle_rerr(Packet& p, uint16_t local_addr, uint32_t now_ms) {
    ControlAction action;
    action.type = CTRL_DROP;
    action.next_hop = 0;

    if (dupdet_is_duplicate(p.msg_id, p.src_addr)) {
        return action;
    }

    // For V1 simplicity, assume p.src_addr represents the node that became unreachable,
    // or the node emitting the RERR. We invalidate the route to p.src_addr.
    routing_invalidate(p.src_addr);

    // If there's a specific unreachable destination in the payload, we'd invalidate that too.
    if (p.payload_len >= 2) {
        uint16_t unreachable_dst = p.payload[0] | (p.payload[1] << 8);
        routing_invalidate(unreachable_dst);
    }

    if (p.dst_addr == local_addr) {
        // RERR reached its destination, consume
        return action;
    }

    // Forward the RERR upstream
    if (p.dst_addr == 0xFFFF) {
        // Broadcast RERR
        if (p.ttl > 1) {
            p.ttl -= 1;
            p.prev_hop = local_addr;
            action.type = CTRL_FORWARD_BROADCAST;
        }
    } else {
        // Unicast RERR
        RouteEntry route;
        if (routing_lookup(p.dst_addr, route, now_ms)) {
            if (p.ttl > 1) {
                p.ttl -= 1;
                p.prev_hop = local_addr;
                action.type = CTRL_FORWARD_UNICAST;
                action.next_hop = route.next_hop;
            }
        }
    }

    return action;
}

void routing_debug_dump(uint32_t now_ms) {
#if DEBUG_ENABLED
    routing_purge_expired(now_ms);
    DBG_PRINTLN("=== Routing Table ===");
    for (int i = 0; i < ROUTING_TABLE_SIZE; ++i) {
        if (routing_table[i].in_use) {
            uint32_t remaining = routing_table[i].lifetime - now_ms;
            DBG_PRINTF("dst: 0x%04X | next: 0x%04X | hop: %d | state: %d | rem: %u ms\n", 
                       routing_table[i].destination, routing_table[i].next_hop, 
                       routing_table[i].hop_count, routing_table[i].state, remaining);
        }
    }
#endif
}
