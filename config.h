// ============================================================
//  config.h  —  LoRa Mesh Node Konfigürasyonu
//  Her node için sadece bu dosyadaki NODE_ID ve
//  HARDWARE_VARIANT değerini değiştir.
// ============================================================
#pragma once

// ── NODE KİMLİĞİ ────────────────────────────────────────────
// Node 1 = 1, Node 2 = 2, Node 3 = 3
#define NODE_ID        1

// ── DONANIM SEÇİMİ ──────────────────────────────────────────
// 1 = LOLIN D32 V1.0.0     (GPIO26=M0, GPIO27=M1)
// 2 = ESP32 Lite V1.0.0    (GPIO25=M0, GPIO26=M1)
// 3 = Fixaj 3in1 PCB       (GPIO32=M0, GPIO33=M1, Serial1)
#define HARDWARE_VARIANT  1

// ── PIN TANIMLARI ────────────────────────────────────────────
#if HARDWARE_VARIANT == 1
  // LOLIN D32
  #define LORA_AUX  4
  #define LORA_TX   16   // ESP32 TX -> E22 RXD
  #define LORA_RX   17   // ESP32 RX -> E22 TXD
  #define LORA_M0   26
  #define LORA_M1   27
  #define LORA_SERIAL_NUM 2   // HardwareSerial(2)

#elif HARDWARE_VARIANT == 2
  // ESP32 Lite V1.0.0
  #define LORA_AUX  4
  #define LORA_TX   16
  #define LORA_RX   17
  #define LORA_M0   25
  #define LORA_M1   26
  #define LORA_SERIAL_NUM 2

#elif HARDWARE_VARIANT == 3
  // Fixaj 3in1 PCB — Serial1 kullanıyor
  #define LORA_AUX  -1   // PCB'de bağlı değil, delay kullanılır
  #define LORA_TX   35   // E22 TXD -> ESP32 RX
  #define LORA_RX   27   // E22 RXD -> ESP32 TX
  #define LORA_M0   32
  #define LORA_M1   33
  #define LORA_SERIAL_NUM 1   // HardwareSerial(1)
#endif

// ── LORA RF AYARLARI ─────────────────────────────────────────
#define LORA_CHANNEL     18      // 868.125 MHz
#define LORA_NET_ID       0
#define LORA_BAUD      9600
#define LORA_AIR_RATE  0x02     // 2.4 kbps
#define LORA_TX_POWER  0x00     // 22 dBm (E22 max)

// ── MESH PROTOKOL PARAMETRELERİ ──────────────────────────────
#define MAX_NEIGHBORS         10
#define MAX_HOP_COUNT         10
#define SEEN_CACHE_SIZE       64
#define QUEUE_MAX_SIZE        50
#define MAX_RETRY_COUNT        3

#define BEACON_INTERVAL_MS    30000UL   // 30 saniye
#define RETRY_INTERVAL_MS     60000UL   // 60 saniye
#define NEIGHBOR_STALE_MS    120000UL   // 120 saniye

// ── FRAME SABITLERI ──────────────────────────────────────────
#define FRAME_MAGIC          0xABCD
#define FRAME_VERSION        0x01
#define FRAME_TYPE_CTRL      0x01
#define FRAME_TYPE_DATA      0x02
#define FRAME_TYPE_ACK       0x03
#define FRAME_TYPE_PING      0x05
#define PAYLOAD_MAX_SIZE      200

// ── NVS ANAHTARLARI ──────────────────────────────────────────
#define NVS_NAMESPACE        "mesh"
#define NVS_KEY_QUEUE        "queue"
#define NVS_KEY_NEIGHBORS    "neighbors"
#define NVS_KEY_SEQ          "seq"

// ── DEBUG LOG ────────────────────────────────────────────────
// 0=kapalı, 1=ERROR, 2=WARN, 3=INFO, 4=DEBUG
#define LOG_LEVEL   4

#define LOG_E(tag, fmt, ...) if(LOG_LEVEL>=1) Serial.printf("[ERR][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) if(LOG_LEVEL>=2) Serial.printf("[WRN][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_I(tag, fmt, ...) if(LOG_LEVEL>=3) Serial.printf("[INF][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_D(tag, fmt, ...) if(LOG_LEVEL>=4) Serial.printf("[DBG][%s] " fmt "\n", tag, ##__VA_ARGS__)
