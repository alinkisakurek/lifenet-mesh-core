// ============================================================
//  gateway.h  —  Gateway Modülü (WiFi öncelikli, GSM fallback)
//  Gereksinim: FR-8.1 … FR-8.7
// ============================================================
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "config.h"
#include "frame.h"

static const char* TAG_GW = "GATEWAY";

// ── YAPILANDIRMA ─────────────────────────────────────────────
#if NODE_ID == 3
  // Sadece Node 3 gerçek internete bağlansın (Kendi bilgilerinizi girin)
  #define WIFI_SSID     "GERCEK_WIFI_ADINIZ"
  #define WIFI_PASS     "GERCEK_WIFI_SIFRENIZ"
#else
  // Diğer nodelar internete bağlanmasın
  #define WIFI_SSID     "Alin iPhone'u"
  #define WIFI_PASS     "Alin202228030"
#endif

// Yardım ekiplerinin sunucusu (Kendi IP'nizi yazabilirsiniz)
#define SERVER_URL    "http://192.168.1.100:3000/api/mock/trigger-earthquake"

// ── GLOBAL DURUM (mesh_router.h tarafından okunur) ───────────
bool    g_has_wifi   = false;
bool    g_has_gsm    = false;
uint8_t g_hops_to_gw = 0xFF;   // Bu node gateway değil başlangıçta

// ── WiFi BAĞLANTI ────────────────────────────────────────────
static bool wifi_try_connect(uint16_t timeout_ms = 8000) {
    // Node 1 ve Node 2'nin boşuna WiFi aramamasını sağlayan kontrol
    if (WIFI_SSID[0] == 'N' && WIFI_SSID[1] == 'O') return false; 
    
    if (WiFi.status() == WL_CONNECTED) return true;

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    LOG_I(TAG_GW, "WiFi baglaniyor: %s", WIFI_SSID);

    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t > timeout_ms) {
            LOG_W(TAG_GW, "WiFi timeout");
            return false;
        }
        delay(250);
    }
    LOG_I(TAG_GW, "WiFi baglandi: %s", WiFi.localIP().toString().c_str());
    return true;
}

// ── HTTP POST ────────────────────────────────────────────────
static bool http_post_message(const LoraFrame *f) {
    HTTPClient http;
    http.begin(SERVER_URL);
    http.setTimeout(1500); // 1.5 saniye timeout! (Node'un donmasını engeller)
    http.addHeader("Content-Type", "application/json");

    // JSON payload
    char json[512];
    snprintf(json, sizeof(json),
        "{\"origin_id\":%d,\"seq\":%d,\"priority\":%d,"
        "\"hop_count\":%d,\"timestamp\":%lu,\"text\":\"%.*s\"}",
        f->hdr.origin_id, f->hdr.seq, f->hdr.priority,
        f->hdr.hop_count, (unsigned long)f->hdr.timestamp,
        f->hdr.payload_len, (char*)f->payload);

    int code = http.POST(json);
    http.end();

    if (code >= 200 && code < 300) {
        LOG_I(TAG_GW, "HTTP POST OK: %d, origin=%d seq=%d",
              code, f->hdr.origin_id, f->hdr.seq);
        return true;
    }
    LOG_E(TAG_GW, "HTTP POST FAIL: %d", code);
    return false;
}

// ── GATEWAY MESAJ İLET ───────────────────────────────────────
bool gateway_send_message(const LoraFrame *f) {
    // WiFi dene
    if (WiFi.status() == WL_CONNECTED || wifi_try_connect(5000)) {
        for (int attempt = 0; attempt < 3; attempt++) {
            if (http_post_message(f)) return true;
            delay(1000);
        }
        LOG_E(TAG_GW, "3 denemede HTTP gonderimi basarisiz");
    }

    // GSM dene (Faz 6)
    if (g_has_gsm) {
        LOG_W(TAG_GW, "GSM gonderimi: henuz implemente edilmedi (Faz 6)");
    }

    return false;
}

// ── GATEWAY DURUM KONTROLÜ ────────────────────────────────────
static void gateway_check() {
    bool prev_wifi = g_has_wifi;

    g_has_wifi = (WiFi.status() == WL_CONNECTED);

    // WiFi yoksa bağlanmayı dene (arka planda, kısa timeout)
    if (!g_has_wifi) {
        g_has_wifi = wifi_try_connect(3000);
    }

    bool is_gw = g_has_wifi || g_has_gsm;

    if (is_gw) {
        g_hops_to_gw = 0;   // Bu node kendisi gateway
    } else {
        g_hops_to_gw = 0xFF;
    }

    if (prev_wifi != g_has_wifi) {
        LOG_I(TAG_GW, "WiFi durumu degisti: %s",
              g_has_wifi ? "BAĞLI (Gateway aktif)" : "KOPTU (Gateway pasif)");
        Serial.printf("[GW] Gateway durumu: WiFi=%s GSM=%s\n",
                      g_has_wifi ? "ON" : "off",
                      g_has_gsm  ? "ON" : "off");
    }
}