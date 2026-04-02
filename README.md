# 🛜 Life Net (Hayat Ağı) - Resilient LoRa Mesh Network

![Version](https://img.shields.io/badge/version-1.0.0--MVP-blue.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-lightgrey.svg)
![Framework](https://img.shields.io/badge/framework-Arduino-00979C.svg)
![Hardware](https://img.shields.io/badge/hardware-Ebyte_E22_LoRa-orange.svg)

**Life Net (Hayat Ağı)** is an autonomous, decentralized, and highly resilient LoRa-based mesh communication network. Designed specifically for disaster and emergency scenarios where cellular networks (GSM) and traditional internet infrastructure fail, this system ensures continuous data transmission through dynamic multi-hop routing.

When any single node within the mesh detects an active internet connection (Wi-Fi/GSM), it automatically elevates its status to a **Gateway**, bridging the isolated local LoRa network to a centralized Node.js backend to notify emergency services.

---

## ✨ Core Features & Technical Architecture

### 1. Dynamic & Autonomous Routing (Mesh Topology)
* **Periodic Beaconing:** Nodes broadcast heartbeat signals (Beacons) every 30 seconds to announce their presence, RSSI (signal strength), and distance to the nearest Gateway.
* **Smart Next-Hop Selection:** When dispatching a message, the routing algorithm evaluates the `Neighbor Table`. It prioritizes direct Gateways first, then nodes with the lowest hop-count to a Gateway, and falls back to standard broadcast flooding if isolated.
* **Stale Node Pruning:** Neighbors that fail to send a beacon within a 120-second window are automatically marked as `STALE` and excluded from routing paths to prevent packet loss.

### 2. Smart Gateway Architecture
* **Automatic Role Discovery:** Nodes actively monitor for predefined Wi-Fi credentials or GSM availability. Upon successful connection, the node broadcasts its new `Gateway` status (`hops = 0`) to the mesh.
* **HTTP Bridging:** Gateway nodes intercept user messages and forward them to a Node.js backend via HTTP POST using JSON payloads.
* **Mock Gateway Mode:** Includes a developer environment toggle (`MOCK_WIFI`) to simulate successful internet delivery and ACK generation for isolated testing.

### 3. Reliable Delivery & Persistence (Zero-Loss Target)
* **Pending Queue (RAM & NVS):** Outgoing messages are stored in a dedicated queue. If an acknowledgment (ACK) is not received, the node automatically retries transmission every 60 seconds (up to 3 times).
* **Delivery-ACK System:** Unicast messages and Gateway deliveries trigger a `TYPE_ACK` frame that routes backward through the mesh to the original sender, confirming end-to-end delivery and clearing the queue.
* **NVS (Non-Volatile Storage) State Recovery:** The sequence counter, neighbor table, and pending message queue are periodically written to the ESP32's flash memory. In the event of a power failure or brownout, the node instantly recovers its state upon reboot.
* **Duplicate Filtering:** A circular cache drops looping or duplicated packets to prevent broadcast storms.

### 4. Advanced CLI (Command Line Interface)
A built-in, interactive serial terminal (115200 baud) allows for real-time monitoring and network testing without requiring a mobile app in the MVP phase.

---

## 🛠️ Hardware Requirements & Abstraction

The codebase includes a Hardware Abstraction Layer (HAL) to seamlessly support various ESP32 development boards without changing the core protocol logic.

* **Microcontroller:** ESP32 (Supported variants: Standard ESP32/WROOM, LOLIN D32, Fixaj 3in1 Board)
* **Transceiver:** Ebyte E22 or E32 Series LoRa modules (Operating in UART Transparent Mode)
* **Pin Mapping:** Defined dynamically in `config.h` (Handles `M0`, `M1`, `AUX`, `TX`, and `RX` routing, including safe-delays for boards with unrouted `AUX` pins)

---

## 🚀 Installation & Setup

**1. Clone the repository:**
```bash
git clone https://github.com/alinkisakurek/lifenet-mesh-core.git
```

**2. Open the project** in the Arduino IDE.

**3. Configure the Node Identity & Hardware:**

Open `config.h` and set your specific hardware variant and unique Node ID:
```cpp
#define NODE_ID 1
#define HARDWARE_VARIANT BOARD_FIXAJ_3IN1
```

**4. Configure the Gateway (Optional):**

Open `gateway.h` and provide your Wi-Fi credentials for the node you wish to act as the internet bridge. For non-gateway nodes, leave the credentials as `"NO_WIFI"`.

**5. Compile & Upload:**

Upload the code to your ESP32. *(Recommended upload speed: `115200` baud to prevent CH340 serial chip timeouts.)*

---

## 💻 CLI Commands & Usage

Once booted, open the Serial Monitor (115200 baud) to interact with the node.

| Command | Description |
| :--- | :--- |
| `status` | Displays comprehensive node state (ID, HW variant, Gateway status, Uptime, Queue depth, and NVS load state). |
| `neighbors` | Prints the active Neighbor Table, showing ID, RSSI, Gateway availability, and Hop Count for surrounding nodes. |
| `send <text>` | Queues and dispatches a text payload into the mesh network. *Optionally, specify priority (e.g., `send 9 SOS`).* |
| `queue` | Displays the Pending Queue, showing MSG_IDs awaiting Delivery-ACKs and their retry counts. |
| `beacon` | Manually triggers an immediate network Beacon to announce presence to the mesh. |
| `config` | Reads and outputs the low-level E22 LoRa module configuration registers. |

---

