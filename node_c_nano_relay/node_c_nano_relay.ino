#include <SoftwareSerial.h>
#include <LoRa_E22.h>

// --- FİXAJ NANO PCB (v4.3+) GERÇEK PİNLERİ ---
#define PIN_RX 3 
#define PIN_TX 4 
#define PIN_M0 7  
#define PIN_M1 6  

SoftwareSerial ss(PIN_RX, PIN_TX);

// Kütüphaneye pinleri vermiyoruz, patron biziz!
LoRa_E22 e22(&ss); 

// --- GÜNCELLENMİŞ HAFİF VERİ PAKETİ ŞABLONU (44 Byte) ---
struct __attribute__((packed)) LifeNetPacket {
  uint8_t sender_id;      
  uint16_t message_id;    
  uint8_t hop_limit;      
  char payload[40];       // Nano'nun RAM'i şişmesin diye 60'tan 40'a düşürdük!
};

const int MAX_SEEN_MESSAGES = 10;
uint16_t seen_messages[MAX_SEEN_MESSAGES];
int seen_index = 0;

bool isMessageSeen(uint16_t msg_id) {
  for (int i = 0; i < MAX_SEEN_MESSAGES; i++) {
    if (seen_messages[i] == msg_id) return true;
  }
  return false;
}

void markMessageSeen(uint16_t msg_id) {
  seen_messages[seen_index] = msg_id;
  seen_index = (seen_index + 1) % MAX_SEEN_MESSAGES; 
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n--- NODE C (FİXAJ NANO RÖLE - SAF DİNLEME MODU) ---");

  // RADYOYU MANUEL OLARAK ZORLA AÇ (M0=0, M1=0)
  pinMode(PIN_M0, OUTPUT);
  pinMode(PIN_M1, OUTPUT);
  digitalWrite(PIN_M0, LOW);
  digitalWrite(PIN_M1, LOW);
  delay(500); 

  ss.begin(9600); 
  e22.begin(); 

  Serial.println("Kutuphane ayar adimi atlandi. Radyo acildi!");
  Serial.println("Fixaj Node C artik havayi dinliyor ve yonlendiriyor...");
}

void loop() {
  if (e22.available() > 1) {
    ResponseStructContainer rsc = e22.receiveMessage(sizeof(LifeNetPacket));
    
    if (rsc.status.code == 1) {
      LifeNetPacket* gelenPaket = (LifeNetPacket*) rsc.data;
      
      if (gelenPaket->sender_id == 3) {
        rsc.close();
        return; 
      }

      if (isMessageSeen(gelenPaket->message_id)) {
        Serial.print("[-] Mesaj ID: "); Serial.print(gelenPaket->message_id);
        Serial.println(" zaten iletildi, cope atiliyor.");
        rsc.close();
        return;
      }

      markMessageSeen(gelenPaket->message_id);

      Serial.println("\n--- YENİ PAKET YAKALANDI ---");
      Serial.print("Gonderen ID : "); Serial.println(gelenPaket->sender_id);
      Serial.print("Mesaj ID    : "); Serial.println(gelenPaket->message_id);
      
      if (gelenPaket->hop_limit > 0) {
        gelenPaket->hop_limit -= 1; 
        
        Serial.print("[+] Sekme Hakki 1 azaltildi. Yeni Hop Limit: ");
        Serial.println(gelenPaket->hop_limit);
        Serial.println("[+] Havaya tekrar basiliyor...");
        
        delay(random(50, 200)); 
        
        e22.sendMessage(gelenPaket, sizeof(LifeNetPacket));
        Serial.println("Yonlendirme Basarili!");
      } else {
        Serial.println("[x] Sekme hakki 0'a ulasti. Yonlendirilmiyor.");
      }
      Serial.println("----------------------------");
      
      rsc.close(); 
    } else {
        rsc.close();
    }
  }
}