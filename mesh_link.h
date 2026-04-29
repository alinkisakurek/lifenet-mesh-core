#pragma once
#include <stdint.h>
#include "mesh_packet.h"

bool mesh_send_unicast(Packet& p, uint16_t local_addr, uint32_t now_ms);
bool mesh_retry_queued_packet(Packet& p, uint16_t local_addr, uint32_t now_ms);
bool mesh_handle_incoming(Packet& p, uint16_t local_addr, uint32_t now_ms);
