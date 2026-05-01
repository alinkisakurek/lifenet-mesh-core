#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <string>
#include <NimBLEDevice.h>

#include "mesh_packet.h"
#include "lora_link.h"
#include "routing.h"
#include "store_forward.h"
#include "mesh_link.h"
#include "debug.h"

// ─── Mesh Config ────────────────────────────────────────────────────────────

#define LOCAL_ADDR  0x0001   // TODO: set per node
#define GATEWAY_ADDR 0x0004  // from PRD

#define PACKET_TYPE_DATA    0
#define PACKET_TYPE_RREQ    1
#define PACKET_TYPE_RREP    2
#define PACKET_TYPE_RERR    3
#define PACKET_TYPE_BEACON  4
#define PACKET_TYPE_ACK     6

static uint16_t local_seq_num = 0;

// ─── BLE / Activation Config (basit) ────────────────────────────────────────

static const char* DEVICE_NAME   = "ESP32_LifeNet_Node";
static const char* SERVICE_UUID  = "12345678-1234-1234-1234-123456789abc";
static const char* CHAR_RX_UUID  = "12345678-1234-1234-1234-123456789abd";
static const char* CHAR_TX_UUID  = "12345678-1234-1234-1234-123456789abe";

static const char* ACTIVATION_PASSWORD = "ACTIVATE_2026";
static const char* NVS_NAMESPACE       = "device_cfg";
static const char* NVS_KEY_ACTIVATED   = "activated";

static Preferences nvs;
static bool deviceActivated = false;

// ─── BLE Globals ────────────────────────────────────────────────────────────

static NimBLECharacteristic* pTxChar = nullptr;
static volatile uint8_t clientCount  = 0;

static const uint32_t NOTIFY_INTERVAL_MS = 50;
static uint32_t lastNotifyMs = 0;

// basit küçük TX kuyruğu
#define TX_BUF_SLOTS    8
#define TX_MSG_MAX_LEN  64

struct TxRingBuffer {
  char    data[TX_BUF_SLOTS][TX_MSG_MAX_LEN];
  uint8_t head;
  uint8_t tail;
  uint8_t count;
};
static TxRingBuffer txRing;
static portMUX_TYPE txRingMux = portMUX_INITIALIZER_UNLOCKED;

static void txRingInit(TxRingBuffer& rb) {
  rb.head = rb.tail = rb.count = 0;
}
static bool txRingPush(TxRingBuffer& rb, const char* msg) {
  portENTER_CRITICAL(&txRingMux);
  if (rb.count >= TX_BUF_SLOTS) {
    portEXIT_CRITICAL(&txRingMux);
    return false;
  }
  strncpy(rb.data[rb.head], msg, TX_MSG_MAX_LEN - 1);
  rb.data[rb.head][TX_MSG_MAX_LEN - 1] = '\0';
  rb.head = (rb.head + 1) % TX_BUF_SLOTS;
  rb.count++;
  portEXIT_CRITICAL(&txRingMux);
  return true;
}
static bool txRingPop(TxRingBuffer& rb, char* out, uint8_t maxLen) {
  portENTER_CRITICAL(&txRingMux);
  if (rb.count == 0) {
    portEXIT_CRITICAL(&txRingMux);
    return false;
  }
  strncpy(out, rb.data[rb.tail], maxLen - 1);
  out[maxLen - 1] = '\0';
  rb.tail = (rb.tail + 1) % TX_BUF_SLOTS;
  rb.count--;
  portEXIT_CRITICAL(&txRingMux);
  return true;
}
static uint8_t txRingCount(TxRingBuffer& rb) {
  portENTER_CRITICAL(&txRingMux);
  uint8_t c = rb.count;
  portEXIT_CRITICAL(&txRingMux);
  return c;
}
static void queueTxMessage(const char* msg) {
  if (!txRingPush(txRing, msg)) {
    Serial.println("[TX] Ring buffer full — message dropped");
  }
}

// ─── LoRa'ya forward helper ────────────────────────────────────────────────

static void forwardToLoRa(const std::string& msg) {
  Packet pkt;
  packet_init(pkt);
  pkt.type        = PACKET_TYPE_DATA;
  pkt.src_addr    = LOCAL_ADDR;
  pkt.dst_addr    = GATEWAY_ADDR;
  pkt.msg_id = esp_random();
  pkt.ttl         = 5;
  pkt.hop_count   = 0;
  pkt.payload_len = (msg.size() > MAX_PAYLOAD_LEN)
                    ? MAX_PAYLOAD_LEN
                    : (uint8_t)msg.size();
  memcpy(pkt.payload, msg.data(), pkt.payload_len);

  bool ok = mesh_send_unicast(pkt, LOCAL_ADDR, millis());
  Serial.printf("[LoRa] Forwarded BLE msg, status=%s\n", ok ? "OK" : "FAIL/QUEUED");
}

// ─── NVS helpers (sadece activated flag) ────────────────────────────────────

static bool nvsInit() {
  if (!nvs.begin(NVS_NAMESPACE, false)) {
    Serial.println("[NVS] ERROR — failed to open namespace");
    return false;
  }
  return true;
}
static bool nvsLoadActivated() {
  return nvs.getBool(NVS_KEY_ACTIVATED, false);
}
static bool nvsSaveActivated(bool val) {
  if (!nvs.putBool(NVS_KEY_ACTIVATED, val)) {
    Serial.println("[NVS] ERROR — failed to write activated flag");
    return false;
  }
  return true;
}

// ─── BLE callbacks ─────────────────────────────────────────────────────────

static void startAdvertising();

static void trimTrailing(std::string& s) {
  while (!s.empty()) {
    char c = s.back();
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') s.pop_back();
    else break;
  }
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo&) override {
    if (clientCount == 0) return;

    std::string rxValue = pChar->getValue();
    Serial.printf("[RX] %u bytes\n", (unsigned)rxValue.length());
    std::string trimmed = rxValue;
    trimTrailing(trimmed);

    if (!deviceActivated) {
      if (trimmed == ACTIVATION_PASSWORD) {
        Serial.println("[AUTH] Activation password correct");
        if (nvsSaveActivated(true)) {
          deviceActivated = true;
          queueTxMessage("ACTIVATED");
        } else {
          queueTxMessage("NVS_ERROR");
        }
      } else {
        queueTxMessage("WRONG_PW");
        Serial.println("[AUTH] Wrong password");
      }
      return;
    }

    // Activated mode → gelen metni LoRa'ya yolla
    if (!trimmed.empty()) {
      forwardToLoRa(trimmed);
      queueTxMessage("MSG_OK");
    }
  }
};

class TxCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info,
                   uint16_t subValue) override {
    if (subValue) {
      Serial.printf("[CCCD] Client subscribed (conn=%u)\n", info.getConnHandle());
    } else {
      Serial.printf("[CCCD] Client unsubscribed (conn=%u)\n", info.getConnHandle());
    }
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    clientCount++;
    Serial.printf("[BLE] Client connected (%d total)\n", clientCount);
    if (!deviceActivated) {
      queueTxMessage("NEED_ACTIVATION");
    }
    if (clientCount < 3) {
      startAdvertising();
    }
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    if (clientCount > 0) clientCount--;
    Serial.printf("[BLE] Disconnected (reason %d, %d remain)\n", reason, clientCount);
    startAdvertising();
  }
};

static void setupAdvertising() {
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setName(DEVICE_NAME);
  pAdv->enableScanResponse(true);
}
static void startAdvertising() {
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  if (pAdv->isAdvertising()) return;
  if (pAdv->start()) {
    Serial.println("[ADV] Advertising started");
  } else {
    Serial.println("[ADV] ERROR — failed to start");
  }
}

// ─── Orijinal setup() + BLE ekle ───────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n==================================");
  Serial.println("Life-Net LoRa Mesh Node Starting...");
  Serial.printf("Local Address: 0x%04X\n", LOCAL_ADDR);
  Serial.println("==================================\n");

  lora_init();
  routing_init();
  dupdet_init();
  sf_init();

  uint32_t now = millis();
  routing_add_or_update(
      GATEWAY_ADDR,
      0x0004,         // Node 2 icin burasi boyle olacak Aracı / Next Hop (NODE 1)
      1,              // hop_count
      0,              // seq_num
      now + 604800000, // node 
      0,
      ROUTE_VALID
  );

  // BLE + NVS init (LoRa'yı yukarıda bıraktık)
  txRingInit(txRing);

  if (nvsInit()) {
    deviceActivated = nvsLoadActivated();
    Serial.printf("[NVS] Device %s\n", deviceActivated ? "ACTIVATED" : "NOT ACTIVATED");
  } else {
    Serial.println("[NVS] Init failed — running in UNACTIVATED mode");
    deviceActivated = false;
  }

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  NimBLECharacteristic* pRxChar = pService->createCharacteristic(
    CHAR_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pRxChar->setCallbacks(new RxCallbacks());

  pTxChar = pService->createCharacteristic(
    CHAR_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
  );
  pTxChar->setCallbacks(new TxCallbacks());

  pService->start();
  setupAdvertising();
  startAdvertising();

  Serial.println("----------------------------------");
  Serial.printf("  Service  : %s\n", SERVICE_UUID);
  Serial.printf("  RX Char  : %s\n", CHAR_RX_UUID);
  Serial.printf("  TX Char  : %s\n", CHAR_TX_UUID);
  Serial.printf("  Status   : %s\n", deviceActivated ? "READY" : "AWAITING ACTIVATION");
  Serial.println("----------------------------------");
}

// ─── Orijinal loop() + en sona BLE TX drain ────────────────────────────────

void loop() {
  uint32_t now_ms = millis();
  Packet p;

  static uint32_t last_hb = 0;
  if (now_ms - last_hb >= 5000) {
      last_hb = now_ms;
      DBG_PRINTF("[HB] Node alive, millis=%u\n", now_ms);
  }

  // Try to receive a packet with a small timeout to not block too long
  if (lora_receive_packet(p, 50)) {
      DBG_PRINTF("[RX] type=%d src=0x%04X dst=0x%04X prev=0x%04X next=0x%04X ttl=%d hop=%d\n",
                 p.type, p.src_addr, p.dst_addr, p.prev_hop, p.next_hop, p.ttl, p.hop_count);
      
      if (p.type == PACKET_TYPE_RREQ || p.type == PACKET_TYPE_RREP || p.type == PACKET_TYPE_RERR) {
          ControlAction action;
          action.type = CTRL_DROP;
          
          if (p.type == PACKET_TYPE_RREQ) {
              action = handle_rreq(p, LOCAL_ADDR, now_ms);
          } else if (p.type == PACKET_TYPE_RREP) {
              action = handle_rrep(p, LOCAL_ADDR, now_ms);
          } else if (p.type == PACKET_TYPE_RERR) {
              action = handle_rerr(p, LOCAL_ADDR, now_ms);
          }

          if (action.type == CTRL_FORWARD_BROADCAST) {
              p.next_hop = 0xFFFF;
              lora_send_packet(p);
          } else if (action.type == CTRL_FORWARD_UNICAST) {
              p.next_hop = action.next_hop;
              lora_send_packet(p);
          } else if (action.type == CTRL_GENERATE_RREP) {
              Packet rrep;
              packet_init(rrep);
              rrep.msg_id = esp_random();
              rrep.src_addr = LOCAL_ADDR;
              rrep.dst_addr = p.src_addr;
              rrep.prev_hop = LOCAL_ADDR;
              rrep.next_hop = action.next_hop;
              rrep.ttl      = 7;
              rrep.hop_count   = 0;
              rrep.seq_num     = ++local_seq_num;
              rrep.type        = PACKET_TYPE_RREP;
              rrep.ai_priority = 1;
              rrep.payload_len = 0;
              lora_send_packet(rrep);
          }
      } else if (p.type == PACKET_TYPE_DATA || p.type == PACKET_TYPE_ACK) {
          mesh_handle_incoming(p, LOCAL_ADDR, now_ms);
      }
  }

  // Periodically process the store-and-forward queue
  sf_process(now_ms, [](Packet& pkt) -> bool {
      return mesh_retry_queued_packet(pkt, LOCAL_ADDR, millis());
  });

  // Simple Serial CLI
  if (Serial.available()) {
      char cmd = Serial.read();
      if (cmd == 'h') {
          DBG_PRINTLN("Commands:\n  h - help\n  r - dump routing table\n  q - dump store-and-forward queue\n  s - send test DATA to gateway");
      } else if (cmd == 'r') {
          routing_debug_dump(now_ms);
      } else if (cmd == 'q') {
          sf_debug_dump(now_ms);
      } else if (cmd == 's') {
          Packet pkt;
          packet_init(pkt);
          pkt.type      = PACKET_TYPE_DATA;
          pkt.src_addr  = LOCAL_ADDR;
          pkt.dst_addr  = GATEWAY_ADDR;
          pkt.msg_id = esp_random();
          pkt.ttl       = 7;
          const char* msg = "HELLO";
          pkt.payload_len = 5;
          for (int i = 0; i < 5; i++) pkt.payload[i] = msg[i];

          DBG_PRINTLN("[CLI] Sending test DATA to Gateway...");
          if (mesh_send_unicast(pkt, LOCAL_ADDR, now_ms)) {
              DBG_PRINTLN("[CLI] Send success (ACK received).");
          } else {
              DBG_PRINTLN("[CLI] Send failed (enqueued or no route).");
          }
      }
  }

  // BLE TX kuyruğunu ara ara boşalt
  if (clientCount > 0 && pTxChar && txRingCount(txRing) > 0) {
    if (now_ms - lastNotifyMs >= NOTIFY_INTERVAL_MS) {
      char msg[TX_MSG_MAX_LEN];
      if (txRingPop(txRing, msg, sizeof(msg))) {
        pTxChar->setValue((uint8_t*)msg, strlen(msg));
        if (pTxChar->notify()) {
          Serial.printf("[TX] \"%s\"\n", msg);
        } else {
          Serial.println("[TX] notify() failed");
        }
        lastNotifyMs = now_ms;
      }
    }
  }

  delay(1); // FreeRTOS watchdog
}