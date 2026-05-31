/*
 * AXS15231B_CanvasTest - Display bring-up / isolation test for the JC3248W535EN
 *
 * Purpose (Issue #40): the main SIP sketch drives the panel through the
 * third-party `JC3248W535EN` library and the screen stays blank. That library
 * exposes no flush() and wraps Arduino_GFX internally, so we cannot influence
 * its init/refresh from the sketch. This standalone sketch removes every other
 * variable (no Wi-Fi, no SIP, no HTTP) and drives the panel with the *proven*
 * raw Arduino_GFX + Arduino_Canvas + flush() stack — the exact pattern from the
 * known-good reference for this board.
 *
 * If THIS renders, the panel/pins/PSRAM are fine and the blank screen is the
 * JC3248W535EN library's config/init — migrate the app sketch onto this stack
 * (see the recipe at the bottom of this file and in the #40 thread).
 * If THIS is also blank, the problem is hardware/board-settings (PSRAM mode,
 * pin mapping, or a dead panel), not software.
 *
 * Board Settings (Arduino IDE):
 *   - Board:        ESP32S3 Dev Module
 *   - PSRAM:        OPI PSRAM   (MANDATORY — the 320x480x16 canvas is 307.2 KB)
 *   - Flash Mode:   QIO 80MHz
 *   - Flash Size:   16MB (128Mb)
 *   - USB CDC On Boot: Enabled
 *
 * Required Library:
 *   - GFX Library for Arduino  (a.k.a. Arduino_GFX, by moononournation)  >= 1.4.x
 */

#include <Arduino_GFX_Library.h>

// ── JC3248W535EN QSPI pin map (verified across reference implementations) ─────
#define TFT_CS   45
#define TFT_SCK  47
#define TFT_D0   21
#define TFT_D1   48
#define TFT_D2   40
#define TFT_D3   39
#define TFT_BL    1   // backlight, active HIGH (some board revisions also use GPIO 19)

// ── Proven display stack ──────────────────────────────────────────────────────
// QSPI bus -> AXS15231B controller -> in-PSRAM Canvas. You draw on `gfx`
// (the canvas); nothing reaches the panel until gfx->flush() DMA-pushes the frame.
Arduino_DataBus *bus        = new Arduino_ESP32QSPI(TFT_CS, TFT_SCK, TFT_D0, TFT_D1, TFT_D2, TFT_D3);
Arduino_GFX     *raw_panel  = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED /*RST*/, 0 /*rotation*/, true /*IPS*/, 320, 480);
Arduino_GFX     *gfx        = new Arduino_Canvas(320, 480, raw_panel, 0, 0);

static uint32_t frame = 0;

void drawTestFrame()
{
    // Everything below draws into the PSRAM canvas; the single flush() at the
    // end is what makes any of it visible.
    gfx->fillScreen(BLACK);

    // Title block
    gfx->fillRect(0, 0, 320, 34, gfx->color565(0, 0, 170));
    gfx->setTextColor(WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(10, 9);
    gfx->print("POCKET-DIAL");

    gfx->setTextSize(1);
    gfx->setTextColor(gfx->color565(85, 255, 255));
    gfx->setCursor(10, 46);
    gfx->print("AXS15231B canvas+flush OK");

    // Primary-color bars — confirms full-width addressing & color order.
    const uint16_t bars[] = { RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, WHITE };
    for (int i = 0; i < 7; i++)
        gfx->fillRect(10 + i * 43, 70, 40, 60, bars[i]);

    // Corner markers — confirms the panel is addressed edge-to-edge (no offset).
    gfx->drawRect(0, 0, 320, 480, WHITE);
    gfx->fillRect(0, 0, 12, 12, RED);             // top-left
    gfx->fillRect(308, 0, 12, 12, GREEN);         // top-right
    gfx->fillRect(0, 468, 12, 12, BLUE);          // bottom-left
    gfx->fillRect(308, 468, 12, 12, YELLOW);      // bottom-right

    // Live counter — confirms repeated flush()es actually refresh the panel.
    gfx->setTextSize(3);
    gfx->setTextColor(gfx->color565(85, 255, 85));
    gfx->setCursor(10, 160);
    gfx->printf("FRAME %lu", (unsigned long)frame);

    gfx->setTextSize(2);
    gfx->setTextColor(WHITE);
    gfx->setCursor(10, 220);
    gfx->printf("uptime %lus", (unsigned long)(millis() / 1000));

    gfx->flush();   // <-- the line the JC3248W535EN-based app sketch is missing
}

void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n[AXS15231B_CanvasTest] Display isolation test (Issue #40)");

    // Backlight up first so a successful render is actually visible.
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    pinMode(19, OUTPUT);          // alternate backlight rail on some revisions
    digitalWrite(19, HIGH);

    Serial.printf("[PSRAM] found=%s  free=%u KB\n",
                  psramFound() ? "YES" : "NO", (unsigned)(ESP.getFreePsram() / 1024));
    if (!psramFound())
        Serial.println("[PSRAM] WARNING: OPI PSRAM not detected — the 307 KB canvas "
                        "alloc will fail. Set Tools > PSRAM = OPI PSRAM.");

    if (!gfx->begin())
    {
        Serial.println("[GFX] FATAL: gfx->begin() returned false. Check PSRAM mode "
                       "and the QSPI pin map. Halting.");
        while (true) { delay(1000); }
    }
    Serial.println("[GFX] begin() OK — drawing test pattern.");

    drawTestFrame();
}

void loop()
{
    frame++;
    drawTestFrame();
    delay(500);   // ~2 fps is plenty to prove repeated flush()es refresh the panel
}

/*
 * ── Migration recipe for SipServer_JC3248W535.ino (Issue #40) ─────────────────
 * Once this sketch renders, port the app off the JC3248W535EN library by giving
 * it a thin wrapper that exposes the same calls it already uses, backed by the
 * stack above:
 *
 *   clear(r,g,b)          -> gfx->fillScreen(gfx->color565(r,g,b));
 *   setColor(r,g,b)       -> remember the colour; apply on the next call
 *   drawFillRect(x,y,w,h) -> gfx->fillRect(x,y,w,h, currentColor);
 *   drawRect(x,y,w,h)     -> gfx->drawRect(x,y,w,h, currentColor);
 *   prt(s,x,y,scale)      -> gfx->setTextSize(scale); gfx->setTextColor(currentColor);
 *                            gfx->setCursor(x,y); gfx->print(s);
 *   ...and call gfx->flush() once at the end of redrawUI()/drawDashboard() and
 *   after each partial update block in loop().
 *
 * Touch (CST816S / AXS integrated, I2C SDA 4 / SCL 8, addr ~0x3B) and QR
 * rendering are NOT provided by Arduino_GFX and must be added separately —
 * track under #43 and #45.
 */
