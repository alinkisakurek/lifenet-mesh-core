// ============================================================
//  frame.h  —  Frame Formatı, Struct Tanımları, CRC32
//  Gereksinim: FR-2.1 … FR-2.6
// ============================================================
#pragma once
#include <Arduino.h>
#include "config.h"

// ── PACKED FRAME HEADER ──────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t magic;           // 0xABCD
    uint8_t  version;         // 0x01
    uint8_t  type;            // FRAME_TYPE_*
    uint8_t  src_id;          // Bu hop'u gönderen node
    uint8_t  dst_id;          // Hedef node (0xFF = broadcast)
    uint8_t  origin_id;       // Mesajı ilk üreten node (sabit)
    uint8_t  hop_count;       // 0'dan başlar, max MAX_HOP_COUNT
    uint8_t  priority;        // Mobil'den gelen öncelik skoru
    uint16_t seq;             // Origin node'un sequence counter'ı
    uint32_t timestamp;       // Unix timestamp (ms/1000)
    uint16_t payload_len;     // Payload boyutu
} FrameHeader;

// ── TAM FRAME ────────────────────────────────────────────────
// ── TAM FRAME ────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    FrameHeader hdr;
    uint8_t  payload[PAYLOAD_MAX_SIZE + sizeof(uint32_t)]; // CRC için ek alan
} LoraFrame;

// ── CTRL/BEACON PAYLOAD ──────────────────────────────────────
// Gereksinim: FR-4.2
typedef struct __attribute__((packed)) {
    uint8_t  has_wifi;        // 1 = aktif WiFi
    uint8_t  has_gsm;         // 1 = aktif GSM
    uint8_t  hops_to_gw;      // gateway'e hop sayısı, 0xFF=ulaşılamaz
    int8_t   last_rssi;       // son ölçülen RSSI (signed)
    uint16_t queue_depth;     // bekleyen mesaj sayısı
    uint16_t reserved;
} BeaconPayload;

// ── ACK PAYLOAD ──────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t  origin_id;
    uint16_t ack_seq;         // hangi seq için ACK
    uint8_t  delivered;       // 1 = gateway'e ulaştı
} AckPayload;

// ── MSG_ID üretimi ───────────────────────────────────────────
// Gereksinim: FR-2.2  (src_id + timestamp + seq birleşimi)
// Duplicate cache için 32-bit hash kullanıyoruz (RAM tasarrufu)
inline uint32_t makeMsgId(uint8_t origin_id, uint16_t seq) {
    return ((uint32_t)origin_id << 24) | ((uint32_t)(millis() & 0xFFFF) << 8) | (seq & 0xFF);
}

// ── CRC32 ────────────────────────────────────────────────────
// Gereksinim: FR-2.5, NFR-4.2
inline uint32_t crc32_compute(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

inline bool frame_verify_crc(const LoraFrame *f) {
    // 1. Calculate the CRC of the actual data
    size_t dataLen = sizeof(FrameHeader) + f->hdr.payload_len;
    uint32_t computed = crc32_compute((const uint8_t*)f, dataLen);
    
    // 2. Read the 4-byte CRC that was appended exactly after the payload
    uint32_t received_crc;
    memcpy(&received_crc, ((const uint8_t*)f) + dataLen, sizeof(uint32_t));
    
    return computed == received_crc;
}

inline void frame_set_crc(LoraFrame *f) {
    // 1. Calculate the CRC
    size_t dataLen = sizeof(FrameHeader) + f->hdr.payload_len;
    uint32_t computed = crc32_compute((const uint8_t*)f, dataLen);
    
    // 2. Write the 4-byte CRC exactly after the payload
    memcpy(((uint8_t*)f) + dataLen, &computed, sizeof(uint32_t));
}

inline size_t frame_total_size(const LoraFrame *f) {
    return sizeof(FrameHeader) + f->hdr.payload_len + sizeof(uint32_t);
}

// ── FRAME BUILD YARDIMCILARI ─────────────────────────────────
inline void frame_init(LoraFrame *f, uint8_t type,
                       uint8_t src, uint8_t dst, uint8_t origin,
                       uint8_t priority, uint16_t seq) {
    memset(f, 0, sizeof(LoraFrame));
    f->hdr.magic      = FRAME_MAGIC;
    f->hdr.version    = FRAME_VERSION;
    f->hdr.type       = type;
    f->hdr.src_id     = src;
    f->hdr.dst_id     = dst;
    f->hdr.origin_id  = origin;
    f->hdr.hop_count  = 0;
    f->hdr.priority   = priority;
    f->hdr.seq        = seq;
    f->hdr.timestamp  = (uint32_t)(millis() / 1000);
}
