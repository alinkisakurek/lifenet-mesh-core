#include <LoRa_E22.h>

#define ESP_RX_PIN 16 
#define ESP_TX_PIN 17 
#define PIN_M0 25     
#define PIN_M1 26     
#define PIN_AUX 4     

LoRa_E22 e22(&Serial2, PIN_AUX, PIN_M0, PIN_M1);

// --- LIFE NET VERİ PAKETİ ŞABLONU ---
// 'packed' komutu ESP32 (32-bit) ile Nano'nun (8-bit) bu paketi aynı boyutta görmesini sağlar
struct __attribute__((packed)) LifeNetPacket {
  uint8_t sender_id;      // Gönderen Cihaz (Node A = 1)
  uint16_t message_id;    // Benzersiz Mesaj Numarası
  uint8_t hop_limit;      // Kalan Sekme Hakkı
  char payload[40];       // Asıl Mesaj (Maksimum 60 harf)
};

void setup() {
  Serial.begin(115200);
  delay(2000); 
  
  Serial.println("\n--- NODE A (GÖNDERİCİ) UYANDI ---");
  Serial2.begin(9600, SERIAL_8N1, ESP_RX_PIN, ESP_TX_PIN);
  delay(500); 

  if (!e22.begin()) {
    Serial.println("HATA: LoRa modulu bulunamadi!");
    while (1); 
  }

  // Ayarları Eşitleme
  ResponseStructContainer c;
  c = e22.getConfiguration();
  Configuration configuration = *(Configuration*) c.data;
  
  configuration.ADDL = 0x01;  
  configuration.ADDH = 0x00;  
  configuration.NETID = 0x00; 
  configuration.CHAN = 23; 
  configuration.SPED.uartBaudRate = UART_BPS_9600; 
  configuration.SPED.airDataRate = AIR_DATA_RATE_010_24; 
  configuration.TRANSMISSION_MODE.fixedTransmission = FT_TRANSPARENT_TRANSMISSION; 
  
  e22.setConfiguration(configuration, WRITE_CFG_PWR_DWN_SAVE);
  Serial.println("Modul ayarlari yapildi. Paketler hazirlaniyor...");
}

void loop() {
  static unsigned long lastSend = 0;
  static uint16_t msgCounter = 100; // Mesaj ID'leri 100'den başlasın
  
  if (millis() - lastSend > 4000) { // Her 4 saniyede bir
    lastSend = millis();
    msgCounter++;

    // 1. ZARFI (PAKETİ) OLUŞTUR VE İÇİNİ DOLDUR
    LifeNetPacket paket;
    paket.sender_id = 1;               // Benim ID'm (Node A)
    paket.message_id = msgCounter;     // Bu mesajın benzersiz numarası
    paket.hop_limit = 3;               // Bu paket 3 kere sekebilir
    
    // Metni payload içine güvenle kopyala
    String acilMesaj = "Enkaz altindayim, konum A-Blok!";
    strncpy(paket.payload, acilMesaj.c_str(), sizeof(paket.payload) - 1);
    paket.payload[sizeof(paket.payload) - 1] = '\0'; // Metin sonu garantisi

    // 2. ZARFI HAVAYA FIRLAT (Byte seviyesinde gönderim)
    Serial.print("Gönderiliyor -> Mesaj ID: ");
    Serial.println(paket.message_id);
    
    ResponseStatus rs = e22.sendMessage(&paket, sizeof(LifeNetPacket));
    Serial.println(rs.getResponseDescription());
  }
}