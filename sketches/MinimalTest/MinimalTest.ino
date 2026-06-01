#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    // Wait for Serial to initialize
    for (int i = 0; i < 12 && !Serial; i++) {
        delay(500);
    }
    
    Serial.println("\n\n=== PSRAM DIAGNOSTIC TEST STARTING ===");
    
    // Check if PSRAM is found
    bool psramOk = psramFound();
    Serial.printf("[PSRAM] psramFound() returned: %s\n", psramOk ? "TRUE" : "FALSE");
    
    if (psramOk) {
        size_t freePsram = ESP.getFreePsram();
        size_t totalPsram = ESP.getPsramSize();
        Serial.printf("[PSRAM] Total PSRAM Size: %u bytes (%d KB)\n", totalPsram, totalPsram / 1024);
        Serial.printf("[PSRAM] Free PSRAM Size: %u bytes (%d KB)\n", freePsram, freePsram / 1024);
        
        // Test allocation in PSRAM
        Serial.println("[PSRAM] Testing 307.2 KB contiguous allocation in PSRAM...");
        void* buf = ps_malloc(307200); // 320 * 480 * 2
        if (buf != nullptr) {
            Serial.println("[PSRAM] >>> SUCCESS: Successfully allocated 307.2 KB in PSRAM! <<<");
            free(buf);
        } else {
            Serial.println("[PSRAM] >>> FAILURE: ps_malloc(307200) returned nullptr! <<<");
        }
    } else {
        Serial.println("[PSRAM] WARNING: PSRAM is NOT detected by the ESP32 core!");
        Serial.printf("[SYSTEM] Free Heap: %u bytes\n", ESP.getFreeHeap());
        
        // Test standard malloc
        Serial.println("[SYSTEM] Testing 307.2 KB allocation in standard Heap...");
        void* buf = malloc(307200);
        if (buf != nullptr) {
            Serial.println("[SYSTEM] >>> SUCCESS: Allocated 307.2 KB in standard Heap! <<<");
            free(buf);
        } else {
            Serial.println("[SYSTEM] >>> FAILURE: malloc(307200) failed (expected due to no PSRAM). <<<");
        }
    }
    
    Serial.println("=== DIAGNOSTIC COMPLETE ===");
    
    // Set backlight pins as outputs and blink
    pinMode(1, OUTPUT);
    pinMode(19, OUTPUT);
}

void loop() {
    static bool state = false;
    state = !state;
    
    digitalWrite(1, state ? HIGH : LOW);
    digitalWrite(19, state ? HIGH : LOW);
    
    Serial.printf("Toggling backlight: %s\n", state ? "HIGH" : "LOW");
    delay(1000);
}
