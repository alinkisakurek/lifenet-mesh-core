// ============================================================
//  mesh_router.h  —  Mesh Yönlendirme Motoru
//  Gereksinim: FR-5.1…5.6, FR-6.1…6.4, FR-4.1…4.5
// ============================================================
#pragma once
#include <Arduino.h>
#include "config.h"
#include "frame.h"
#include "e22_driver.h"
#include "mesh_state.h"

static const char* TAG_ROUTER = "ROUTER";

// ── FORWARD DECLARATION ───────────────────────────────────────
static void route_frame(LoraFrame *f, int8_t rx_rssi);
static void send_ack(uint8_t origin_id, uint16_t seq, bool delivered);
static void queue_retry_all();

// Gateway durumu (gateway.h tarafından güncellenir)
extern bool g_has_wifi;
extern bool g_has_gsm;
extern uint8_t g_hops_to_gw;
extern bool gateway_send_message(const LoraFrame *f);

// ── BEACON GÖNDER ────────────────────────────────────────────
// Gereksinim: FR-4.1, FR-4.2
static void send_beacon() {
    LoraFrame f;
    frame_init(&f, FRAME_TYPE_CTRL,
               NODE_ID, 0xFF, NODE_ID, 0, 0);

    BeaconPayload bp;
    bp.has_wifi    = g_has_wifi ? 1 : 0;
    bp.has_gsm     = g_has_gsm  ? 1 : 0;
    bp.hops_to_gw  = g_hops_to_gw;
    bp.last_rssi   = 0;       // bu node'un kendi RSSI'sı yok beacon'da
    bp.queue_depth = (uint16_t)queue_count();
    bp.reserved    = 0;

    memcpy(f.payload, &bp, sizeof(bp));
    f.hdr.payload_len = sizeof(bp);
    frame_set_crc(&f);

    size_t sz = frame_total_size(&f);
    e22_send((uint8_t*)&f, sz);
    LOG_I(TAG_ROUTER, "Beacon yayinlandi: wifi=%d gsm=%d hops=%d queue=%d",
          bp.has_wifi, bp.has_gsm, bp.hops_to_gw, bp.queue_depth);
}

// ── EN İYİ NEXT-HOP SEÇİMİ ───────────────────────────────────
// Gereksinim: FR-5.2
// Dönüş: -1 = uygun komşu yok
static int8_t pick_next_hop() {
    int8_t best_gw_node    = -1;
    int8_t best_gw_rssi    = -128;
    int8_t best_relay_node = -1;
    uint8_t best_hops      = 0xFF;
    int8_t  best_relay_rssi = -128;

    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        Neighbor& n = neighbor_table[i];
        if (!n.valid || n.is_stale) continue;

        // (1) Gateway'i olan komşu: en iyi RSSI seç
        if (n.has_gateway && n.rssi > best_gw_rssi) {
            best_gw_rssi = n.rssi;
            best_gw_node = (int8_t)n.node_id;
        }

        // (2) Yoksa: gateway'e en az hop mesafesi olan komşu
        if (!n.has_gateway) {
            if (n.hops_to_gw < best_hops ||
                (n.hops_to_gw == best_hops && n.rssi > best_relay_rssi)) {
                best_hops       = n.hops_to_gw;
                best_relay_rssi = n.rssi;
                best_relay_node = (int8_t)n.node_id;
            }
        }
    }

    if (best_gw_node >= 0) return best_gw_node;
    if (best_relay_node >= 0 && best_hops < 0xFF) return best_relay_node;
    return -1;  // (3) Uygun komşu yok → PENDING
}

// ── DATA FRAME ROUTE ET ───────────────────────────────────────
// Gereksinim: FR-5.1, FR-5.4, FR-6.1, FR-6.2, FR-6.3
static void route_frame(LoraFrame *f, int8_t rx_rssi) {
    // FR-6.3: Kendi mesajım döngüye mi girdi?
    if (f->hdr.origin_id == NODE_ID) {
        LOG_D(TAG_ROUTER, "Kendi mesajim geri dondu, dusuruldu (origin=%d seq=%d)",
              f->hdr.origin_id, f->hdr.seq);
        return;
    }

    // FR-6.1, FR-6.2: Duplicate kontrolü
    if (seen_check(f->hdr.origin_id, f->hdr.seq)) {
        LOG_D(TAG_ROUTER, "Duplicate, dusuruldu: origin=%d seq=%d",
              f->hdr.origin_id, f->hdr.seq);
        return;
    }
    seen_mark(f->hdr.origin_id, f->hdr.seq);

    // FR-5.4: TTL / hop limit
    if (f->hdr.hop_count >= MAX_HOP_COUNT) {
        LOG_W(TAG_ROUTER, "Hop limit asildi (%d), dusuruldu", f->hdr.hop_count);
        return;
    }

    // Bu node gateway mi? FR-8.1
    if (g_has_wifi || g_has_gsm) {
        LOG_I(TAG_ROUTER, "Mesaj gateway'e ulasti: origin=%d seq=%d prio=%d",
              f->hdr.origin_id, f->hdr.seq, f->hdr.priority);
        Serial.printf("\n>>> GATEWAY: Mesaj alindi\n");
        Serial.printf("    Origin: N%d | Seq: %d | Hop: %d | Prio: %d\n",
                      f->hdr.origin_id, f->hdr.seq,
                      f->hdr.hop_count, f->hdr.priority);
        Serial.printf("    Metin : %.*s\n\n",
                      f->hdr.payload_len, (char*)f->payload);

        bool ok = gateway_send_message(f);
        if (ok) {
            send_ack(f->hdr.origin_id, f->hdr.seq, true);
        }
        return;
    }

    // Relay: src_id güncelle, hop_count artır
    f->hdr.src_id    = NODE_ID;
    f->hdr.hop_count++;
    frame_set_crc(f);

    int8_t next = pick_next_hop();
    if (next < 0) {
        // FR-7.1: Uygun komşu yok → kuyruğa al
        LOG_W(TAG_ROUTER, "Uygun komsu yok, kuyruğa eklendi: origin=%d seq=%d",
              f->hdr.origin_id, f->hdr.seq);
        queue_push(f);
        queue_save_nvs();
        return;
    }

    // Flooding: tüm aktif (non-stale) komşulara broadcast
    // (Düz flood; gelecekte gradient routing ile değiştirilebilir — ADR-01)
    size_t sz = frame_total_size(f);
    bool sent = e22_send((uint8_t*)f, sz);
    if (!sent) {
        queue_push(f);
        queue_save_nvs();
    } else {
        LOG_I(TAG_ROUTER, "Relay: origin=%d seq=%d hop=%d rssi=%d",
              f->hdr.origin_id, f->hdr.seq, f->hdr.hop_count, rx_rssi);
    }
}

// ── ACK GÖNDER ───────────────────────────────────────────────
// Gereksinim: FR-5.5, FR-8.6
static void send_ack(uint8_t origin_id, uint16_t seq, bool delivered) {
    LoraFrame f;
    frame_init(&f, FRAME_TYPE_ACK,
               NODE_ID, 0xFF, NODE_ID, 0, seq_next());

    AckPayload ap;
    ap.origin_id = origin_id;
    ap.ack_seq   = seq;
    ap.delivered = delivered ? 1 : 0;

    memcpy(f.payload, &ap, sizeof(ap));
    f.hdr.payload_len = sizeof(ap);
    frame_set_crc(&f);

    size_t sz = frame_total_size(&f);
    e22_send((uint8_t*)&f, sz);
    LOG_D(TAG_ROUTER, "ACK gonderildi: origin=%d seq=%d delivered=%d",
          origin_id, seq, delivered);
}

// ── GELEN FRAME İŞLE ─────────────────────────────────────────
static void process_incoming(LoraFrame *f, int8_t rssi) {
    // Magic ve versiyon kontrolü — Gereksinim: FR-2.6
    if (f->hdr.magic != FRAME_MAGIC) {
        LOG_W(TAG_ROUTER, "Yanlis magic: 0x%04X", f->hdr.magic);
        return;
    }
    if (f->hdr.version != FRAME_VERSION) {
        LOG_W(TAG_ROUTER, "Bilinmeyen versiyon: %d", f->hdr.version);
        return;
    }
    if (!frame_verify_crc(f)) {
        LOG_W(TAG_ROUTER, "CRC hatasi, dusuruldu");
        return;
    }

    switch (f->hdr.type) {
        case FRAME_TYPE_CTRL: {
            // Beacon — komşu tablosunu güncelle — Gereksinim: FR-4.3
            if (f->hdr.payload_len >= sizeof(BeaconPayload)) {
                BeaconPayload bp;
                memcpy(&bp, f->payload, sizeof(bp));
                neighbor_update(f->hdr.src_id, rssi,
                                bp.has_wifi || bp.has_gsm,
                                bp.hops_to_gw);
                neighbors_save_nvs();

                // Komşu güncellenince kuyruğu dene — Gereksinim: FR-7.4
                queue_retry_all();
            }
            break;
        }
        case FRAME_TYPE_DATA: {
            // Terminale mesaj göster (diğer node'ların mesajları)
            Serial.printf("\n[RX] N%d->N%d | Seq:%d | Hop:%d | Prio:%d | RSSI:%d\n",
                          f->hdr.origin_id, f->hdr.dst_id,
                          f->hdr.seq, f->hdr.hop_count,
                          f->hdr.priority, rssi);
            Serial.printf("     Metin: %.*s\n\n",
                          f->hdr.payload_len, (char*)f->payload);
            route_frame(f, rssi);
            break;
        }
        case FRAME_TYPE_ACK: {
            if (f->hdr.payload_len >= sizeof(AckPayload)) {
                AckPayload ap;
                memcpy(&ap, f->payload, sizeof(ap));
                if (ap.delivered) {
                    // Gereksinim: FR-6.4
                    queue_remove_by_seq(ap.origin_id, ap.ack_seq);
                    queue_save_nvs();
                    Serial.printf("[ACK] Mesaj iletildi: origin=N%d seq=%d\n",
                                  ap.origin_id, ap.ack_seq);
                }
            }
            break;
        }
        case FRAME_TYPE_PING: {
            LOG_D(TAG_ROUTER, "PING alindi: src=%d", f->hdr.src_id);
            send_ack(f->hdr.src_id, f->hdr.seq, false);
            break;
        }
        default:
            LOG_W(TAG_ROUTER, "Bilinmeyen frame tipi: 0x%02X", f->hdr.type);
    }
}

// ── KUYRUK RETRY ─────────────────────────────────────────────
// Gereksinim: FR-7.4, FR-7.5
static void queue_retry_all() {
    int retried = 0;
    for (int i = 0; i < QUEUE_MAX_SIZE; i++) {
        if (!msg_queue[i].valid) continue;

        // Retry sayacı MAX_RETRY'a ulaşmışsa atla (komşu bulunana kadar sayaç artmaz)
        int8_t next = pick_next_hop();
        if (next < 0) {
            LOG_D(TAG_ROUTER, "Retry: hala komsu yok, bekleniyor");
            return;
        }

        LoraFrame *f = &msg_queue[i].frame;
        f->hdr.src_id = NODE_ID;
        f->hdr.hop_count++;
        frame_set_crc(f);

        size_t sz = frame_total_size(f);
        if (e22_send((uint8_t*)f, sz)) {
            msg_queue[i].last_attempt_ms = millis();
            msg_queue[i].retry_count++;
            retried++;
            LOG_I(TAG_ROUTER, "Kuyruk retry: origin=%d seq=%d attempt=%d",
                  f->hdr.origin_id, f->hdr.seq, msg_queue[i].retry_count);

            // FR-7.5: MAX_RETRY aşıldıysa sil (artık komşu var ama hata)
            if (msg_queue[i].retry_count >= MAX_RETRY_COUNT) {
                LOG_E(TAG_ROUTER, "Max retry asildi, mesaj silindi: origin=%d seq=%d",
                      f->hdr.origin_id, f->hdr.seq);
                msg_queue[i].valid = false;
                queue_size--;
            }
        }
    }
    if (retried > 0) queue_save_nvs();
}

// ── YENİ KULLANICI MESAJI OLUŞTUR ────────────────────────────
static void send_user_message(const char *text, uint8_t priority = 5) {
    LoraFrame f;
    uint16_t seq = seq_next();
    frame_init(&f, FRAME_TYPE_DATA,
               NODE_ID, 0xFF, NODE_ID, priority, seq);

    size_t tlen = strlen(text);
    if (tlen > PAYLOAD_MAX_SIZE - 1) tlen = PAYLOAD_MAX_SIZE - 1;
    memcpy(f.payload, text, tlen);
    f.payload[tlen]   = 0;
    f.hdr.payload_len = (uint16_t)tlen;
    frame_set_crc(&f);

    // Seen cache'e ekle (kendi mesajımı tekrar işleme)
    seen_mark(NODE_ID, seq);

    int8_t next = pick_next_hop();
    size_t sz   = frame_total_size(&f);

    if (next >= 0 && e22_send((uint8_t*)&f, sz)) {
        Serial.printf("[TX] Mesaj gonderildi: seq=%d prio=%d hop=N%d\n",
                      seq, priority, next);
    } else {
        Serial.printf("[TX] Komsu yok, kuyruğa alindi: seq=%d\n", seq);
        queue_push(&f);
        queue_save_nvs();
    }
}
