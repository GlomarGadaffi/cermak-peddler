#pragma once

#include <string>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Pocket-Dial on-device touchscreen UI — the "BBS-glow" live-status WALLBOARD.
//
// The 3.5" 320x480 portrait panel (Guition JC3248W535, AXS15231B QSPI) is no
// longer a configuration surface — ALL config moved to the SSH "sysop terminal."
// This screen is now a passive, always-on, glanceable monitor: the physical-screen
// counterpart of the SSH [1] System Monitor. It shows live calls, the extension
// roster + counts, vitals (uptime / free heap), a reach line ("SSH here to
// configure"), and the operator-log ticker. The only touch interactions left are
// tap-anywhere to cycle the brass/phosphor theme, and — on a standalone board whose
// SoftAP came up secured — press-and-hold to re-show the AP credentials splash.
//
// LVGL v8.4 (NOT v9 — the installed managed component is lvgl/lvgl ^8.3.11). All
// object/style/anim calls below target the v8 API. The UI is built once on the main
// task, then driven from the Core-1 LVGL task and the Core-0 status task; every
// ui_* call MUST be made under the s_lvgl_mux recursive mutex in
// esp_main_display.cpp. The panel runs in FULL_REFRESH mode (see esp_main_display.cpp)
// so there are NO per-tick animations — state changes are discrete and cheap.
// ─────────────────────────────────────────────────────────────────────────────

// Bounds mirror the registrar pools (PoolConfig.hpp: 32 clients / 8 sessions).
#define UI_MAX_EXTENSIONS  32     // registered-extension roster cap (clientPool)
#define UI_MAX_CALLS       8      // live-call list cap (sessionPool)
#define UI_MAX_DND_CHIPS   8      // DND chips shown before "+N"
#define UI_LOG_LINES       4      // visible operator-log ticker rows

// Live-call state, derived on the SIP side from the registrar snapshot. Reuses the
// lamp colours: ACTIVE = hot accent (◆), RINGING = brass highlight (◐).
enum UiCallState {
    UI_CALL_RINGING = 0,   // Invited  -> ◐ ringing (brass highlight)
    UI_CALL_ACTIVE         // Connected -> ◆ active  (hot accent — the "glow")
};

// One live call row (caller ext -> destination). Fixed-size POD: copied into the
// snapshot with no heap churn on the polling tick.
struct UiCall {
    char a[12];          // caller extension
    char b[12];          // destination extension
    uint8_t state;       // UiCallState
    int     durationSec; // connected duration (0 while ringing)
};

// One registered extension (roster). `ext[0]=='\0'` marks an empty slot. POD.
struct UiExt {
    char ext[12];
    bool inCall;
    bool dnd;
};

// Whole-board snapshot handed from the SIP poller (Core 0) to the UI. The caller
// fills only the first callCount/extCount/dndCount entries; the UI bounds-checks
// all three. Plain POD so it can be stack-built in system_status_task and copied
// in under the LVGL mutex.
struct UiBoardSnapshot {
    UiCall calls[UI_MAX_CALLS];
    int    callCount;
    UiExt  exts[UI_MAX_EXTENSIONS];
    int    extCount;
    char   dnd[UI_MAX_DND_CHIPS][12];
    int    dndCount;
    int    dndOverflow;   // extra DND extensions beyond UI_MAX_DND_CHIPS (shown as "+N")
};

// Initialize the LVGL wallboard (header, reach line, LIVE CALLS hero, roster/counts,
// vitals, operator-log ticker, and the minimal first-boot onboarding/splash overlay).
void ui_init(void);

// Transition between first-boot Onboarding/Splash and the live wallboard. In
// onboarding mode a brand splash + a scannable Wi-Fi join QR cover the board, and
// configuration continues in the web dashboard at http://pocketdial.local/.
// (An earlier revision of this comment said config was "SSH-only afterward"; the
// SSH sysop terminal was deleted outright rather than hardened — see
// docs/THREAT_MODEL.md E-3 — so HTTP is the only admin surface.)
//
// `ssid`/`pass` are REQUIRED when `onboarding` is true: they are rendered on the
// glass and encoded into the join QR, so they must be the credentials the SoftAP
// actually came up with. They previously defaulted to "My-Ap"/"12345678" — the
// same hardcoded passphrase that made the onboarding AP's WPA2 decorative, since
// it was identical on every unit and published in this repo. The defaults are
// gone so that literal cannot quietly come back; pass DeviceConfig::getApPsk().
void ui_set_onboarding_mode(bool onboarding, const char* ssid = nullptr, const char* pass = nullptr);

// Show the standalone SoftAP's SSID + WPA2 passphrase (and a scannable Wi-Fi join
// QR) as a timed splash IN FRONT OF the live wallboard. Call it only when the AP
// actually came up secured — there is nothing to show for an open AP.
//
// Unlike ui_set_onboarding_mode(true) this does NOT put the UI into onboarding
// state: standalone AP is the normal operating mode, so the wallboard keeps
// updating underneath and the splash stands aside by itself after ~90 s, or on a
// tap. A long press anywhere on the wallboard brings it back, which is the only
// thing that keeps the passphrase recoverable on a board whose dashboard sits
// behind the very AP the operator cannot yet join.
//
// `ssid`/`pass` are REQUIRED and have no defaults, for the same reason
// ui_set_onboarding_mode's were removed: no plausible-looking placeholder may ever
// stand in for a real credential. They are copied internally, so the caller's
// buffers need not outlive the call.
void ui_show_ap_credentials(const char* ssid, const char* pass);

// Update the header (uptime clock + online lamp), the reach line (host + IP), the
// roster counts strip (EXT n/32 · CALLS n/8) and the vitals strip (uptime / free
// heap %). Mirrors the SSH monitor's vitals content. freeHeapPct < 0 => "—".
void ui_update_status(const std::string& ip, int uptimeSec, int stationNum,
                      int clientCount, int sessionCount, int freeHeapPct = -1);

// Repaint the LIVE CALLS list + roster + DND chips from a fresh registrar snapshot.
// Only rows/chips whose content actually changed are touched, so the full-refresh
// frame work stays minimal. Safe to call every poll. This is the "glow" — it lights
// up the instant a call is placed.
void ui_update_board(const UiBoardSnapshot& snap);

// Append one line to the operator-log ticker (append-only, bounded ring).
void ui_add_log(const char* line);

// Header aux indicator (kept for source compatibility; folded into the header).
void ui_set_battery(float volts, int percent);

// Low-level capacitive-touch coordinate router (AXS15231B). Wallboard is passive:
// the gestures are tap-anywhere to cycle the brass/phosphor theme, press-and-hold to
// re-show the AP credentials splash on a secured standalone board, and a tap on that
// splash to dismiss it. All three are routed by LVGL's own hit-testing, so nothing
// is dispatched from here; kept so esp_main_display.cpp keeps linking.
void ui_handle_touch_press(int16_t x, int16_t y);
