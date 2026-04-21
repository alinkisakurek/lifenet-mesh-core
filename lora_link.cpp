#include "lora_link.h"

// PRD Section 1: Hardware Mapping
// Note: This mapping is based on the Fixaj board reference design.
// It must still be verified on actual hardware, especially because
// GPIO35 is typically an input-only pin on standard ESP32 boards.
#define LORA_TX_PIN 27  // ESP32 TX1 (connected to module RX)
#define LORA_RX_PIN 35  // ESP32 RX1 (connected to module TX)
#define LORA_M0_PIN 32
#define LORA_M1_PIN 33

void lora_init() {
    // Configure M0/M1 GPIOs as outputs
    pinMode(LORA_M0_PIN, OUTPUT);
    pinMode(LORA_M1_PIN, OUTPUT);

    // Set M0 = LOW, M1 = LOW for normal data mode (as per PRD)
    digitalWrite(LORA_M0_PIN, LOW);
    digitalWrite(LORA_M1_PIN, LOW);

    // Initialize Serial1 (9600 baud, 8N1)
    // HardwareSerial::begin(baud, config, rxPin, txPin)
    Serial1.begin(9600, SERIAL_8N1, LORA_RX_PIN, LORA_TX_PIN);

    // Small delay after init
    delay(100);
}

bool lora_send_packet(const Packet& p) {
    uint8_t buf[128]; // Max packet size is 19 + 64 + 2 = 85 bytes, 128 is plenty
    
    // Reserve 3 bytes at the start for framing: SOF1 (1), SOF2 (1), LEN (1)
    size_t serialized_len = packet_serialize(p, &buf[3], sizeof(buf) - 3);
    if (serialized_len == 0 || serialized_len > 85) {
        return false; // Serialization failed or invalid length
    }

    buf[0] = 0xAA; // SOF1
    buf[1] = 0x55; // SOF2
    buf[2] = (uint8_t)serialized_len;

    size_t total_len = serialized_len + 3;
    size_t written = Serial1.write(buf, total_len);
    Serial1.flush(); // Wait for transmission to complete
    
    return (written == total_len);
}

enum RxState {
    WAIT_SOF1,
    WAIT_SOF2,
    WAIT_LEN,
    READ_PACKET_BYTES
};

bool lora_receive_packet(Packet& out, uint32_t timeout_ms) {
    uint32_t start_time = millis();
    uint8_t buf[128];
    size_t bytes_read = 0;
    uint8_t expected_len = 0;
    RxState state = WAIT_SOF1;

    while ((millis() - start_time) < timeout_ms) {
        if (Serial1.available()) {
            uint8_t c = Serial1.read();

            switch (state) {
                case WAIT_SOF1:
                    if (c == 0xAA) state = WAIT_SOF2;
                    break;
                case WAIT_SOF2:
                    if (c == 0x55) {
                        state = WAIT_LEN;
                    } else if (c == 0xAA) {
                        state = WAIT_SOF2; // Repeated SOF1
                    } else {
                        state = WAIT_SOF1;
                    }
                    break;
                case WAIT_LEN:
                    // Max packet size is 19 (header) + 64 (payload) + 2 (CRC) = 85 bytes
                    if (c == 0 || c > 85) {
                        state = WAIT_SOF1; // Invalid length, resync
                    } else {
                        expected_len = c;
                        bytes_read = 0;
                        state = READ_PACKET_BYTES;
                    }
                    break;
                case READ_PACKET_BYTES:
                    buf[bytes_read++] = c;
                    if (bytes_read == expected_len) {
                        // Full framed packet received, attempt deserialize
                        if (packet_deserialize(out, buf, expected_len)) {
                            return true;
                        }
                        // If CRC fails, packet is corrupted, resync
                        state = WAIT_SOF1;
                    }
                    break;
            }
        } else {
            // Yield a bit to avoid blocking the OS completely
            delay(1);
        }
    }
    
    return false;
}
