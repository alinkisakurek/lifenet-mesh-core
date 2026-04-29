# Life Net LoRa Mesh – Product Requirements Document (PRD)

---

## 0. How to Use This PRD (for AI Code Tools)

This document defines the target behaviour of the LoRa mesh firmware for the Life Net project.

Code **SHOULD** be generated in small, testable modules, in roughly this order:

1. Low-level LoRa driver & Packet serialization/deserialization
2. Routing table & AODV control message handling (RREQ / RREP / RERR)
3. Store-and-forward queue
4. Retry & ACK mechanism
5. BLE ↔ LoRa bridge on ESP32
6. Gateway node: WiFi/MQTT uplink

> **Do NOT** change the packet format or add/remove fields in the header.
>
> Prefer readability over micro-optimizations; manual tuning will be done later.
>
> Use **C++** for ESP32 with the **Arduino framework** and **FreeRTOS tasks** as the main concurrency model.
>
> All nodes are built on the same hardware platform and share the same pin mapping and radio configuration.

---

## 1. Hardware and Pin Structure

In this version, the network uses **3 identical nodes**. Each node consists of:

- Fixaj 3-in-1 ESP PCB
- ESP32 DevKit (30-pin ESP32-WROOM module)
- LoRa E22 SX1262 E900T22D module
- Appropriate 868/900 MHz antenna

All nodes share the same hardware and pin configuration.

### 1.1 Core Platform

| Item | Details |
|---|---|
| MCU | ESP32-WROOM-32 (30-pin DevKit) |
| Power input | 5 V via USB or external supply |
| Logic voltage | 3.3 V (on-board regulator) |
| Programming | USB-UART interface on Fixaj 3-in-1 PCB |

### 1.2 LoRa E22 SX1262 Pin Mapping

The E22 SX1262 module is connected to ESP32 **UART1** (`HardwareSerial(1)`):

| Signal | Direction | ESP32 GPIO |
|---|---|---|
| E22_TX (module TX → ESP32 RX) | Input | GPIO27 (UART1 RX) |
| E22_RX (module RX ← ESP32 TX) | Output | GPIO35 (UART1 TX – input-only pin on DevKit; TX driven internally by UART1) |
| E22_M0 | Output | GPIO32 |
| E22_M1 | Output | GPIO33 |

**V1 default mode configuration:**

| M0 | M1 | Mode |
|---|---|---|
| LOW | LOW | Normal data mode |

**UART parameters (E22 UART link):**

| Parameter | Value |
|---|---|
| Port | Serial1 (HardwareSerial(1)) |
| Baud rate | 9600 bps |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |

### 1.3 ESP32 GPIO Usage Summary (Per Node)

| GPIO | Function |
|---|---|
| GPIO27 | LoRa E22 TX (ESP32 RX1) |
| GPIO35 | LoRa E22 RX (ESP32 TX1, input-only pin on DevKit) |
| GPIO32 | LoRa E22 M0 (mode select) |
| GPIO33 | LoRa E22 M1 (mode select) |
| Others | Reserved for BLE, buttons, sensors, and future expansion (not used in V1) |

> **Note:** The exact pin mapping follows the Fixaj 3-in-1 ESP PCB reference design. If the hardware revision changes, this table **MUST** be updated accordingly.

---

## 2. Mesh Network Structure

### 2.1 Network Topology

The network **SHALL** follow a **multi-hop (hop-to-hop)** communication model.

There are two operational node roles in V1:

**Relay Nodes**
- Receive and forward packets between nodes
- Perform routing and store-and-forward

**Gateway Node**
- Final destination for uplink traffic
- Connected to the internet via WiFi (and optionally MQTT)

**Communication flow (conceptual):**

```
End node (mobile / BLE-attached) → Relay Node(s) → Gateway Node
```

Nodes **MUST NOT** require direct communication with the gateway. They rely on intermediate nodes to relay packets.

Each node **MUST**:
- Receive LoRa packets
- Decide whether to forward them based on routing logic
- Forward packets if necessary toward the gateway

---

## 3. Packet Structure

All mesh traffic uses a common packet structure with a **fixed-size header** and **fixed payload buffer**.

```c
typedef struct __attribute__((packed)) {
    uint32_t msg_id;       // Unique message ID for duplicate detection

    uint16_t src_addr;     // Original source node
    uint16_t dst_addr;     // Final destination node

    uint16_t prev_hop;     // Last forwarding node
    uint16_t next_hop;     // Next intended hop (0xFFFF = broadcast)

    uint8_t  ttl;          // Decrement at each hop; drop at zero
    uint8_t  hop_count;    // Increment at each hop (for diagnostics / metrics)

    uint16_t seq_num;      // Per-source packet sequence number
    uint8_t  type;         // DATA / RREQ / RREP / RERR / BEACON / ACK

    uint8_t  ai_priority;  // 0=LOW, 1=NORMAL, 2=HIGH, 3=CRITICAL
    uint8_t  payload_len;  // Actual payload length in bytes
    uint8_t  payload[64];  // Application payload buffer (V1 fixed size)

    uint16_t crc;          // CRC16 over header + payload_len bytes (excluding crc field)
} Packet;
```

### Field Semantics

| Field | Description |
|---|---|
| `msg_id` | Unique message identifier for duplicate suppression across the mesh. |
| `src_addr` | Original source node address. |
| `dst_addr` | Final destination node address. |
| `prev_hop` | Last node that forwarded the packet. |
| `next_hop` | Intended next hop; `0xFFFF` indicates broadcast (all neighbours). |
| `ttl` | Decremented at each hop; when it reaches zero, the packet **MUST** be dropped. |
| `hop_count` | Incremented at each hop; used for diagnostics and optional route metrics. |
| `seq_num` | Per-source packet sequence number (not full RFC 3561 destination sequence). |
| `type` | Packet type: `DATA`, `RREQ`, `RREP`, `RERR`, `BEACON`, `ACK`. |
| `ai_priority` | Application-level importance tag (0=LOW, 1=NORMAL, 2=HIGH, 3=CRITICAL). Carried end-to-end; consumed at gateway/cloud side. Does **not** affect routing, queue ordering, or retransmission in V1. |
| `payload_len` | Valid payload length in bytes (0–64). |
| `payload[64]` | Application data buffer (V1 uses a fixed maximum). |
| `crc` | CRC16 over the serialized header and payload (excluding the CRC field itself). |

> **Implementation note:** Packets **SHALL** be serialized/deserialized explicitly (byte-by-byte) rather than relying on raw struct casting, to avoid padding and endianness issues on different toolchains.

---

## 4. Node Types and Roles

### Relay Node

- Forwards DATA and control packets according to routing rules.
- Participates fully in AODV-based route discovery.
- Maintains routing table and store-and-forward queue.

### Gateway Node

- Same LoRa mesh behaviour as a Relay Node.
- Additionally connects to WiFi and forwards application messages (with `ai_priority`) to a cloud/backend (e.g., MQTT).
- Has a fixed address `GATEWAY_ADDR` in V1.

---

## 5. Routing Layer

### 5.1 Protocol Choice

The mesh layer **SHALL** implement a reactive, on-demand routing protocol based on **AODV** (Ad-hoc On-Demand Distance Vector, RFC 3561).

- Routes are **not** maintained proactively.
- A route to a given destination is discovered only when a node has data to send and does not already have a valid entry in its routing table.

**Rationale:**
- LoRa has very limited bandwidth; proactive protocols generate too much periodic overhead.
- Topology is expected to be mostly static but must tolerate node failures → reactive + self-healing fits well.

### 5.2 Control Packet Types

The following AODV control messages **SHALL** be supported, encoded via the `type` field:

| Type | Description |
|---|---|
| `RREQ` | Route Request (broadcast, route discovery) |
| `RREP` | Route Reply (unicast, route confirmation) |
| `RERR` | Route Error (unicast/broadcast, link failure notification) |
| `DATA` | Application payload |
| `BEACON` | Optional periodic presence announcement (V2) |
| `ACK` | Simple per-hop acknowledgement for reliability |

### 5.3 Route Discovery (RREQ Flooding)

Routes are discovered on-demand by broadcasting RREQ packets across the mesh. Each node that receives an RREQ and does not have a valid route to the destination rebroadcasts it (with TTL decremented and hop_count incremented) until the RREQ reaches the destination or a node with a valid route.

### 5.4 Route Reply (RREP)

When the destination node (or an intermediate node with a valid route) receives an RREQ, it responds with a unicast RREP toward the originator, establishing the forward route hop-by-hop.

### 5.5 Route Error (RERR)

When a node detects a broken link (e.g., repeated TX failure), it generates a RERR to notify upstream nodes that the route is no longer valid. Affected routing table entries are marked INVALID.

### 5.6 Routing Table Schema

```c
destination : uint16
next_hop    : uint16
hop_count   : uint8
seq_num     : uint16   // last known sequence number for destination (simplified)
lifetime    : uint32   // expiration timestamp, ms
link_rssi   : int8     // optional, V2
state       : { VALID, INVALID, REPAIRING }
```

- **Table size:** minimum 32 entries.
- Entries older than `ROUTE_LIFETIME` (default: **120 s**) **SHALL** be purged.

### 5.7 Loop Prevention and Storm Control

- Every forwarded packet **SHALL** decrement `ttl`; packets with `ttl == 0` are dropped.
- Every node **SHALL** keep a **duplicate buffer** of the last 32 `(MSG_ID, SRC_ADDR)` tuples and silently drop repeats.
- RREQ rebroadcasts **SHALL** be delayed by a small random jitter (**10–100 ms**) to reduce LoRa airtime collisions.

### 5.8 Out of Scope (V1) / Future Work (V2)

- Proactive routing protocols (OLSR, DSDV)
- Multi-path routing
- Link-quality-based route selection (RSSI-aware routing)
- `BEACON`-based neighbour discovery
- Full RFC 3561 destination sequence numbers

### 5.9 Store-and-Forward Queue

The store-and-forward (SF) subsystem allows intermediate and source nodes to buffer DATA packets when a route is temporarily unavailable, and retry delivery once a route is established.

#### 5.9.1 Queue Structure

- Fixed-size circular buffer (minimum 8 entries).
- Each entry holds a complete `Packet` copy plus metadata: enqueue timestamp, retry count, next retry time.

#### 5.9.2 Enqueue Conditions

A DATA packet is enqueued when `mesh_send_unicast` fails (no valid route or repeated ACK timeout).

#### 5.9.3 Retry Policy

- Maximum retries: configurable (`SF_MAX_RETRIES`, default 5).
- Retry interval: exponential backoff starting at `SF_RETRY_BASE_MS` (default 5 s), doubling each attempt.
- Packets exceeding max retries are dropped and a `RERR` MAY be generated.

#### 5.9.4 Queue Processing

A background task (`sf_process`) periodically scans the queue and attempts to resend eligible packets using `mesh_send_unicast`.

#### 5.9.5 Eviction Policy

When the queue is full, the oldest (lowest-priority) entry SHALL be evicted to make room for a new packet.

#### 5.9.6 Persistence

In V1, the queue is **RAM-only** (no flash persistence). Reboot clears all queued packets.

> **Important note:** V1 store-forward semantics must be preserved: queued DATA packets (whether originally sent by this node or forwarded on behalf of others) are retried via `sf_process` using the same `mesh_send_unicast` callback, **without changing packet format, `msg_id` or addressing**.

### 5.10 Retry & ACK Mechanism (Simple)

- Each unicast transmission waits for a per-hop `ACK` within `ACK_TIMEOUT_MS` (default: 500 ms).
- If no ACK is received, the packet is retransmitted up to `MAX_RETRIES` times (default: 3).
- After exhausting retries, the link is considered broken → trigger `RERR` and invalidate the route.
- ACKs are strictly 1-hop and are **NOT** forwarded beyond the immediate sender/receiver pair.

### 5.11 Duplicate Detection

- Each node maintains a rolling duplicate buffer of the last 32 `(msg_id, src_addr)` pairs.
- On receiving any packet, the buffer is checked **before** any processing or forwarding.
- Duplicates are **silently dropped** (no ACK, no forwarding).
- The buffer uses a circular/FIFO eviction policy.

### 5.12 Implementation Tasks (for AI tools)

1. Implement `Packet` serialization/deserialization (byte-by-byte, no struct casting).
2. Implement CRC16 computation and verification.
3. Implement the routing table with `routing_lookup`, `routing_update`, and `routing_purge`.
4. Implement RREQ flooding with jitter and duplicate suppression.
5. Implement RREP unicast path back to originator.
6. Implement RERR generation and route invalidation.
7. Implement the store-and-forward queue with exponential backoff.
8. Implement the retry/ACK loop inside `mesh_send_unicast`.
9. Implement duplicate detection buffer.
10. Implement multi-hop DATA forwarding as described in §5.13.

### 5.13 Multi-hop DATA Forwarding (V1.1)

To support true multi-hop delivery for application payloads, intermediate nodes **SHALL** forward DATA packets when certain conditions are met.

#### 5.13.1 Forwarding Conditions

When a node receives a packet with `type == DATA` and `dst_addr != local_addr`, it **SHALL** behave as follows:

**Step 1 – Duplicate check**

```
Call dupdet_is_duplicate(msg_id, src_addr).
If true → silently drop (no forwarding, no ACK).
```

**Step 2 – TTL and hop_count update**

```
If ttl <= 1 → drop the packet (do not forward, do not enqueue).
Otherwise:
    ttl        -= 1
    hop_count  += 1
```

**Step 3 – Route lookup**

```
Call routing_lookup(dst_addr, route, now_ms).
If no valid route exists → return without forwarding
    (optionally trigger RREQ – implementation detail, can be V2).
```

**Step 4 – Forward via unicast API**

```
If valid route exists and ttl > 0:
    Call mesh_send_unicast(packet, local_addr, now_ms)
```

When forwarding, the implementation **MUST**:
- Preserve `msg_id`, `src_addr`, and `dst_addr` exactly as received (no re-randomizing).
- Allow `mesh_send_unicast` to update `prev_hop` and `next_hop` according to the routing table.
- On successful forwarding (ACK received), consider the packet forwarded; **no** local delivery is performed.

**Interaction with store-and-forward:**

If `mesh_send_unicast` returns failure, the existing logic **MAY** enqueue the DATA packet into the store-and-forward queue — even if the node is only an intermediate relay. This ensures best-effort delivery for transit traffic as well as locally-originated traffic.

#### 5.13.2 Non-forwarding Cases

| Condition | Behaviour |
|---|---|
| `type == DATA` and `dst_addr == local_addr` | Deliver to local application (if any) and generate an ACK back to `prev_hop`. |
| `type == ACK` | ACKs remain strictly 1-hop and are **NOT** forwarded. |

#### 5.13.3 API Usage Requirements

Multi-hop DATA forwarding **MUST** reuse the existing APIs and data structures:

| API | Purpose |
|---|---|
| `routing_lookup(uint16_t dst, RouteEntry& out, uint32_t now_ms)` | Next-hop resolution |
| `mesh_send_unicast(Packet& p, uint16_t local_addr, uint32_t now_ms)` | Per-hop sending with ACK and retries |
| `dupdet_is_duplicate(uint32_t msg_id, uint16_t src_addr)` | Loop prevention |

> **No changes** to the `Packet` struct layout or serialization format are allowed.

---

## 6. Assumptions Relevant to Routing

- Exactly **one gateway** exists in V1, with a compile-time constant address `GATEWAY_ADDR` (default `0x0004`). Non-gateway nodes **SHALL** treat `GATEWAY_ADDR` as the default `dst_addr` for uplink traffic.
- All nodes share the same LoRa radio parameters (frequency, SF, BW, CR, sync word) so that every transmission is in principle receivable by every in-range neighbour.
- Addresses are **16-bit** and unique per node, configured at flash time.
- The network size in V1 is small (**3–10 nodes**), so simple flooding + TTL + duplicate buffer is acceptable.

## 7. Power Strategy (V1)

- In V1, all nodes are assumed to be mains-powered or otherwise energy-unconstrained.
- All nodes **SHALL** keep their LoRa radio and ESP32 MCU in an **always-on state** (no deep sleep), enabling continuous reception and forwarding of packets.
- No duty-cycling, time-synchronized wakeup schemes, or low-power scheduling are implemented in this version.
- Power optimization and sleep/awake scheduling are explicitly out of scope for V1 and may be introduced as a separate design in a future revision.

Implementation note:

- Do NOT use ESP32 deep sleep or light sleep in V1.
- Tasks MAY block on queues/semaphores, but the LoRa receive path MUST remain active.

---

## 8. BLE and Gateway (High-Level Placeholder)

(Details to be elaborated separately; included here only as context.)

- Each node runs a BLE server that accepts messages from a mobile phone.
- When a BLE message is received, the node wraps it into a `DATA` Packet, assigns an `ai_priority`, and injects it into the mesh.
- The gateway node receives mesh `DATA` packets and forwards them (with `ai_priority`) to the internet (e.g., via MQTT).
