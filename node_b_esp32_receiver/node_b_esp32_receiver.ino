#include <LoRa_E22.h>

#define ESP_RX_PIN 16 
#define ESP_TX_PIN 17 
#define PIN_M0 19     
#define PIN_M1 18     
#define PIN_AUX 4     

LoRa_E22 e22(&Serial2, PIN_AUX, PIN_M0, PIN_M1);

// --- GÖNDERİCİ İLE BİREBİR AYNI ŞABLON ---
struct __attribute__((packed)) LifeNetPacket {
  uint8_t sender_id;      
  uint16_t message_id;    
  uint8_t hop_limit;      
  char payload[40];       
};

void setup() {
  Serial.begin(115200);
  delay(2000); 
  
  Serial.println("\n--- NODE B (ALICI) UYANDI ---");
  Serial2.begin(9600, SERIAL_8N1, ESP_RX_PIN, ESP_TX_PIN);
  delay(500); 

  if (!e22.begin()) {
    Serial.println("HATA: LoRa modulu bulunamadi!");
    while (1); 
  }

  // Ayarları Eşitleme (Aynı Ağ)
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
  Serial.println("Node B havayi yapısal paketler için dinliyor...");
}

void loop() {
  if (e22.available() > 1) {
    // 1. HAVADAN GELEN VERİYİ ZARF BOYUTUNDA OKU
    ResponseStructContainer rsc = e22.receiveMessage(sizeof(LifeNetPacket));
    
    if (rsc.status.code == 1) {
      // 2. GELEN BYTELARI PAKET ŞABLONUNA DÖNÜŞTÜR
      LifeNetPacket* gelenPaket = (LifeNetPacket*) rsc.data;

      // --- MASAÜSTÜ MESAFE SİMÜLASYONU BAŞLANGICI ---
      // Node A paketi 3 ile yollar. 
      // Eğer 3 ile geliyorsa, Node A bana çok uzak, sinyali duymadım varsay:
      if (gelenPaket->hop_limit == 3) {
        // Hiçbir şey yazdırma ve paketi çöpe at
        rsc.close();
        return; 
      }
      // --- SİMÜLASYON BİTİŞİ ---
      
      Serial.println("\n--- YENİ MESH PAKETİ YAKALANDI ---");
      Serial.print("Gönderen ID : "); Serial.println(gelenPaket->sender_id);
      Serial.print("Mesaj ID    : "); Serial.println(gelenPaket->message_id);
      Serial.print("Kalan Sekme : "); Serial.println(gelenPaket->hop_limit);
      Serial.print("Acil Durum  : "); Serial.println(gelenPaket->payload);
      Serial.println("----------------------------------");
      
      // Bellek sızıntısını önlemek için rsc'yi kapat
      rsc.close(); 
    } else {
      Serial.println("Bozuk paket alındı.");
    }
  }
}