/*
 * PinConfigFuzzer.ino - Safe Hardware Diagnostic Fuzzer for ESP32-S3 Smart Displays
 *
 * This fuzzer is designed to safely discover hardware pin configurations for unknown 
 * ESP32-S3 boards (such as the Guition JC3248W535 variants) without causing CPU crashes.
 *
 * CRITICAL SAFETY NOTE:
 * Standard and Octal (OPI) SPI Flash & PSRAM utilize dedicated pins that will immediately 
 * crash the CPU if reconfigured as general-purpose IO. 
 * This fuzzer strictly avoids:
 *   - GPIO 6-11 (Standard SPI Flash)
 *   - GPIO 26-37 (Octal SPI / OPI PSRAM)
 *
 * Features:
 *   1. Backlight GPIO Fuzzing - Cycles safe pins HIGH one-by-one to identify screen illumination.
 *   2. Touch I2C Bus Scanner - Scans safe SDA/SCL pin combinations to locate the Touch chip (0x3B).
 */

#include <Arduino.h>
#include <Wire.h>

// Safe GPIO pins on ESP32-S3 (omitting boot strapping and Octal SPI Flash/PSRAM pins)
static const int SAFE_PINS[] = {
    0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48
};
static const int NUM_SAFE_PINS = sizeof(SAFE_PINS) / sizeof(SAFE_PINS[0]);

// Common touch screen I2C addresses (AXS15231B is usually 0x3B, CST816S is usually 0x15)
static const uint8_t TOUCH_ADDRESSES[] = {0x3B, 0x15, 0x5D, 0x2C};
static const int NUM_TOUCH_ADDRESSES = sizeof(TOUCH_ADDRESSES) / sizeof(TOUCH_ADDRESSES[0]);

void printMenu() {
    Serial.println();
    Serial.println("==================================================");
    Serial.println("          ESP32-S3 PIN CONFIG FUZZER              ");
    Serial.println("==================================================");
    Serial.println("Select a diagnostic tool:");
    Serial.println("  [1] Fuzz LCD Backlight Pin");
    Serial.println("  [2] Scan I2C Pins for Touch Controller");
    Serial.println("  [3] Run All Tests");
    Serial.println("  [m] Print this Menu");
    Serial.println("==================================================");
    Serial.print("Enter choice (1, 2, 3, m): ");
}

void fuzzBacklight() {
    Serial.println("\n--- STARTING BACKLIGHT PIN FUZZER ---");
    Serial.println("Keep an eye on the display! The moment it lights up, note the GPIO number below.");
    Serial.println("Each pin will be driven HIGH for 2.0 seconds.");
    delay(1000);

    for (int i = 0; i < NUM_SAFE_PINS; i++) {
        int pin = SAFE_PINS[i];
        Serial.printf("[BACKLIGHT] Testing GPIO %d... Toggling HIGH\n", pin);
        
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH);
        
        // Wait 2 seconds while screen is HIGH
        delay(2000);
        
        digitalWrite(pin, LOW);
        pinMode(pin, INPUT); // Revert to floating input to prevent contention
        
        // Brief pause between pins
        delay(200);
    }
    Serial.println("--- BACKLIGHT FUZZER COMPLETE ---");
}

void scanI2CTouch() {
    Serial.println("\n--- STARTING I2C TOUCH PIN SCANNER ---");
    Serial.println("Iterating through all safe SDA/SCL pin combinations to locate touch chips...");
    delay(1000);

    bool foundAny = false;

    for (int i = 0; i < NUM_SAFE_PINS; i++) {
        int sda = SAFE_PINS[i];
        for (int j = 0; j < NUM_SAFE_PINS; j++) {
            int scl = SAFE_PINS[j];
            if (sda == scl) continue; // SDA and SCL cannot be the same pin

            // Release previous bus allocations
            Wire.end();
            
            // Initialise new bus pins
            Wire.begin(sda, scl);
            Wire.setClock(100000); // Standard 100KHz

            // Probe target addresses
            for (int k = 0; k < NUM_TOUCH_ADDRESSES; k++) {
                uint8_t addr = TOUCH_ADDRESSES[k];
                Wire.beginTransmission(addr);
                byte error = Wire.endTransmission(true);

                if (error == 0) {
                    Serial.printf(">>> SUCCESS: Found Touch Chip at 0x%02X! [SDA: GPIO %d, SCL: GPIO %d] <<<\n", addr, sda, scl);
                    foundAny = true;
                }
            }
        }
    }

    if (!foundAny) {
        Serial.println("[I2C SCAN] No touch devices responded on any safe pin configurations.");
    }
    Serial.println("--- I2C SCANNER COMPLETE ---");
}

void setup() {
    Serial.begin(115200);
    // Wait for native USB CDC debug connection
    for (int i = 0; i < 8 && !Serial; i++) {
        delay(250);
    }
    
    Serial.println("\n\n[FUZZER] Diagnostic environment loaded.");
    printMenu();
}

void loop() {
    if (Serial.available() > 0) {
        char ch = Serial.read();
        // Consume any trailing newline/carriage returns
        while (Serial.available() > 0 && (Serial.peek() == '\n' || Serial.peek() == '\r')) {
            Serial.read();
        }

        switch (ch) {
            case '1':
                fuzzBacklight();
                printMenu();
                break;
            case '2':
                scanI2CTouch();
                printMenu();
                break;
            case '3':
                fuzzBacklight();
                scanI2CTouch();
                printMenu();
                break;
            case 'm':
            case 'M':
                printMenu();
                break;
            default:
                Serial.printf("Unknown choice: '%c'\n", ch);
                printMenu();
                break;
        }
    }
    delay(100);
}
