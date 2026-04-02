// ============================================================
//  lora_mesh.ino  —  Ana Dosya
//
//  KURULUM:
//    1. config.h içinde NODE_ID ve HARDWARE_VARIANT değiştir
//    2. gateway.h içinde WIFI_SSID ve WIFI_PASS doldur
//    3. Gerekli kütüphane yok (hepsi dahili Arduino + Preferences)
//
//  TERMİNAL KOMUTLARI (Serial Monitor, 115200 baud, newline):
//    neighbors    — komşu tablosunu göster
//    queue        — mesaj kuyruğunu göster
//    send <metin> — LoRa üzerinden mesaj gönder
//    beacon       — hemen beacon gönder
//    config       — E22 konfigürasyonunu oku ve göster
//    status       — node durumunu göster
//    help         — komut listesi
// ============================================================

#include "config.h"
#include "frame.h"
#include "gateway.h"      // g_has_wifi, g_has_gsm, g_hops_to_gw tanımları
#include "e22_driver.h"
#include "mesh_state.h"
#include "mesh_router.h"

static const char* TAG_MAIN = "MAIN";

// ── ZAMANLAYICILAR ────────────────────────────────────────────
static uint32_t t_beacon    = 0;
static uint32_t t_retry     = 0;
static uint32_t t_stale     = 0;
static uint32_t t_gw_check  = 0;
static uint32_t t_nvs_save  = 0;

// ── TERMİNAL KOMUT BUFFER ─────────────────────────────────────
static char cmd_buf[256];
static int  cmd_len = 0;

// ── KOMUT İŞLEYİCİ ───────────────────────────────────────────
static void handle_command(const char *cmd) {
    Serial.printf("\n> %s\n", cmd);

    if (strcmp(cmd, "neighbors") == 0) {
        neighbors_print();

    } else if (strcmp(cmd, "queue") == 0) {
        queue_print();

    } else if (strncmp(cmd, "send ", 5) == 0) {
        const char *text = cmd + 5;
        if (strlen(text) == 0) {
            Serial.println("Kullanim: send <metin>");
            return;
        }
        // Öncelik varsayılan 5; "send 8 yardim" formatında da alınabilir
        uint8_t prio = 5;
        if (text[1] == ' ' && text[0] >= '1' && text[0] <= '9') {
            prio = text[0] - '0';
            text += 2;
        }
        send_user_message(text, prio);

    } else if (strcmp(cmd, "beacon") == 0) {
        send_beacon();
        Serial.println("Beacon gönderildi.");

    } else if (strcmp(cmd, "config") == 0) {
        e22_print_config();

    } else if (strcmp(cmd, "status") == 0) {
        Serial.println("\n╔══════════════════════════════════╗");
        Serial.println("║         NODE DURUMU              ║");
        Serial.println("╠══════════════════════════════════╣");
        Serial.printf( "║ Node ID       : %-16d ║\n", NODE_ID);
        Serial.printf( "║ HW Variant    : %-16d ║\n", HARDWARE_VARIANT);
        Serial.printf( "║ WiFi          : %-16s ║\n", g_has_wifi ? "BAĞLI" : "yok");
        Serial.printf( "║ GSM           : %-16s ║\n", g_has_gsm  ? "BAĞLI" : "yok");
        Serial.printf( "║ Gateway       : %-16s ║\n",
                       (g_has_wifi||g_has_gsm) ? "AKTİF" : "değil");
        Serial.printf( "║ Hops to GW    : %-16d ║\n",
                       g_hops_to_gw == 0xFF ? 99 : g_hops_to_gw);
        Serial.printf( "║ Komşu sayısı  : %-16d ║\n", queue_count() >= 0 ? neighbor_count : 0);
        Serial.printf( "║ Kuyruk        : %-16d ║\n", queue_count());
        Serial.printf( "║ Seq counter   : %-16d ║\n", g_seq_counter);
        Serial.printf( "║ Uptime (sn)   : %-16lu ║\n", millis()/1000);
        Serial.println("╚══════════════════════════════════╝\n");

    } else if (strcmp(cmd, "help") == 0) {
        Serial.println("\n=== KOMUTLAR ===");
        Serial.println("  neighbors       — komşu tablosunu göster");
        Serial.println("  queue           — mesaj kuyruğunu göster");
        Serial.println("  send <metin>    — mesaj gönder (öncelik 1-9 ile: send 8 yardim)");
        Serial.println("  beacon          — beacon yayınla");
        Serial.println("  config          — E22 konfigürasyonunu oku");
        Serial.println("  status          — node durumu");
        Serial.println("  help            — bu yardım");
        Serial.println("================\n");

    } else {
        Serial.printf("Bilinmeyen komut: '%s' — 'help' yazın\n", cmd);
    }
}

// ── SETUP ────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n");
    Serial.println("╔════════════════════════════════════════╗");
    Serial.println("║     LoRa Mesh Node — Başlatılıyor     ║");
    Serial.printf( "║     Node ID: %-3d  |  HW: %d           ║\n",
                   NODE_ID, HARDWARE_VARIANT);
    Serial.println("╚════════════════════════════════════════╝");

    // NVS'den yükle
    seq_load_nvs();
    neighbors_load_nvs();
    queue_load_nvs();

    // E22 başlat ve konfigüre et
    e22_init(NODE_ID);

    // Gateway başlat
    WiFi.mode(WIFI_STA);
    gateway_check();

    // İlk beacon
    delay(1000);
    send_beacon();

    // Zamanlayıcıları başlat
    t_beacon   = millis();
    t_retry    = millis();
    t_stale    = millis();
    t_gw_check = millis();
    t_nvs_save = millis();

    Serial.println("\nHazır. Komutlar için 'help' yazın.\n");
}

// ── LOOP ─────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    // ── LoRa RX ──────────────────────────────────────────────
    static uint8_t rx_buf[sizeof(LoraFrame) + 4];
    int8_t rx_rssi = -120;
    int n = e22_recv(rx_buf, sizeof(rx_buf), &rx_rssi);
    if (n >= (int)sizeof(FrameHeader)) {
        LoraFrame *f = (LoraFrame*)rx_buf;
        process_incoming(f, rx_rssi);
    }

    // ── Beacon zamanlayıcısı ──────────────────────────────────
    if (now - t_beacon >= BEACON_INTERVAL_MS) {
        t_beacon = now;
        neighbor_check_stale();
        send_beacon();
    }

    // ── Retry zamanlayıcısı ───────────────────────────────────
    if (now - t_retry >= RETRY_INTERVAL_MS) {
        t_retry = now;
        queue_expire();
        if (queue_count() > 0) queue_retry_all();
    }

    // ── Gateway kontrol ───────────────────────────────────────
    if (now - t_gw_check >= 15000UL) {
        t_gw_check = now;
        gateway_check();
    }

    // ── Periyodik NVS kayıt ───────────────────────────────────
    if (now - t_nvs_save >= 300000UL) {  // 5 dakikada bir
        t_nvs_save = now;
        neighbors_save_nvs();
        queue_save_nvs();
    }

    // ── Seri terminal girişi ──────────────────────────────────
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (cmd_len > 0) {
                cmd_buf[cmd_len] = '\0';
                handle_command(cmd_buf);
                cmd_len = 0;
            }
        } else if (cmd_len < (int)sizeof(cmd_buf) - 1) {
            cmd_buf[cmd_len++] = c;
        }
    }

    delay(10);
}
