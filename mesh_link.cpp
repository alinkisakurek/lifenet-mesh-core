#include "mesh_link.h"
#include "lora_link.h"
#include "routing.h"
#include "store_forward.h"
#include <Arduino.h>

#define ACK_TIMEOUT_MS 2000
#define MAX_TX_RETRIES 3

#define PACKET_TYPE_DATA 0
#define PACKET_TYPE_ACK 6

bool mesh_send_unicast(Packet& p, uint16_t local_addr, uint32_t now_ms) {
    RouteEntry route;
    if (!routing_lookup(p.dst_addr, route, now_ms)) {
        sf_enqueue(p, now_ms);
        return false; // No route exists
    }

    p.prev_hop = local_addr;
    p.next_hop = route.next_hop;

    for (int attempt = 0; attempt <= MAX_TX_RETRIES; ++attempt) {
        if (!lora_send_packet(p)) {
            continue; // Hardware/UART send failed locally, try again
        }

        // Wait for an ACK
        Packet rx;
        if (lora_receive_packet(rx, ACK_TIMEOUT_MS)) {
            // Validate the ACK
            if (rx.type == PACKET_TYPE_ACK &&
                rx.msg_id == p.msg_id &&
                rx.src_addr == route.next_hop &&
                rx.dst_addr == local_addr) {
                return true; // Successfully forwarded!
            }
        }
        // If lora_receive_packet times out, or the received packet wasn't our ACK,
        // the loop continues and we retry the transmission.
    }

    // All retries failed
    routing_invalidate(p.dst_addr);
    sf_enqueue(p, now_ms); // Best-effort enqueue for later
    
    return false;
}

bool mesh_handle_incoming(Packet& p, uint16_t local_addr, uint32_t now_ms) {
    if (p.dst_addr == local_addr) {
        if (p.type == PACKET_TYPE_DATA) {
            // Generate and send an ACK back to the prev_hop that sent this to us
            Packet ack;
            packet_init(ack);
            ack.msg_id = p.msg_id;
            ack.src_addr = local_addr;
            ack.dst_addr = p.prev_hop;
            ack.prev_hop = local_addr;
            ack.next_hop = p.prev_hop;
            ack.type = PACKET_TYPE_ACK;
            ack.ttl = 1;
            ack.hop_count = 0;
            ack.payload_len = 0;
            
            lora_send_packet(ack);
            return true;
        } else if (p.type == PACKET_TYPE_ACK) {
            // Consume the ACK if it wasn't intercepted by a waiting mesh_send_unicast
            // (e.g. if it arrived slightly after timeout).
            return true;
        }
    }
    
    // Not addressed to us, or routing/forwarding integration will come later.
    return false;
}
