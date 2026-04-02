// ============================================================
//  mesh_state.h  —  Komşu Tablosu, Seen Cache, Mesaj Kuyruğu
//  Gereksinim: FR-4.3…4.5, FR-6.1…6.4, FR-7.1…7.7
// ============================================================
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "frame.h"

static const char* TAG_STATE = "STATE";
static Preferences prefs;

// ── KOMŞU TABLOSU ────────────────────────────────────────────
// Gereksinim: FR-4.3
typedef struct {
    uint8_t  node_id;
    int8_t   rssi;
    uint8_t  has_gateway;    // wifi veya gsm
    uint8_t  hops_to_gw;
    uint32_t last_seen_ms;
    bool     is_stale;
    bool     valid;
} Neighbor;

static Neighbor neighbor_table[MAX_NEIGHBORS];
static int neighbor_count = 0;

// Komşu ekle/güncelle — Gereksinim: FR-4.3, FR-4.4
static void neighbor_update(uint8_t nid, int8_t rssi, uint8_t has_gw,
                             uint8_t hops_to_gw) {
    // Varsa güncelle
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbor_table[i].valid && neighbor_table[i].node_id == nid) {
            neighbor_table[i].rssi        = rssi;
            neighbor_table[i].has_gateway = has_gw;
            neighbor_table[i].hops_to_gw  = hops_to_gw;
            neighbor_table[i].last_seen_ms = millis();
            neighbor_table[i].is_stale    = false;
            LOG_D(TAG_STATE, "Komsu guncellendi: %d rssi=%d gw=%d", nid, rssi, has_gw);
            return;
        }
    }
    // Yeni ekle
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (!neighbor_table[i].valid) {
            neighbor_table[i] = {nid, rssi, has_gw, hops_to_gw,
                                  (uint32_t)millis(), false, true};
            neighbor_count++;
            LOG_I(TAG_STATE, "Yeni komsu: %d rssi=%d gw=%d hops=%d",
                  nid, rssi, has_gw, hops_to_gw);
            return;
        }
    }
    LOG_W(TAG_STATE, "Komsu tablosu dolu!");
}

// Stale kontrolü — Gereksinim: FR-4.4
static void neighbor_check_stale() {
    uint32_t now = millis();
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (!neighbor_table[i].valid) continue;
        if (!neighbor_table[i].is_stale &&
            now - neighbor_table[i].last_seen_ms > NEIGHBOR_STALE_MS) {
            neighbor_table[i].is_stale = true;
            LOG_W(TAG_STATE, "Komsu stale: %d", neighbor_table[i].node_id);
        }
    }
}

// NVS kaydet/yükle — Gereksinim: FR-4.5
static void neighbors_save_nvs() {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBytes(NVS_KEY_NEIGHBORS, neighbor_table,
                   sizeof(neighbor_table));
    prefs.end();
    LOG_D(TAG_STATE, "Komsu tablosu NVS'e yazildi");
}

static void neighbors_load_nvs() {
    prefs.begin(NVS_NAMESPACE, true);
    size_t len = prefs.getBytesLength(NVS_KEY_NEIGHBORS);
    if (len == sizeof(neighbor_table)) {
        prefs.getBytes(NVS_KEY_NEIGHBORS, neighbor_table, len);
        // Reset sonrası eski girdiler stale sayılır
        for (int i = 0; i < MAX_NEIGHBORS; i++) {
            if (neighbor_table[i].valid)
                neighbor_table[i].is_stale = true;
        }
        LOG_I(TAG_STATE, "Komsu tablosu NVS'den yuklendi (stale)");
    }
    prefs.end();
}

// Terminal çıktısı — "neighbors" komutu için
static void neighbors_print() {
    Serial.println("\n╔══════════════════════════════════════════════╗");
    Serial.println("║         KOMŞU TABLOSU                       ║");
    Serial.println("╠═══╦══════╦══════╦═════╦════════╦══════════╣");
    Serial.println("║ # ║ ID   ║ RSSI ║ GW  ║ Hops   ║ Durum    ║");
    Serial.println("╠═══╬══════╬══════╬═════╬════════╬══════════╣");
    int shown = 0;
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        if (!neighbor_table[i].valid) continue;
        Neighbor& n = neighbor_table[i];
        Serial.printf("║ %d ║ N%-3d ║ %4d ║ %-3s ║ %-6d ║ %-8s ║\n",
            ++shown, n.node_id, n.rssi,
            n.has_gateway ? "YES" : "no",
            n.hops_to_gw == 0xFF ? 99 : n.hops_to_gw,
            n.is_stale ? "STALE" : "OK");
    }
    if (shown == 0)
        Serial.println("║       (komşu yok)                           ║");
    Serial.println("╚═══╩══════╩══════╩═════╩════════╩══════════╝\n");
}

// ── SEEN CACHE (Duplicate filtresi) ─────────────────────────
// Gereksinim: FR-6.1, FR-6.2
// origin_id(8bit) + seq(16bit) = 24bit key -> 32bit için padding
typedef struct {
    uint8_t  origin_id;
    uint16_t seq;
    bool     used;
} SeenEntry;

static SeenEntry seen_cache[SEEN_CACHE_SIZE];
static int seen_head = 0;

static bool seen_check(uint8_t origin_id, uint16_t seq) {
    for (int i = 0; i < SEEN_CACHE_SIZE; i++) {
        if (seen_cache[i].used &&
            seen_cache[i].origin_id == origin_id &&
            seen_cache[i].seq == seq)
            return true;
    }
    return false;
}

static void seen_mark(uint8_t origin_id, uint16_t seq) {
    seen_cache[seen_head] = {origin_id, seq, true};
    seen_head = (seen_head + 1) % SEEN_CACHE_SIZE;
}

// ── MESAJ KUYRUĞU ────────────────────────────────────────────
// Gereksinim: FR-7.1 … FR-7.7
typedef struct {
    LoraFrame frame;
    uint8_t   retry_count;
    uint32_t  last_attempt_ms;
    uint32_t  enqueue_time_ms;
    bool      valid;
} QueueEntry;

static QueueEntry msg_queue[QUEUE_MAX_SIZE];
static int queue_size = 0;

static int queue_count() {
    int c = 0;
    for (int i = 0; i < QUEUE_MAX_SIZE; i++)
        if (msg_queue[i].valid) c++;
    return c;
}

// Kuyruğa ekle — Gereksinim: FR-7.1, FR-7.6
static bool queue_push(const LoraFrame *f) {
    // Boş slot bul
    for (int i = 0; i < QUEUE_MAX_SIZE; i++) {
        if (!msg_queue[i].valid) {
            msg_queue[i].frame           = *f;
            msg_queue[i].retry_count     = 0;
            msg_queue[i].last_attempt_ms = 0;
            msg_queue[i].enqueue_time_ms = millis();
            msg_queue[i].valid           = true;
            queue_size++;
            LOG_I(TAG_STATE, "Kuyruga eklendi: origin=%d seq=%d (toplam %d)",
                  f->hdr.origin_id, f->hdr.seq, queue_size);
            return true;
        }
    }
    // Kuyruk dolu: en düşük öncelikli + en eski sil — Gereksinim: FR-7.6
    int victim = -1;
    uint8_t min_prio = 255;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < QUEUE_MAX_SIZE; i++) {
        if (!msg_queue[i].valid) continue;
        if (msg_queue[i].frame.hdr.priority < min_prio ||
            (msg_queue[i].frame.hdr.priority == min_prio &&
             msg_queue[i].enqueue_time_ms < oldest)) {
            min_prio = msg_queue[i].frame.hdr.priority;
            oldest   = msg_queue[i].enqueue_time_ms;
            victim   = i;
        }
    }
    if (victim >= 0) {
        LOG_W(TAG_STATE, "Kuyruk dolu, en eski/dusuk oncelikli silindi: slot=%d", victim);
        msg_queue[victim].frame           = *f;
        msg_queue[victim].retry_count     = 0;
        msg_queue[victim].last_attempt_ms = 0;
        msg_queue[victim].enqueue_time_ms = millis();
        return true;
    }
    return false;
}

// Kuyruğu sil (ACK alındığında) — Gereksinim: FR-6.4
static void queue_remove_by_seq(uint8_t origin_id, uint16_t seq) {
    for (int i = 0; i < QUEUE_MAX_SIZE; i++) {
        if (msg_queue[i].valid &&
            msg_queue[i].frame.hdr.origin_id == origin_id &&
            msg_queue[i].frame.hdr.seq == seq) {
            msg_queue[i].valid = false;
            queue_size--;
            LOG_I(TAG_STATE, "Kuyruktan silindi (ACK): origin=%d seq=%d",
                  origin_id, seq);
            return;
        }
    }
}

// Expired temizlik — Gereksinim: FR-7.7 (24 saat)
static void queue_expire() {
    uint32_t now = millis();
    for (int i = 0; i < QUEUE_MAX_SIZE; i++) {
        if (!msg_queue[i].valid) continue;
        if (now - msg_queue[i].enqueue_time_ms > 86400000UL) {
            LOG_W(TAG_STATE, "Kuyruk expired: origin=%d seq=%d",
                  msg_queue[i].frame.hdr.origin_id,
                  msg_queue[i].frame.hdr.seq);
            msg_queue[i].valid = false;
            queue_size--;
        }
    }
}

// NVS kaydet/yükle — Gereksinim: FR-7.2
static void queue_save_nvs() {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBytes(NVS_KEY_QUEUE, msg_queue, sizeof(msg_queue));
    prefs.end();
    LOG_D(TAG_STATE, "Kuyruk NVS'e yazildi (%d mesaj)", queue_count());
}

static void queue_load_nvs() {
    prefs.begin(NVS_NAMESPACE, true);
    size_t len = prefs.getBytesLength(NVS_KEY_QUEUE);
    if (len == sizeof(msg_queue)) {
        prefs.getBytes(NVS_KEY_QUEUE, msg_queue, len);
        queue_size = queue_count();
        LOG_I(TAG_STATE, "Kuyruk NVS'den yuklendi: %d mesaj", queue_size);
    }
    prefs.end();
}

// Terminal çıktısı — "queue" komutu için
static void queue_print() {
    Serial.println("\n╔══════════════════════════════════════════════════════╗");
    Serial.println("║              MESAJ KUYRUĞU                          ║");
    Serial.println("╠══╦══════╦═════╦═══════╦══════════╦══════════════╣");
    Serial.println("║# ║Origin║ Seq ║ Retry ║ Öncelik  ║ Süre (sn)    ║");
    Serial.println("╠══╬══════╬═════╬═══════╬══════════╬══════════════╣");
    int shown = 0;
    for (int i = 0; i < QUEUE_MAX_SIZE; i++) {
        if (!msg_queue[i].valid) continue;
        QueueEntry& q = msg_queue[i];
        uint32_t age = (millis() - q.enqueue_time_ms) / 1000;
        Serial.printf("║%-2d║ N%-4d║%5d║ %5d ║ %8d ║ %12d ║\n",
            ++shown, q.frame.hdr.origin_id, q.frame.hdr.seq,
            q.retry_count, q.frame.hdr.priority, age);
        // Mesaj içeriğini de göster
        Serial.printf("║   Mesaj: %-47s║\n", (char*)q.frame.payload);
    }
    if (shown == 0)
        Serial.println("║              (kuyruk boş)                           ║");
    Serial.println("╚══╩══════╩═════╩═══════╩══════════╩══════════════╝\n");
}

// Sequence counter — NVS'den yükle/artır
static uint16_t g_seq_counter = 0;

static uint16_t seq_next() {
    g_seq_counter++;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUShort(NVS_KEY_SEQ, g_seq_counter);
    prefs.end();
    return g_seq_counter;
}

static void seq_load_nvs() {
    prefs.begin(NVS_NAMESPACE, true);
    g_seq_counter = prefs.getUShort(NVS_KEY_SEQ, 0);
    prefs.end();
    LOG_D(TAG_STATE, "Seq counter yuklendi: %d", g_seq_counter);
}
