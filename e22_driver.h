// ============================================================
//  e22_driver.h  —  E22-900T22D UART Sürücüsü
//  Gereksinim: NFR-2.5 (AUX pin), FR-4.2 (Packet RSSI)
//  Faz 1 çıktısı: lora_send / lora_recv / lora_configure
// ============================================================
#pragma once
#include <Arduino.h>
#include <HardwareSerial.h>
#include "config.h"
#include "frame.h"

static const char* TAG_E22 = "E22";

// E22 için HardwareSerial nesnesi
#if LORA_SERIAL_NUM == 1
  static HardwareSerial LoraSerial(1);
#else
  static HardwareSerial LoraSerial(2);
#endif

// ── MOD GEÇİŞLERİ ────────────────────────────────────────────
static void e22_wait_aux() {
#if LORA_AUX >= 0
    uint32_t t = millis();
    while (digitalRead(LORA_AUX) == LOW) {
        if (millis() - t > 3000) {
            LOG_W(TAG_E22, "AUX timeout!");
            break;
        }
        delay(2);
    }
#else
    delay(100);   // Fixaj PCB — AUX yok, delay ile bekle
#endif
}

static void e22_set_normal_mode() {
    digitalWrite(LORA_M0, LOW);
    digitalWrite(LORA_M1, LOW);
    delay(50);
    e22_wait_aux();
    LOG_D(TAG_E22, "Normal mod");
}

static void e22_set_config_mode() {
    digitalWrite(LORA_M0, HIGH);
    digitalWrite(LORA_M1, HIGH);
    delay(50);
    e22_wait_aux();
    LOG_D(TAG_E22, "Config mod");
}

// ── KONFIGÜRASYON YAZMA ──────────────────────────────────────
// Gereksinim: NFR-6.1, NFR-6.2
// Ebyte E22 konfigürasyon komutu: 9 byte
//   [0xC0][ADDH][ADDL][SPED][CHAN][OPTION1][OPTION2][OPTION3][OPTION4]
// SPED:    bit7-5=UART baud, bit4-3=parity, bit2-0=air rate
// OPTION1: bit7=Relay, bit6=LBT, bit5=WOR, bit4-3=FEC, bit2=PacketRSSI, bit1-0=TXPower
static void e22_configure(uint8_t node_address) {
    e22_set_config_mode();
    delay(100);

    // REG0 (SPED): 9600 baud | 8N1 | 2.4kbps = 0x62
    uint8_t reg0 = 0x62; 
    
    // REG1 (OPTION): SubPacket 240B, RSSI Ambient Enable, TX Power 22dBm = 0x20
    // (Note: Ambient RSSI allows LBT to work)
    uint8_t reg1 = 0x20; 
    
    // REG2 (CHAN): 868.125MHz
    uint8_t reg2 = (uint8_t)LORA_CHANNEL; 
    
    // REG3 (TRANSMISSION MODE): Packet RSSI Enable, Transparent Mode, LBT Enable = 0xA3
    // Transparent mode (bit 6 = 0) is required since your code does not prepend hardware routing bytes!
    uint8_t reg3 = 0xA3; 

    uint8_t cmd[12] = {
        0xC0, 0x00, 0x09,         // Command: Save permanently, Start Addr 0x00, Length 9 bytes
        0x00, node_address, 0x00, // ADDH = 0, ADDL = NodeID, NETID = 0
        reg0, reg1, reg2, reg3,   // The 4 configuration registers
        0x00, 0x00                // CRYPT_H, CRYPT_L (No hardware encryption)
    };

    LoraSerial.write(cmd, sizeof(cmd));
    delay(200);
    e22_wait_aux();

    while (LoraSerial.available()) {
        uint8_t b = LoraSerial.read();
        LOG_D(TAG_E22, "CFG resp: 0x%02X", b);
    }

    e22_set_normal_mode();
    LOG_I(TAG_E22, "Konfigurasyon tamamlandi. Transparent Mod aktif.");
}

// ── KONFIGÜRASYON OKUMA (GET) ─────────────────────────────────
static void e22_print_config() {
    e22_set_config_mode();
    delay(50);

    uint8_t cmd[3] = {0xC1, 0xC1, 0xC1};
    LoraSerial.write(cmd, 3);
    delay(200);

    Serial.println("\n=== E22 Konfigürasyonu ===");
    int idx = 0;
    while (LoraSerial.available()) {
        uint8_t b = LoraSerial.read();
        Serial.printf("  Byte[%d] = 0x%02X (%d)\n", idx++, b, b);
    }
    Serial.println("=========================\n");

    e22_set_normal_mode();
}

// ── GÖNDERME ─────────────────────────────────────────────────
// Gereksinim: NFR-2.5 — AUX LOW iken gönderim yapılmaz
static bool e22_send(const uint8_t *buf, size_t len) {
#if LORA_AUX >= 0
    if (digitalRead(LORA_AUX) == LOW) {
        LOG_W(TAG_E22, "AUX LOW — kanal meşgul, gönderim iptal");
        return false;
    }
#endif
    if (len > 240) {
        LOG_E(TAG_E22, "Frame çok büyük: %d byte", len);
        return false;
    }
    LoraSerial.write(buf, len);
    e22_wait_aux();
    LOG_D(TAG_E22, "Gönderildi: %d byte", len);
    return true;
}

// ── ALMA (RSSI ile) ───────────────────────────────────────────
// Gereksinim: FR-4.2 — Packet RSSI enable olduğunda son byte RSSI
// Dönüş: alınan byte sayısı (0 = yok), rssi_out = RSSI değeri
static int e22_recv(uint8_t *buf, size_t maxLen, int8_t *rssi_out) {
    if (!LoraSerial.available()) return 0;

    int n = 0;
    uint32_t last_byte_time = millis();
    
    // UART çok yavaş (9600 baud) olduğu için, paketin tamamının 
    // buffer'a inmesini beklemeliyiz. Son byte geldikten sonra 
    // 25ms boyunca yeni byte gelmezse paketi bitti sayıyoruz.
    while (millis() - last_byte_time < 25) {
        while (LoraSerial.available() && n < (int)maxLen) {
            buf[n++] = LoraSerial.read();
            last_byte_time = millis(); // Yeni byte geldi, zamanlayıcıyı sıfırla
        }
    }

    if (n == 0) return 0;

    // Packet RSSI: son byte RSSI (-byte = dBm, Ebyte convention)
    // Gerçek RSSI = -buf[n-1]  (ör: 0x50 = -80 dBm)
    if (rssi_out && n > (int)sizeof(FrameHeader)) {
        *rssi_out = -(int8_t)buf[n - 1];
        n--;  // RSSI byte'ını frame dışı tut
    } else if (rssi_out) {
        *rssi_out = -120;
    }

    LOG_D(TAG_E22, "Alındı: %d byte, RSSI=%d dBm", n, rssi_out ? *rssi_out : 0);
    return n;
}

// ── BAŞLATMA ─────────────────────────────────────────────────
static void e22_init(uint8_t node_address) {
    pinMode(LORA_M0, OUTPUT);
    pinMode(LORA_M1, OUTPUT);
#if LORA_AUX >= 0
    pinMode(LORA_AUX, INPUT);
#endif

    // Fixaj PCB: Serial1 özel pinler; diğerleri: Serial2 standart
    LoraSerial.begin(LORA_BAUD, SERIAL_8N1, LORA_TX, LORA_RX);
    delay(100);

    LOG_I(TAG_E22, "E22 başlatılıyor... Node=%d", node_address);
    e22_configure(node_address);
}
