#pragma once

#include <string>

// Initialize the LVGL HMI switchboard interface.
// Sets up style sheets, containers, buttons, and buffers.
void ui_init(void);

// Transition the screen into Onboarding Setup Mode or the Main Dashboard.
// In Onboarding mode, renders the scannable Wi-Fi QR join code and setup instructions.
void ui_set_onboarding_mode(bool onboarding, const char* ssid = "My-Ap", const char* pass = "12345678");

// Update system telemetry on the retro dashboard in real-time.
void ui_update_status(const std::string& ip, int uptimeSec, int stationNum, int clientCount, int sessionCount);

// Append a live server log line to the scrolling terminal console at the bottom of the screen.
void ui_add_log(const char* line);

// Update battery voltage and percentage indicator in the title bar header.
void ui_set_battery(float volts, int percent);

// Handle low-level capacitive touch coordinate inputs from the AXS15231B touch controller.
void ui_handle_touch_press(int16_t x, int16_t y);
