// Tui.cpp — pocket-dial ANSI sysop-terminal TUI (foundation, B2b-1).
//
// Implements docs/design/{brand,tui-style,tui-ia,accessibility}.md: the brass/
// phosphor operator board reached over SSH. See Tui.hpp for the architecture.
//
// Platform note: this file is host-compilable. The ONLY platform-gated bit is the
// uptime fallback (esp_timer); everything else — ANSI emission, palette, glyphs,
// component geometry, input decode, screen router — is pure C++17. The guard
// matches the pattern used across src/ (ESP_PLATFORM||ESP32||ARDUINO).

#include "Tui.hpp"
#include "SipSecretStore.hpp"   // single audited CSPRNG secret generator ([4]/[D]/[G])

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include "esp_timer.h"
#include "esp_system.h"   // esp_restart() for the [R] reboot confirm
#include "nvs_flash.h"    // nvs_flash_erase() for the [4]/[X] factory reset
#include "nvs.h"          // nvs_open/set_u8 for the [2]/[M] wifi_mode switch
#endif

// ── Width constants (the 80×24 floor; brand §6.3 "80×24 is the floor") ────────
static constexpr int kCols = 80;   // content frame width (cols 1..80)

// CDR ring depth shown in the [5] REPORTS "n/32" headroom. Mirrors the SIP-side
// POCKETDIAL_CDR_RECORDS default (CallDetailRecord.hpp) WITHOUT including that SIP
// header — Tui is transport-/SIP-agnostic by construction. If the SIP ring depth is
// re-tuned, update this label-only constant to match (it does not size any buffer).
static constexpr int POCKETDIAL_CDR_RECORDS_UI = 32;

// ─────────────────────────────────────────────────────────────────────────────
// ANSI primitives
// ─────────────────────────────────────────────────────────────────────────────

void Tui::put(const char* s)
{
    if (_write && s) _write(s, std::strlen(s));
}
void Tui::put(const std::string& s)
{
    if (_write) _write(s.data(), s.size());
}
void Tui::putn(const char* s, size_t n)
{
    if (_write && s) _write(s, n);
}

void Tui::clearScreen()
{
    // Full clear + cursor home. Used only on screen TRANSITIONS and Ctrl-L
    // (line-noise recovery) — never in steady-state live repaint (renderer
    // contract tui-style §6.4).
    put("\x1b[2J\x1b[H");
}
void Tui::home()        { put("\x1b[H"); }
void Tui::hideCursor()  { put("\x1b[?25l"); }
void Tui::showCursor()  { put("\x1b[?25h"); }
void Tui::sgrReset()    { if (_color) put("\x1b[0m"); }

void Tui::moveTo(int row, int col)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
    put(buf);
}

void Tui::sgr(const char* code)
{
    if (!_color || !code) return;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "\x1b[%sm", code);
    put(buf);
}

// ── Palette: semantic role → xterm-16 SGR, per theme (tui-style §1.2/1.3/1.4) ──
// BRASS: text=33(yellow) header/lamp=93(br-yellow) dim=90 dnd=93(amber) alert=31.
// PHOSPHOR: text=32(green) header/lamp=92(br-green) dim=90 dnd=93(amber, KEPT)
//           alert=31. Border stays brass(33) in BOTH themes (rails are brass —
//           brand §5). DND amber + alert red are theme-invariant by design.
const char* Tui::sgrFor(Role r) const
{
    const bool phos = (_theme == Theme::Phosphor);
    switch (r)
    {
        case Role::Border: return "33";                 // brass rails, both themes
        case Role::Text:   return phos ? "32" : "33";
        case Role::Header: return phos ? "92" : "93";
        case Role::Lamp:   return phos ? "92" : "93";
        case Role::Dim:    return "90";                  // grey, both themes
        case Role::Dnd:    return "93";                  // amber, both themes
        case Role::Alert:  return "31";                  // red, both themes
    }
    return "0";
}

void Tui::roled(Role r, const std::string& text)
{
    if (_color) sgr(sgrFor(r));
    put(text);
    if (_color) put("\x1b[0m");
}

// ─────────────────────────────────────────────────────────────────────────────
// Glyph table — one indirection so the mono/ASCII degradation is free.
// Fallbacks per brand §3.3 + tui-style §6.2:
//   ●→(*) ○→( ) ◐→(~) ◆→<*> ⊘→[/] ↳→-> ▲→/!\  ◆READY→<*>
//   ╔→+ ╗→+ ╚→+ ╝→+ ═→= ║→|  ├→+ ┤→+   ▸→>  (◉)→(o)  ▌▐→|
// In unicode mode we use the box-drawing set from the mockups (┌┐└┘─│├┤ for
// panels; ╔╗╚╝═║ for the banner nameplate — handled inline in renderBanner).
// ─────────────────────────────────────────────────────────────────────────────
const char* Tui::glyph(Glyph g) const
{
    if (_unicode)
    {
        switch (g)
        {
            case Glyph::Online:    return "\xe2\x97\x8f"; // ●
            case Glyph::Unreach:   return "\xe2\x97\x8b"; // ○
            case Glyph::Ringing:   return "\xe2\x97\x90"; // ◐
            case Glyph::Active:    return "\xe2\x97\x86"; // ◆
            case Glyph::Dnd:       return "\xe2\x8a\x98"; // ⊘
            case Glyph::Fwd:       return "\xe2\x86\xb3"; // ↳
            case Glyph::Alert:     return "\xe2\x96\xb2"; // ▲
            case Glyph::Ready:     return "\xe2\x97\x86"; // ◆ (READY shares ◆)
            case Glyph::BoxTL:     return "\xe2\x94\x8c"; // ┌
            case Glyph::BoxTR:     return "\xe2\x94\x90"; // ┐
            case Glyph::BoxBL:     return "\xe2\x94\x94"; // └
            case Glyph::BoxBR:     return "\xe2\x94\x98"; // ┘
            case Glyph::BoxH:      return "\xe2\x94\x80"; // ─
            case Glyph::BoxV:      return "\xe2\x94\x82"; // │
            case Glyph::BoxVR:     return "\xe2\x94\x9c"; // ├
            case Glyph::BoxVL:     return "\xe2\x94\xa4"; // ┤
            case Glyph::Marker:    return "\xe2\x96\xb8"; // ▸
            case Glyph::Sigil:     return "(\xe2\x97\x89)\xe2\x94\x80\xe2\x96\xb6"; // (◉)─▶
            case Glyph::JackDot:   return "(\xe2\x97\x89)"; // (◉)
            case Glyph::WordmarkL: return "\xe2\x96\x8c\xe2\x96\x90"; // ▌▐
            case Glyph::WordmarkR: return "\xe2\x96\x90\xe2\x96\x8c"; // ▐▌
        }
    }
    switch (g)   // ASCII fallback tier
    {
        case Glyph::Online:    return "(*)";
        case Glyph::Unreach:   return "( )";
        case Glyph::Ringing:   return "(~)";
        case Glyph::Active:    return "<*>";
        case Glyph::Dnd:       return "[/]";
        case Glyph::Fwd:       return "->";
        case Glyph::Alert:     return "/!\\";
        case Glyph::Ready:     return "<*>";
        case Glyph::BoxTL:     return "+";
        case Glyph::BoxTR:     return "+";
        case Glyph::BoxBL:     return "+";
        case Glyph::BoxBR:     return "+";
        case Glyph::BoxH:      return "-";
        case Glyph::BoxV:      return "|";
        case Glyph::BoxVR:     return "+";
        case Glyph::BoxVL:     return "+";
        case Glyph::Marker:    return ">";
        case Glyph::Sigil:     return "(o)->";
        case Glyph::JackDot:   return "(o)";
        case Glyph::WordmarkL: return "|";
        case Glyph::WordmarkR: return "|";
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
// Small helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string Tui::fmtClock(uint64_t sec)
{
    // Title-bar clock is HH:MM:SS. We have no wall-clock RTC guarantee, so the
    // spine shows uptime in HH:MM:SS form (hours can exceed 24 — proof of life,
    // narrative.md "uptime = quiet proof"). Clamp hours to 2 digits min.
    unsigned h = static_cast<unsigned>((sec / 3600ULL) % 100ULL);
    unsigned m = static_cast<unsigned>((sec / 60ULL) % 60ULL);
    unsigned s = static_cast<unsigned>(sec % 60ULL);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);
    return std::string(buf);
}

Tui::LiveStats Tui::stats() const
{
    if (_stats) return _stats();
    LiveStats z;   // zeros, but a correct spine (host fallback / unattached)
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    z.uptimeSec = static_cast<uint64_t>(esp_timer_get_time() / 1000000LL);
#endif
    return z;
}

Tui::MonitorSnapshot Tui::monitor() const
{
    if (_monitor) return _monitor();
    MonitorSnapshot z;   // all-idle, correct spine (unattached / host fallback)
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    z.uptimeSec = static_cast<uint64_t>(esp_timer_get_time() / 1000000LL);
#endif
    return z;
}

Tui::ReportsSnapshot Tui::reports() const
{
    if (_reports) return _reports();
    return ReportsSnapshot{};   // empty CDR — an honest "no calls yet" view
}

Tui::SecurityInfo Tui::security() const
{
    if (_security) return _security();
    return SecurityInfo{};      // safe defaults (unprovisioned, SSH on)
}

Tui::PbxConfigSnapshot Tui::pbxConfig() const
{
    if (_pbxConfig) return _pbxConfig();
    return PbxConfigSnapshot{}; // empty-but-honest panel (no rows, correct caps)
}

Tui::RegistrarInfo Tui::registrar() const
{
    if (_registrar) return _registrar();
    return RegistrarInfo{};     // Open mode + empty device list (honest default)
}

// Build the §3.7.2 forward-target list: every registered extension, then an
// explicit "clear" sentinel. The tokens are what setForward() stores: a bare
// extension or "" (clear). Ring groups are intentionally excluded — see the note
// at the (removed) group loop below: the SIP layer cannot yet resolve a
// "grp:<name>" forward target, so offering it would be dishonest.
std::vector<Tui::PickEntry>
Tui::buildForwardPickList(const PbxConfigSnapshot& cfg) const
{
    std::vector<PickEntry> out;
    for (const auto& e : cfg.extensions)
    {
        PickEntry p;
        p.token = e.ext;
        p.label = e.ext + (e.name.empty() ? "" : (" " + e.name));
        p.state = e.state;
        p.isGroup = false;
        out.push_back(std::move(p));
    }
    // NOTE: ring groups are deliberately NOT offered as forward targets. The SIP
    // layer (redirectInvite→findClient) does not unwrap a "grp:<name>" token, so a
    // forward-to-ring-group would silently no-op at call time. Offering it here
    // would advertise a capability that does not work. Forward-to-ring-group
    // routing is a tracked follow-up; until the SIP layer resolves group tokens,
    // the picker offers only real extensions plus the explicit clear sentinel.
    // The explicit "clear this forward" sentinel (§3.7.1 F5): token "" = unset.
    PickEntry clr;
    clr.token = "";
    clr.label = "(clear this forward)";
    clr.isGroup = false;
    out.push_back(std::move(clr));
    return out;
}

std::string Tui::themeLabel() const
{
    // Footer ALWAYS names the active theme by label (brand §5 — never by hue).
    // The ▸ is brand chrome, not state.
    std::string s = "Theme: ";
    s += (_theme == Theme::Brass) ? "BRASS" : "PHOSPHOR";
    s += " ";
    s += glyph(Glyph::Marker);   // ▸ (or > in ASCII)
    return s;
}

void Tui::setSize(uint16_t cols, uint16_t rows)
{
    _cols = cols ? cols : 80;
    _rows = rows ? rows : 24;
}

// Emit `text` then pad with spaces to a full 80-col line + CRLF. `vis` is the
// visible width of `text` (callers track it because multibyte glyphs and SGR make
// strlen() wrong). Used for centered body lines on the placeholder/confirm spine.
void Tui::centerLine(const std::string& text, int vis)
{
    int pad = kCols - vis;
    if (pad < 0) pad = 0;
    put(text);
    std::string sp(static_cast<size_t>(pad), ' ');
    put(sp);
    put("\r\n");
}

// True display width in terminal COLUMNS (not bytes). Walks the string skipping
// ANSI SGR runs (ESC '[' … 'm') and counting one column per UTF-8 code point.
// Every glyph this TUI emits is single-column in xterm, so a code-point count is
// exact; a UTF-8 continuation byte (0b10xxxxxx) is NOT a new code point and is
// not counted. This makes the hand-tracked `vis` counters redundant — any framed
// row can be padded with (panelWidth - dispWidth(body)) and the right rail lands
// flush regardless of how many multibyte glyphs the row carries.
int Tui::dispWidth(const std::string& s)
{
    int cols = 0;
    for (size_t i = 0; i < s.size(); )
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1b)   // ESC — skip a CSI/SGR sequence "ESC [ … <final>"
        {
            size_t j = i + 1;
            if (j < s.size() && s[j] == '[')   // CSI introducer
            {
                ++j;
                // Parameter/intermediate bytes, terminated by a final byte in
                // 0x40-0x7e (e.g. 'm' for SGR, 'H' for cursor — none visible).
                while (j < s.size())
                {
                    unsigned char d = static_cast<unsigned char>(s[j]);
                    ++j;
                    if (d >= 0x40 && d <= 0x7e) break;   // CSI final byte
                }
            }
            i = j;
            continue;
        }
        // A new UTF-8 code point starts on any byte that is NOT a continuation
        // byte (continuation bytes are 0b10xxxxxx → 0x80..0xbf). One column each.
        if ((c & 0xc0) != 0x80) ++cols;
        ++i;
    }
    return cols;
}

// ─────────────────────────────────────────────────────────────────────────────
// Component: title bar (row 1) — the brand spine.  tui-style §2.1:
//   ┌────────────────────────────────────────────────────────────────────┐
//   │ POCKET-DIAL v3.0   [ MODE ]                              HH:MM:SS    │
//   ├────────────────────────────────────────────────────────────────────┤
// The clock is the only ⟳ live cell on this row. Content stays within 78 cols
// with a frame at col 80 (brand §6.3 right-margin rule).
// ─────────────────────────────────────────────────────────────────────────────
void Tui::drawSpineTop(const char* mode, const LiveStats& st)
{
    const std::string h = glyph(Glyph::BoxH);   // ─ or -
    const std::string v = glyph(Glyph::BoxV);   // │ or |

    // Top rule:  ┌──…──┐   (78 horizontals between corners → 80 wide)
    {
        std::string line = glyph(Glyph::BoxTL);
        for (int i = 0; i < kCols - 2; ++i) line += h;
        line += glyph(Glyph::BoxTR);
        roled(Role::Border, line);
        put("\r\n");
    }

    // Title row:  │ POCKET-DIAL v3.0   [ MODE ]                    HH:MM:SS │
    // Build the inner 78-col content with exact spacing then frame it.
    std::string brand = "POCKET-DIAL " + st.fw.substr(0, 4); // "v3.0" (vMAJOR.MINOR)
    if (brand.size() < 12) brand = "POCKET-DIAL v3.0";
    // Use the locked vMAJOR.MINOR human form regardless of patch in st.fw:
    brand = "POCKET-DIAL v3.0";
    std::string modeTag = std::string("[ ") + mode + " ]";
    std::string clock   = fmtClock(st.uptimeSec);

    // Inner width = 78 (cols 2..79). Layout: " " + brand + gap + modeTag + gap +
    // clock + " ". We compute the two gaps so the clock ends flush at col 79.
    const int inner = kCols - 2;                 // 78
    int used = 1 /*lead sp*/ + (int)brand.size() + 3 /*sep*/ + (int)modeTag.size()
             + (int)clock.size() + 1 /*trail sp*/;
    int fill = inner - used;
    if (fill < 1) fill = 1;

    // Emit framed, colored title row piece by piece.
    roled(Role::Border, v);
    put(" ");
    roled(Role::Text, brand);
    put("   ");
    roled(Role::Header, modeTag);
    put(std::string((size_t)fill, ' '));
    roled(Role::Text, clock);
    put(" ");
    roled(Role::Border, v);
    put("\r\n");

    // Separator rule:  ├──…──┤
    {
        std::string line = glyph(Glyph::BoxVR);
        for (int i = 0; i < kCols - 2; ++i) line += h;
        line += glyph(Glyph::BoxVL);
        roled(Role::Border, line);
        put("\r\n");
    }
}

// Component: key-hint footer (row 24).  tui-style §2.2. Always ends in the theme
// label. We draw the separator rule, the keys line, then the bottom rule.
void Tui::drawFooter(const std::string& keys)
{
    const std::string h = glyph(Glyph::BoxH);
    const std::string v = glyph(Glyph::BoxV);

    // Separator above footer.
    {
        std::string line = glyph(Glyph::BoxVR);
        for (int i = 0; i < kCols - 2; ++i) line += h;
        line += glyph(Glyph::BoxVL);
        roled(Role::Border, line);
        put("\r\n");
    }

    // Footer content: │ <keys> … <padding> <Theme: X ▸> │
    std::string tl = themeLabel();
    // Visible width of the theme label measured from the built label itself — the ▸
    // is 1 column though 3 bytes, and the BRASS label ("Theme: BRASS ▸" = 14 cols)
    // is narrower than PHOSPHOR, so a fixed worst-case width left BRASS footers 3
    // cols short. Route through dispWidth() like every other framed row.
    int tlVis = dispWidth(tl);
    const int inner = kCols - 2;      // 78
    // Budget for the keys field: the inner width less the lead space, the theme
    // label, the trail space, and a minimum 1-col gap. If the keys don't fit, we
    // truncate them (dispWidth-aware, never splitting a UTF-8 sequence or a CSI
    // run) rather than clamping the fill and overrunning the 80-col frame — no
    // footer can silently break the rail. (All shipping footers fit; this is a
    // guard for future edits.)
    const int keysBudget = inner - 1 /*lead*/ - tlVis - 1 /*trail*/ - 1 /*min gap*/;
    std::string keysOut = keys;       // mutable copy we may truncate
    int keysVis = dispWidth(keysOut); // DISPLAY columns — keys may hold arrows (←/→/↑/↓)
    if (keysVis > keysBudget && keysBudget >= 0)
    {
        std::string clipped;
        int cols = 0;
        for (size_t i = 0; i < keysOut.size(); )
        {
            unsigned char c = static_cast<unsigned char>(keysOut[i]);
            if (c == 0x1b)            // copy an entire CSI/SGR run as one unit (0 cols)
            {
                size_t j = i + 1;
                if (j < keysOut.size() && keysOut[j] == '[')
                {
                    ++j;
                    while (j < keysOut.size())
                    {
                        unsigned char d = static_cast<unsigned char>(keysOut[j]);
                        ++j;
                        if (d >= 0x40 && d <= 0x7e) break;
                    }
                }
                clipped.append(keysOut, i, j - i);
                i = j;
                continue;
            }
            // Measure a whole UTF-8 code point (1 lead byte + continuation bytes).
            size_t j = i + 1;
            while (j < keysOut.size() &&
                   (static_cast<unsigned char>(keysOut[j]) & 0xc0) == 0x80) ++j;
            if (cols + 1 > keysBudget) break;     // would exceed budget — stop whole
            clipped.append(keysOut, i, j - i);
            ++cols;
            i = j;
        }
        keysOut.swap(clipped);
        keysVis = cols;
    }
    int fill = inner - 1 /*lead*/ - keysVis - tlVis - 1 /*trail*/;
    if (fill < 1) fill = 1;

    roled(Role::Border, v);
    put(" ");
    roled(Role::Text, keysOut);
    put(std::string((size_t)fill, ' '));
    roled(Role::Header, tl);
    put(" ");
    roled(Role::Border, v);
    put("\r\n");

    // Bottom rule:  └──…──┘
    {
        std::string line = glyph(Glyph::BoxBL);
        for (int i = 0; i < kCols - 2; ++i) line += h;
        line += glyph(Glyph::BoxBR);
        roled(Role::Border, line);
        // No trailing CRLF — keep the cursor at the bottom-right so a follow-up
        // prompt repositions cleanly.
    }
}

// Component: status chip — glyph + LABEL wrapped in the role's accent SGR, then
// reset (tui-style §1.4 renderer rule: wrap ONLY the glyph+label, then ESC[0m).
void Tui::statusChip(Glyph g, Role r, const char* label)
{
    if (_color) sgr(sgrFor(r));
    put(glyph(g));
    put(" ");
    put(label);
    if (_color) put("\x1b[0m");
}

// Map a status-lexicon State → its (glyph, role, label) in ONE place so the
// monitor never re-decides per call site (brand §4.5: glyph + label, color last).
void Tui::appendStateChip(State s, std::string& out, int& vis,
                          const char* labelOverride)
{
    // Initialized defaults (UNREACH/dim) so GCC sees no uninitialized path even
    // though every enumerator is handled below.
    Glyph g = Glyph::Unreach; Role r = Role::Dim; const char* label = "UNREACH";
    switch (s)
    {
        case State::Online:  g = Glyph::Online;  r = Role::Lamp; label = "ONLINE";  break;
        case State::Unreach: g = Glyph::Unreach; r = Role::Dim;  label = "UNREACH"; break;
        case State::Ringing: g = Glyph::Ringing; r = Role::Header; label = "RINGING"; break;
        case State::Active:  g = Glyph::Active;  r = Role::Lamp; label = "ACTIVE";  break;
        case State::Dnd:     g = Glyph::Dnd;     r = Role::Dnd;  label = "DND";     break;
        case State::Idle:    g = Glyph::Unreach; r = Role::Dim;  label = "idle";    break;
    }
    if (labelOverride) label = labelOverride;
    if (_color) out += std::string("\x1b[") + sgrFor(r) + "m";
    out += glyph(g);                 vis += (_unicode ? 1 : 3);
    out += " ";                      vis += 1;
    out += label;                    vis += (int)std::strlen(label);
    if (_color) out += "\x1b[0m";
}

int Tui::stateChipVis(State s, const char* labelOverride) const
{
    const char* label = labelOverride ? labelOverride : "UNREACH";
    if (!labelOverride)
    {
        switch (s)
        {
            case State::Online:  label = "ONLINE";  break;
            case State::Unreach: label = "UNREACH"; break;
            case State::Ringing: label = "RINGING"; break;
            case State::Active:  label = "ACTIVE";  break;
            case State::Dnd:     label = "DND";     break;
            case State::Idle:    label = "idle";    break;
        }
    }
    return (_unicode ? 1 : 3) + 1 + (int)std::strlen(label);
}

// Component: 10-cell vitals bar (tui-style §2.10). "[██████░░░░] 61%" — the % is
// authoritative; the bar reinforces (fill=accent lamp, empty=dim). ASCII fallback
// uses '#'/'.' (the §2.10 map). Appends a colored bar+label to `out`.
void Tui::appendVitalsBar(int pct, std::string& out, int& vis)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (pct + 5) / 10;            // round to nearest 10% cell
    if (filled > 10) filled = 10;
    const char* full  = _unicode ? "\xe2\x96\x88" : "#";   // █ / #
    const char* empty = _unicode ? "\xe2\x96\x91" : ".";   // ░ / .
    out += "[";                              vis += 1;
    if (_color) out += std::string("\x1b[") + sgrFor(Role::Lamp) + "m";
    for (int i = 0; i < filled; ++i) { out += full; vis += 1; }
    if (_color) out += "\x1b[0m";
    if (_color) out += std::string("\x1b[") + sgrFor(Role::Dim) + "m";
    for (int i = filled; i < 10; ++i) { out += empty; vis += 1; }
    if (_color) out += "\x1b[0m";
    out += "]";                              vis += 1;
    char b[8]; std::snprintf(b, sizeof(b), " %d%%", pct);
    if (_color) out += std::string("\x1b[") + sgrFor(Role::Text) + "m";
    out += b;                                vis += (int)std::strlen(b);
    if (_color) out += "\x1b[0m";
}

// ─────────────────────────────────────────────────────────────────────────────
// Screen: LOGIN BANNER (brand §3.2 / tui-style §3.1)
// The canonical nameplate, descriptor sub-rule, identity block, house-rules
// notice, in a 78-wide ╔══╗ frame. We render the ╔═╗║╚╝ nameplate directly (it is
// banner-specific chrome distinct from the panel ┌┐ set) with its ASCII fallback
// (+ = |) when !_unicode. Accessibility §2.6: in the ASCII/no-color tier we still
// render it (it's brand), but it degrades losslessly to the §3.3 mono form.
// ─────────────────────────────────────────────────────────────────────────────
void Tui::renderBanner()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();

    // Box-drawing pieces (banner uses the DOUBLE rule for the brass rail).
    const char* TL = _unicode ? "\xe2\x95\x94" : "+";   // ╔
    const char* TR = _unicode ? "\xe2\x95\x97" : "+";   // ╗
    const char* BL = _unicode ? "\xe2\x95\x9a" : "+";   // ╚
    const char* BR = _unicode ? "\xe2\x95\x9d" : "+";   // ╝
    const char* HH = _unicode ? "\xe2\x95\x90" : "=";   // ═
    const char* VV = _unicode ? "\xe2\x95\x91" : "|";   // ║
    const char* SUB= _unicode ? "\xe2\x94\x80" : "-";   // ─ (sub-rule inside)

    const int W = 78;            // frame width (≤78 content, margin at 80 — §3.1)
    const int innerW = W - 2;    // 76 columns between the ║ rails

    auto rule = [&](const char* l, const char* mid, const char* r) {
        if (_color) sgr(sgrFor(Role::Border));
        put(" ");                 // 1-col lead margin (mockup indents the frame)
        put(l);
        for (int i = 0; i < innerW; ++i) put(mid);
        put(r);
        if (_color) put("\x1b[0m");
        put("\r\n");
    };
    // A framed content row: " ║ <body padded to innerW> ║". The visible column
    // width is measured from `body` itself (dispWidth skips SGR + counts glyphs as
    // 1 col), so the right rail lands flush no matter how many multibyte glyphs the
    // body carries. `bodyVis` is ignored (kept so existing call sites compile); the
    // measured width is authoritative — this is the border-alignment fix.
    auto row = [&](const std::string& body, int /*bodyVis*/) {
        if (_color) sgr(sgrFor(Role::Border));
        put(" ");
        put(VV);
        if (_color) put("\x1b[0m");
        // body is emitted by the caller already colored; here we only pad.
        put(body);
        int pad = innerW - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        put(VV);
        if (_color) put("\x1b[0m");
        put("\r\n");
    };
    auto blank = [&]() { row("", 0); };

    // The FIGlet nameplate (brand §2.1) — 5 art lines, each ≤ innerW. These are
    // pure ASCII art so they need no fallback; they are brass-colored chrome.
    static const char* art[5] = {
        "   ____   ___   ____ _  _______ _____      ____  ___    _    _",
        "  |  _ \\ / _ \\ / ___| |/ / ____|_   _|    |  _ \\|_ _|  / \\  | |",
        "  | |_) | | | | |   | ' /|  _|   | |_____ | | | || |  / _ \\ | |",
        "  |  __/| |_| | |___| . \\| |___  | |_____|| |_| || | / ___ \\| |___",
        "  |_|    \\___/ \\____|_|\\_\\_____| |_|      |____/|___/_/   \\_\\_____|",
    };

    rule(TL, HH, TR);
    blank();
    for (int i = 0; i < 5; ++i)
    {
        std::string b;
        if (_color) b += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        b += "   ";                       // indent the art inside the frame
        b += art[i];
        if (_color) b += "\x1b[0m";
        row(b, 3 + (int)std::strlen(art[i]));
    }
    blank();

    // Descriptor sub-rule line:  ◖▌ S Y S O P  T E R M I N A L ▐◗  single-board…
    {
        std::string body;
        std::string deco = _unicode ? "\xe2\x97\x96\xe2\x96\x8c" : "[#"; // ◖▌ / [#
        std::string decoR = _unicode ? "\xe2\x96\x90\xe2\x97\x97" : "]"; // ▐◗ / ]
        int vis = 0;
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Header) + "m";
        body += "   "; vis += 3;
        body += deco;  vis += _unicode ? 2 : 2;
        body += "  S Y S O P   T E R M I N A L  "; vis += 31;
        body += decoR; vis += _unicode ? 2 : 1;
        if (_color) body += "\x1b[0m";
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        body += "     single-board SIP PBX"; vis += 24;
        if (_color) body += "\x1b[0m";
        row(body, vis);
    }

    // Inner sub-rule (single ─ line inside the double frame).
    {
        std::string body;
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Border) + "m";
        body += " ";
        for (int i = 0; i < innerW - 2; ++i) body += SUB;
        body += " ";
        if (_color) body += "\x1b[0m";
        row(body, innerW);
    }

    // Identity block (runtime fields). Fixed-width labels so colons align (§3.1).
    auto idLine = [&](const char* label, const std::string& value,
                      bool readyLamp) {
        std::string body;
        int vis = 0;
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        body += "   "; vis += 3;
        body += label; vis += (int)std::strlen(label);
        std::string dot = _unicode ? " \xc2\xb7 " : " . ";  // · / .
        body += dot; vis += 3;
        body += value; vis += (int)value.size();
        if (_color) body += "\x1b[0m";
        // The READY lamp shares the HOST line ONLY when PROVISIONED — the sanctioned
        // "◆ READY — operator on duty" fits the 78-col box. The UN-provisioned line
        // (README Reconciled #4) is far longer and would wrap/break the right rail, so
        // it is rendered on its own dedicated line below HOST (see the caller).
        if (readyLamp && st.provisioned)
        {
            int gap = 40 - vis; if (gap < 2) gap = 2;
            body += std::string((size_t)gap, ' '); vis += gap;
            if (_color) body += std::string("\x1b[") + sgrFor(Role::Lamp) + "m";
            body += glyph(Glyph::Ready); vis += _unicode ? 1 : 3;
            body += " READY"; vis += 6;
            if (_color) body += "\x1b[0m";
            if (_color) body += std::string("\x1b[") + sgrFor(Role::Text) + "m";
            const std::string dash = _unicode ? " \xe2\x80\x94 " : " - ";   // — em dash
            body += dash; vis += 3;
            body += "operator on duty"; vis += 16;
            if (_color) body += "\x1b[0m";
        }
        row(body, vis);
    };

    // Banner branch (README reconciled #4): provisioned vs un-provisioned (the lamp
    // text is selected inside idLine() from st.provisioned).
    idLine("HOST", st.host, true);
    if (!st.provisioned)
    {
        // UN-provisioned box: the locked first-run lamp line (README Reconciled #4) is
        // too long to share the HOST line, so it gets its own full line below HOST.
        std::string body;
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Lamp) + "m";
        body += "   ";
        body += glyph(Glyph::Ready);
        body += " READY";
        if (_color) body += "\x1b[0m";
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        body += _unicode ? " \xe2\x80\x94 " : " - ";   // — em dash
        body += "UNPROVISIONED";
        body += _unicode ? " \xc2\xb7 " : " . ";       // · mid-dot
        body += "first SSH session starts setup";
        if (_color) body += "\x1b[0m";
        const int lampVis = 3 + (_unicode ? 1 : 3) + 6 + 3 + 13 + 3 + 30;
        row(body, lampVis);
    }
    idLine("ADDR", st.ip + (st.mac.empty() ? "" : ("  " + (_unicode ?
            std::string("\xc2\xb7") : std::string(".")) + "  " + st.mac)), false);
    idLine("FW  ", std::string("POCKET-DIAL ") + st.fw, false);

    // Inner sub-rule again.
    {
        std::string body;
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Border) + "m";
        body += " ";
        for (int i = 0; i < innerW - 2; ++i) body += SUB;
        body += " ";
        if (_color) body += "\x1b[0m";
        row(body, innerW);
    }

    // House-rules notice (two lines, operator-terse).
    {
        std::string body;
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        body += "   Authorized sysops only. All sessions are logged to the CDR.";
        if (_color) body += "\x1b[0m";
        row(body, 3 + 60);
    }
    {
        std::string body;
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        body += "   Press  ?  any time for help.   Esc backs out.   This is an ESP32-S3.";
        if (_color) body += "\x1b[0m";
        row(body, 3 + 70);
    }
    blank();
    rule(BL, HH, BR);

    // The greeting line + prompt sigil that lands us into the hub. Per the ticket,
    // the banner is a BRIEF greeting; we cue the operator to continue into the hub.
    put("\r\n ");
    if (_color) sgr(sgrFor(Role::Text));
    put(glyph(Glyph::Sigil));   // (◉)─▶
    put(" Press any key to continue\xe2\x80\xa6");  // …  (ellipsis)
    if (!_unicode) { /* … already emitted as 3-byte UTF-8; harmless on dumb */ }
    if (_color) put("\x1b[0m");
    put("\r\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Screen: MASTER HUB (tui-style §3.2 / tui-ia §1)
// ─────────────────────────────────────────────────────────────────────────────
void Tui::renderHub()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();

    drawSpineTop("SYSTEM MANAGEMENT", st);

    const std::string v = glyph(Glyph::BoxV);
    // A blank framed body row.
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    // A framed body row from a pre-built colored body. Width is measured from the
    // body (dispWidth skips SGR, counts glyphs as 1 col) so the right rail stays
    // flush; the caller's `vis` is ignored (kept for source compatibility).
    auto bodyRow = [&](const std::string& body, int /*vis*/) {
        roled(Role::Border, v);
        put(" ");                                  // 1-col inner lead
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };

    bodyBlank();

    // The numbered hub matrix (tui-style §2.4). [n] digits in bright header,
    // names in brass text. Two rows of three + the R/L system row.
    auto hubItem = [&](const char* num, const char* name, std::string& body,
                       int& vis) {
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Header) + "m";
        body += num; vis += (int)std::strlen(num);     // "[1]"
        if (_color) body += "\x1b[0m";
        body += " "; vis += 1;
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        body += name; vis += (int)std::strlen(name);
        if (_color) body += "\x1b[0m";
    };

    {   // row: [1] SYSTEM MONITOR   [2] NETWORK   [3] PBX CONFIG
        std::string body; int vis = 0;
        body += "    "; vis += 4;
        hubItem("[1]", "SYSTEM MONITOR", body, vis);
        body += "        "; vis += 8;
        hubItem("[2]", "NETWORK", body, vis);
        body += "          "; vis += 10;
        hubItem("[3]", "PBX CONFIG", body, vis);
        bodyRow(body, vis);
    }
    {   // row: [4] SECURITY   [5] REPORTS/LOGS   [6] ABOUT
        std::string body; int vis = 0;
        body += "    "; vis += 4;
        hubItem("[4]", "SECURITY", body, vis);
        body += "              "; vis += 14;
        hubItem("[5]", "REPORTS/LOGS", body, vis);
        body += "     "; vis += 5;
        hubItem("[6]", "ABOUT", body, vis);
        bodyRow(body, vis);
    }
    bodyBlank();
    {   // row: [R] REBOOT   [L] LOGOUT
        std::string body; int vis = 0;
        body += "    "; vis += 4;
        hubItem("[R]", "REBOOT", body, vis);
        body += "     "; vis += 5;
        hubItem("[L]", "LOGOUT", body, vis);
        bodyRow(body, vis);
    }
    bodyBlank();

    // Headroom / status line (⟳ live; tui-style §3.2). REAL data:
    //   ● n ONLINE   ○ n UNREACH   ·   n/8 calls   ·   ext n/32   ·   AP mode
    {
        std::string body; int vis = 0;
        body += "  "; vis += 2;
        // ● n ONLINE
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Lamp) + "m";
        body += glyph(Glyph::Online); vis += _unicode ? 1 : 3;
        if (_color) body += "\x1b[0m";
        { char b[32]; std::snprintf(b, sizeof(b), " %d ONLINE", st.online);
          if (_color) body += std::string("\x1b[") + sgrFor(Role::Text) + "m";
          body += b; vis += (int)std::strlen(b); if (_color) body += "\x1b[0m"; }
        body += "   "; vis += 3;
        // ○ n UNREACH (dim)
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Dim) + "m";
        body += glyph(Glyph::Unreach); vis += _unicode ? 1 : 3;
        { char b[32]; std::snprintf(b, sizeof(b), " %d UNREACH", st.unreachable);
          body += b; vis += (int)std::strlen(b); }
        if (_color) body += "\x1b[0m";
        // · counts · (brass)
        std::string dot = _unicode ? "\xc2\xb7" : ".";
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        { char b[64];
          std::snprintf(b, sizeof(b), "    %s    %d/%d calls    %s    ext %d/%d",
                        dot.c_str(), st.activeCalls, st.maxCalls,
                        dot.c_str(), st.extCount, st.maxExt);
          body += b;
          // visible width: the · is 2 bytes but 1 col under unicode.
          vis += (int)std::strlen(b) - (_unicode ? 2 : 0); }
        { const char* modeWord = (st.netMode == 1) ? "STATION"
                               : (st.netMode == 0) ? "SETUP" : "AP";
          char b[32]; std::snprintf(b, sizeof(b), "    %s   %s", dot.c_str(), modeWord);
          body += b; vis += (int)std::strlen(b) - (_unicode ? 1 : 0); }
        if (_color) body += "\x1b[0m";
        bodyRow(body, vis);
    }
    bodyBlank();

    // Prompt sigil + select line (brand §3.4). (◉)─▶ Select an option: _
    {
        std::string body; int vis = 0;
        body += "  "; vis += 2;
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        body += glyph(Glyph::Sigil); vis += _unicode ? 5 : 5;   // (◉)─▶ ≈5 cols
        body += " Select an option: "; vis += 19;
        if (_color) body += "\x1b[0m";
        bodyRow(body, vis);
    }

    // Pad the body out so the footer lands on row 24 (the spine is fixed height).
    // Geometry: 3 title rows + 18 body rows + 3 footer rows = 24 (mockup §3.2).
    // Body rows so far: blank, 2 matrix, blank, R/L, blank, headroom, blank,
    // prompt = 9. Add 9 blanks to reach 18.
    for (int i = 0; i < 9; ++i) bodyBlank();

    drawFooter("[1-6] Go  [R] Reboot  [L] Logout  [T] Theme  [?] Help");

    // Cache the clock so repaintHubLive() only rewrites it when it changes.
    _lastClock = fmtClock(st.uptimeSec);
}

// Repaint ONLY the live cells on the hub (clock at row 1, headroom at its row) by
// cursor positioning — never a full clear (brief §6 / renderer contract §6.4).
// B2b-1: the session loop is input-driven, so this is invoked opportunistically
// from tickLive(); the 1 Hz monitor screen is a later increment.
void Tui::repaintHubLive()
{
    if (_screen != Screen::Hub) return;
    LiveStats st = stats();
    std::string clock = fmtClock(st.uptimeSec);
    if (clock == _lastClock) return;     // nothing changed → emit nothing
    _lastClock = clock;
    // The clock occupies the 8 cols ending at col 79 on row 1. Position and
    // overwrite in place (brass text), then restore. We do not move the visible
    // cursor parking spot meaningfully — the hub hides the cursor anyway.
    moveTo(1, 71);                       // col 71..78 = "HH:MM:SS"
    if (_color) sgr(sgrFor(Role::Text));
    put(clock);
    if (_color) put("\x1b[0m");
}

// ─────────────────────────────────────────────────────────────────────────────
// Screen: HELP overlay (tui-style §3.14). Hub-scoped help for B2b-1: it lists the
// hub keys and the state-key lexicon. Drawn full-frame (a true centered overlay
// over a dimmed body arrives with the modal shell in a later increment; the spine
// + key list satisfy the "? help on every screen / Esc dismisses" contract now).
// ─────────────────────────────────────────────────────────────────────────────
void Tui::renderHelp()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();

    // Context help is scoped to the screen it was opened FROM (tui-ia §3): the title
    // bar carries that screen's [ MODE ] tag and the body lists only that screen's
    // keys. _helpReturn is set in gotoScreen() when routing into Help.
    const char* mode  = "SYSTEM MANAGEMENT";
    const char* title = "Help \xc2\xb7 Master Hub";   // · is 2 bytes / 1 col
    switch (_helpReturn)
    {
        case Screen::Monitor:     mode = "MONITOR";     title = "Help \xc2\xb7 System Monitor"; break;
        case Screen::Placeholder: mode = "SYSTEM MANAGEMENT"; title = "Help \xc2\xb7 Coming soon"; break;
        case Screen::Network:     mode = "NETWORK";  title = "Help \xc2\xb7 Network"; break;
        case Screen::Security:    mode = "SECURITY"; title = "Help \xc2\xb7 Security"; break;
        case Screen::ChangePin:   mode = "SECURITY"; title = "Help \xc2\xb7 Change PIN"; break;
        case Screen::Devices:     mode = "SECURITY"; title = "Help \xc2\xb7 Registrar / Devices"; break;
        case Screen::RegModePick: mode = "SECURITY"; title = "Help \xc2\xb7 Registrar mode"; break;
        case Screen::SecretEntry: mode = "SECURITY"; title = "Help \xc2\xb7 SIP secret"; break;
        case Screen::DeviceForget: mode = "SECURITY"; title = "Help \xc2\xb7 Forget device"; break;
        case Screen::Reports:     mode = "REPORTS";  title = "Help \xc2\xb7 Reports / Logs"; break;
        case Screen::CdrDetail:   mode = "REPORTS";  title = "Help \xc2\xb7 Call detail"; break;
        case Screen::About:       mode = "ABOUT";    title = "Help \xc2\xb7 About"; break;
        case Screen::PbxConfig:   mode = "PBX CONFIG"; title = "Help \xc2\xb7 PBX Config"; break;
        case Screen::PbxAddMenu:
        case Screen::PbxAddSingle:
        case Screen::PbxAddRange: mode = "PBX CONFIG"; title = "Help \xc2\xb7 Add extension"; break;
        case Screen::PbxGroupEdit:
        case Screen::PbxGroupDelete: mode = "PBX CONFIG"; title = "Help \xc2\xb7 Ring group"; break;
        case Screen::PbxForwardEdit:
        case Screen::PbxForwardPick: mode = "PBX CONFIG"; title = "Help \xc2\xb7 Forwards / DND"; break;
        case Screen::PbxIvrEdit:  mode = "PBX CONFIG"; title = "Help \xc2\xb7 IVR digit"; break;
        default:                  break;   // Hub (and any other) → Master Hub help
    }
    drawSpineTop(mode, st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    // text is emitted at col 4; width is MEASURED via dispWidth (the optional `vis`
    // is ignored, kept for source compat) so multibyte glyphs keep the rail flush.
    auto bodyText = [&](const std::string& text, int vis = -1) {
        (void)vis;
        roled(Role::Border, v);
        put("   ");
        roled(Role::Text, text);
        int pad = (kCols - 2) - 3 - dispWidth(text);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };

    bodyBlank();
    {
        roled(Role::Border, v); put("   ");
        roled(Role::Header, title);
        int pad = (kCols - 2) - 3 - dispWidth(title);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v); put("\r\n");
    }
    bodyBlank();

    // Screen-scoped key list. `bodyUsed` tracks emitted body rows so the trailing
    // blanks always pad the body to exactly 18 (3 title + 18 body + 3 footer = 24)
    // regardless of how many keys this screen lists.
    int bodyUsed = 2;   // the blank + title + blank above
    auto line = [&](const std::string& t, int vis = -1) { bodyText(t, vis); ++bodyUsed; };
    auto gap  = [&]() { bodyBlank(); ++bodyUsed; };

    if (_helpReturn == Screen::Monitor)
    {
        line("F .......... freeze / unfreeze the 1 Hz refresh");
        line("C .......... clear stale / torn-down rows");
        line("Esc ........ back to the main hub");
        line("? .......... this help        Ctrl-L ... redraw screen");
    }
    else if (_helpReturn == Screen::Network)
    {
        line("M .......... switch network mode  [guarded \xe2\x80\x94 reboots]", 49);
        line("Esc ........ back to the main hub");
        line("? .......... this help        Ctrl-L ... redraw screen");
    }
    else if (_helpReturn == Screen::Security)
    {
        line("P .......... change the admin PIN  (never echoed)");
        line("K .......... toggle SSH access on / off");
        line("X .......... factory reset  [guarded \xe2\x80\x94 double-confirm]", 50);
        line("Esc ........ back to the main hub");
        line("? .......... this help        Ctrl-L ... redraw screen");
    }
    else if (_helpReturn == Screen::ChangePin)
    {
        line("Tab ........ next field  (Current / New / Confirm)");
        line("0-9 ........ type a digit  (shown as \xe2\x80\xa2, never echoed)");
        line("Backspace .. edit the focused field");
        line("Enter ...... apply the change   Esc ... cancel");
    }
    else if (_helpReturn == Screen::Reports)
    {
        line("Tab ........ flip view  (Recent Calls / Event Log)");
        line(_unicode ? "\xe2\x86\x91/\xe2\x86\x93 ........ select a call row" : "Up/Dn ...... select a call row");
        line("Enter ...... open the call-detail modal");
        line("Esc ........ back to the main hub");
        line("? .......... this help        Ctrl-L ... redraw screen");
    }
    else if (_helpReturn == Screen::CdrDetail)
    {
        line(_unicode ? "\xe2\x86\x91/\xe2\x86\x93 ........ walk records" : "Up/Dn ...... walk records");
        line("Esc ........ back to the call list");
        line("? .......... this help        Ctrl-L ... redraw screen");
    }
    else if (_helpReturn == Screen::About)
    {
        line("This screen is the honesty card: hardware, firmware,");
        line("capacity caps, and what is NOT here (voicemail v2, no SD).");
        line("Esc ........ back to the main hub");
        line("? .......... this help        Ctrl-L ... redraw screen");
    }
    else if (_helpReturn == Screen::Placeholder)
    {
        line("Esc ........ back to the main hub");
        line("? .......... this help        Ctrl-L ... redraw screen");
    }
    else if (_helpReturn == Screen::PbxConfig)
    {
        line(_unicode ? "\xe2\x86\x90/\xe2\x86\x92 / Tab .. move between the five tabs" : "Left/Right . move between the five tabs");
        line(_unicode ? "\xe2\x86\x91/\xe2\x86\x93 ........ select a row" : "Up/Dn ...... select a row");
        line("Enter ...... edit the selected row (forwards / group)");
        line("A / D ...... add / delete  (Ring Groups)   Space ... DND");
        line("Esc ........ back to the main hub");
        line("? .......... this help        Ctrl-L ... redraw screen");
    }
    else if (_helpReturn == Screen::PbxGroupEdit ||
             _helpReturn == Screen::PbxForwardEdit ||
             _helpReturn == Screen::PbxForwardPick ||
             _helpReturn == Screen::PbxAddMenu ||
             _helpReturn == Screen::PbxAddSingle ||
             _helpReturn == Screen::PbxAddRange ||
             _helpReturn == Screen::PbxIvrEdit ||
             _helpReturn == Screen::PbxGroupDelete)
    {
        line("Tab ........ next field        Space ... toggle / open");
        line(_unicode ? "\xe2\x86\x91/\xe2\x86\x93 ........ move within a list" : "Up/Dn ...... move within a list");
        line(_unicode ? "\xe2\x86\x90/\xe2\x86\x92 ........ radio / button choice" : "Left/Right . radio / button choice");
        line("Enter ...... apply        Esc ... cancel (no write)");
        line("? .......... this help        Ctrl-L ... redraw screen");
    }
    else   // Hub-scoped help
    {
        line("1 .......... SYSTEM MONITOR");
        line("2 .......... NETWORK");
        line("3 .......... PBX CONFIG");
        line("4 .......... SECURITY");
        line("5 .......... REPORTS / LOGS");
        line("6 .......... ABOUT");
        line("R .......... reboot the board  [guarded \xe2\x80\x94 confirms first]", 47);
        line("L .......... log out of this SSH session");
        line("T .......... toggle theme  (BRASS / PHOSPHOR)");
        line("Esc ........ back  (no-op on the hub \xe2\x80\x94 you are home)", 44);
        line("? .......... this help        Ctrl-L ... redraw screen");
    }
    gap();
    // State key — restate the glyph+label lexicon (accessibility: label is
    // authoritative, color removable). Common to every scope.
    {
        std::string s = "State key:  ";
        int vis = 12;
        s += glyph(Glyph::Online);  s += " ONLINE  ";  vis += (_unicode ? 1 : 3) + 9;
        s += glyph(Glyph::Unreach); s += " UNREACH  "; vis += (_unicode ? 1 : 3) + 10;
        s += glyph(Glyph::Dnd);     s += " DND  ";     vis += (_unicode ? 1 : 3) + 6;
        s += glyph(Glyph::Fwd);     s += " FWD  ";     vis += (_unicode ? 2 : 2) + 6;
        s += glyph(Glyph::Alert);   s += " ALERT";     vis += (_unicode ? 1 : 3) + 6;
        line(s, vis);
    }

    // Pad the body to 18 rows so the footer lands on row 24 for every scope.
    for (int i = bodyUsed; i < 18; ++i) bodyBlank();

    // Footer key hint names the close action; the [ MODE ] tag above already tells
    // the operator which screen Esc returns to.
    drawFooter("[Esc] Close help  [?] Close");
}

// ─────────────────────────────────────────────────────────────────────────────
// Screen: REBOOT confirm (tui-style §3.13). The one guarded-action shell. Safe
// default ([ Stay up ]) pre-focused. Honors [←/→]/Enter/y/n/Esc.
// ─────────────────────────────────────────────────────────────────────────────
void Tui::renderRebootConfirm()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("SYSTEM MANAGEMENT", st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };

    // Centered confirm box, 58 wide (tui-style §3.13 geometry), indented to center.
    const int boxW = 58;
    const int indent = (kCols - 2 - boxW) / 2;       // ≈10
    const char* TL = glyph(Glyph::BoxTL);
    const char* TR = glyph(Glyph::BoxTR);
    const char* BL = glyph(Glyph::BoxBL);
    const char* BR = glyph(Glyph::BoxBR);
    const char* VR = glyph(Glyph::BoxVR);
    const char* VL = glyph(Glyph::BoxVL);
    const std::string H = glyph(Glyph::BoxH);

    auto boxFrame = [&](bool top, bool sep) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        if (sep) put(VR); else put(top ? TL : BL);
        // Top rule carries the box title:  "┌─ Confirm ──…──┐"
        int used = 0;
        if (top) { put(H); put(" Confirm "); used = 1 + 9; }   // H + " Confirm "
        for (int i = 0; i < boxW - 2 - used; ++i) put(H);
        if (sep) put(VL); else put(top ? TR : BR);
        if (_color) put("\x1b[0m");
        int pad = (kCols - 2) - indent - boxW;
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto boxLine = [&](const std::string& inner, int /*vis*/) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        put(" ");
        put(inner);
        int ipad = (boxW - 2) - 1 - dispWidth(inner);   // measured, not trusted
        if (ipad < 0) ipad = 0;
        put(std::string((size_t)ipad, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        int pad = (kCols - 2) - indent - boxW;
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };

    bodyBlank();
    bodyBlank();
    boxFrame(true, false);
    {   // ▲ ALERT   REBOOT now?
        std::string inner; int vis = 0;
        if (_color) inner += std::string("\x1b[") + sgrFor(Role::Alert) + "m";
        inner += glyph(Glyph::Alert); vis += _unicode ? 1 : 3;
        inner += " ALERT"; vis += 6;
        if (_color) inner += "\x1b[0m";
        if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += "   REBOOT now?"; vis += 14;
        if (_color) inner += "\x1b[0m";
        boxLine(inner, vis);
    }
    boxLine("", 0);
    boxLine("Phones drop for ~8 seconds while the board", 42);
    boxLine("restarts. Calls in progress will end.", 37);
    boxLine("", 0);
    {   // < Reboot >        [ Stay up ]   — safe default focused (reverse).
        std::string inner; int vis = 0;
        inner += "   "; vis += 3;
        // Unfocused destructive button: plain "< Reboot >".
        if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += "< Reboot >"; vis += 10;
        if (_color) inner += "\x1b[0m";
        inner += "        "; vis += 8;
        // Focused safe button: reverse video "[ Stay up ]".
        if (_color) inner += "\x1b[7m";
        inner += "[ Stay up ]"; vis += 11;
        if (_color) inner += "\x1b[0m";
        boxLine(inner, vis);
    }
    boxFrame(false, true);   // separator rule before the key line
    boxLine("[\xe2\x86\x90/\xe2\x86\x92] Choose   [Enter] Confirm   y/N   [Esc] Cancel",
            _unicode ? 44 : 46);
    boxFrame(false, false);

    // Geometry: 3 title + 18 body + 3 footer = 24. Body so far: 2 blanks +
    // top-frame + ALERT + blank + 2 text + blank + buttons + sep-frame + keys +
    // bottom-frame = 12. Add 6 blanks to reach 18.
    for (int i = 0; i < 6; ++i) bodyBlank();

    drawFooter("[\xe2\x86\x90/\xe2\x86\x92] Choose  [Enter] Confirm  [Esc] Cancel");
}

// ─────────────────────────────────────────────────────────────────────────────
// Screen: PLACEHOLDER for hub items 1-6 not yet built (B2b-1). Centered
// "‹SCREEN› — coming soon. [Esc] back" on the real spine.
// ─────────────────────────────────────────────────────────────────────────────
void Tui::renderPlaceholder()
{
    static const char* kModes[7] = {
        "", "MONITOR", "NETWORK", "PBX CONFIG", "SECURITY", "REPORTS", "ABOUT"
    };
    static const char* kNames[7] = {
        "", "SYSTEM MONITOR", "NETWORK", "PBX CONFIG", "SECURITY",
        "REPORTS / LOGS", "ABOUT"
    };
    int idx = (_placeholderItem >= 1 && _placeholderItem <= 6)
              ? _placeholderItem : 1;

    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop(kModes[idx], st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    // Centered framed text row. Visible width is measured from the body so the
    // right rail stays flush; the caller's `vis` is ignored (kept for compat).
    auto centered = [&](const std::string& colored, int /*vis*/) {
        roled(Role::Border, v);
        int total = kCols - 2;
        int w = dispWidth(colored);
        int left = (total - w) / 2;
        if (left < 0) left = 0;
        int right = total - left - w;
        if (right < 0) right = 0;
        put(std::string((size_t)left, ' '));
        put(colored);
        put(std::string((size_t)right, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };

    for (int i = 0; i < 9; ++i) bodyBlank();   // vertical center

    {
        std::string c; int vis = 0;
        if (_color) c += std::string("\x1b[") + sgrFor(Role::Header) + "m";
        c += kNames[idx]; vis += (int)std::strlen(kNames[idx]);
        if (_color) c += "\x1b[0m";
        if (_color) c += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        std::string dash = _unicode ? " \xe2\x80\x94 " : " - ";
        c += dash; vis += 3;
        c += "coming soon."; vis += 12;
        if (_color) c += "\x1b[0m";
        centered(c, vis);
    }
    bodyBlank();
    {
        std::string c; int vis = 0;
        if (_color) c += std::string("\x1b[") + sgrFor(Role::Dim) + "m";
        c += "[Esc] back"; vis += 10;
        if (_color) c += "\x1b[0m";
        centered(c, vis);
    }

    // Geometry: 3 title + 18 body + 3 footer = 24. Body: 9 + "coming soon" + blank
    // + "[Esc] back" + 6 = 18.
    for (int i = 0; i < 6; ++i) bodyBlank();

    drawFooter("[Esc] Back  [?] Help");
}

// ─────────────────────────────────────────────────────────────────────────────
// Screen: [1] SYSTEM MONITOR — the live wallboard (tui-style §3.3).
// Live-call matrix (§2.9) + registration roster + vitals bars (§2.10). The STATUS/
// DUR matrix cells, the roster chips, and the vitals + UP clock are the ⟳ live
// region; renderMonitor() draws the whole frame once, repaintMonitorLive() then
// overwrites only those cells ~1 Hz by cursor positioning (no full clear).
// [F] freezes the refresh; [C] clears stale rows; [P] is intentionally UNbound.
// ─────────────────────────────────────────────────────────────────────────────

// Render one call-matrix row's content (CH EXT DEST DUR CODEC STATUS) into `out`
// with the fixed column geometry from §2.9. Used by both the full draw and the
// live repaint so the columns never drift. Returns the visible width.
// Columns:  CH(2) ' '*2 EXT(4) ' '*2 DEST(14) ' '*2 DUR(6) ' '*2 CODEC(5) ' '*2 STATUS
static std::string fmtDur(int sec)
{
    if (sec <= 0) return "\xe2\x80\x94";   // — (em dash) for idle; ASCII caller folds
    int m = sec / 60, s = sec % 60;
    if (m > 99) m = 99;                    // DUR column is MM:SS (6 wide) — clamp
    char b[16]; std::snprintf(b, sizeof(b), "%02d:%02d", m, s);
    return std::string(b);
}

// UTF-8-safe truncate `s` to at most `cols` display columns, never splitting a
// multibyte code point. SGR runs are not expected in the raw cell strings passed
// here (color is applied AFTER padding), so this walks plain UTF-8: it advances by
// whole code points and stops once `cols` columns have been emitted. Using
// printf's "%-.Ns" instead would count BYTES and could cut mid-sequence, producing
// an invalid partial code point that corrupts the row and breaks dispWidth().
static std::string truncCols(const std::string& s, int cols)
{
    std::string out;
    int w = 0;
    for (size_t i = 0; i < s.size() && w < cols; )
    {
        size_t start = i;
        ++i;   // lead byte
        while (i < s.size() &&
               (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) ++i;  // continuations
        out.append(s, start, i - start);
        ++w;
    }
    return out;
}

// Format `s` into exactly `cols` DISPLAY columns: UTF-8-safe truncate, then pad on
// the right with spaces measured by dispWidth (not byte length). This is the single
// column formatter the live-call matrix routes EXT/DEST/DUR/CODEC through so a
// multibyte glyph (em-dash "—" in DUR, arrow "→" in DEST) can never shift the
// columns the way printf's byte-counting "%-Ns" did.
static std::string padCols(const std::string& s, int cols)
{
    std::string t = truncCols(s, cols);
    int pad = cols - Tui::dispWidth(t);
    if (pad < 0) pad = 0;
    t.append(static_cast<size_t>(pad), ' ');
    return t;
}

void Tui::renderMonitor()
{
    clearScreen();
    hideCursor();
    drawMonitorFrame();
}

// Emit the whole monitor frame from the CURRENT cursor position (no clear). The
// full draw homes via clearScreen(); the 1 Hz repaint homes via home(). Every row
// is a full 80-col line, so rewriting from the top leaves no residue and avoids the
// ESC[2J flicker the brief bans in steady state. Row padding is measured with
// dispWidth(body) so multibyte glyphs never push the right rail (no hand-counts).
void Tui::drawMonitorFrame()
{
    MonitorSnapshot ms = monitor();
    LiveStats st = stats();
    st.uptimeSec = ms.uptimeSec;
    drawSpineTop("MONITOR", st);

    const std::string v = glyph(Glyph::BoxV);
    const std::string dot = _unicode ? "\xc2\xb7" : ".";

    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    // Framed body row. Width is measured from the pre-colored body via dispWidth
    // (skips SGR, counts each glyph as 1 col) so the right rail is always flush.
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    // Wrap a string in a role's SGR (no-op when color is off).
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };

    // Row 4: LIVE CALLS  …  n/8 active  ·  ⟳ 1 Hz   (badge is a live cell).
    {
        std::string left = col(Role::Header, " LIVE CALLS");
        char act[24];
        std::snprintf(act, sizeof(act), "%d/%d active", ms.activeCalls, ms.maxCalls);
        std::string badge = _monFrozen
            ? (_unicode ? "\xe2\x9d\x9a\xe2\x9d\x9a FROZEN" : "|| FROZEN")   // ❚❚
            : (_unicode ? "\xe2\x9f\xb3 1 Hz" : "(*) 1 Hz");                 // ⟳
        std::string right = col(_monFrozen ? Role::Dim : Role::Text,
                                std::string(act) + "   " + dot + "  " + badge);
        int gap = (kCols - 2) - 1 - dispWidth(left) - dispWidth(right) - 1;
        if (gap < 1) gap = 1;
        bodyRow(left + std::string((size_t)gap, ' ') + right);
    }

    // Row 5: column header.  Row 6: rule (matches §2.9 column widths).
    bodyRow(col(Role::Header,
        " CH  EXT   DEST            DUR     CODEC   STATUS"));
    {
        const char* H = glyph(Glyph::BoxH);
        std::string r = " ";
        auto run = [&](int n) { for (int i = 0; i < n; ++i) r += H; };
        run(2); r += "  "; run(4); r += "  "; run(14); r += "  ";
        run(6); r += "  "; run(5); r += "  "; run(15);
        bodyRow(col(Role::Border, r));
    }

    // Rows 7..10: four channel rows. A live session lights its slot; the rest idle.
    for (int ch = 1; ch <= kMonMaxChans; ++ch)
    {
        const CallRow* cr = nullptr;
        for (const auto& c : ms.calls) { if (c.ch == ch) { cr = &c; break; } }

        char chbuf[8]; std::snprintf(chbuf, sizeof(chbuf), " %2d  ", ch);
        std::string body;
        if (cr && cr->state != State::Idle)
        {
            // Column grid (DISPLAY cols): EXT(4)+2 DEST(14)+2 DUR(6)+2 CODEC(5)+2,
            // then the STATUS chip. Every cell is laid out via padCols() so the
            // multibyte em-dash ("—" in DUR for a not-yet-connected call) and arrow
            // ("→" in DEST) pad by visible width, not bytes — printf's "%-Ns" counted
            // bytes and shifted CODEC/STATUS left whenever a glyph appeared.
            std::string line = std::string(chbuf)
                + padCols(cr->ext,  4) + "  "
                + padCols(cr->dest, 14) + "  "
                + padCols(fmtDur(cr->durSec), 6) + "  "
                + padCols(cr->codec, 5) + "  ";
            body = col(Role::Text, line);
            int vis = 0; appendStateChip(cr->state, body, vis);
        }
        else
        {
            // Idle slot. DEST/DUR/CODEC are an em dash; STATUS is "○ idle". Lay the
            // dashes on the SAME column grid as the live rows (via padCols) so the
            // "○ idle" chip starts at the same body column as a live STATUS chip and
            // dispWidth stays exact in both tiers.
            std::string em = _unicode ? "\xe2\x80\x94" : "-";
            std::string line = std::string(chbuf)
                + padCols(em, 4) + "  "
                + padCols(em, 14) + "  "
                + padCols(em, 6) + "  "
                + padCols(em, 5) + "  ";
            body = col(Role::Text, line);
            int vis = 0; appendStateChip(State::Idle, body, vis);
        }
        bodyRow(body);
    }

    bodyBlank();   // row 11

    // Row 12: ROSTER  ● n ONLINE  ○ n UNREACH                VITALS
    {
        std::string body = col(Role::Header, " ROSTER   ");
        int vis = 0;
        appendStateChip(State::Online, body, vis,
                        (std::to_string(ms.online) + " ONLINE").c_str());
        body += "  ";
        appendStateChip(State::Unreach, body, vis,
                        (std::to_string(ms.unreachable) + " UNREACH").c_str());
        int gap = 49 - dispWidth(body); if (gap < 2) gap = 2;
        body += std::string((size_t)gap, ' ');
        body += col(Role::Header, "VITALS");
        bodyRow(body);
    }

    // Rows 13..15: roster entries (two columns) + vitals bars (right). Vitals are
    // MEM (real heap %), POOL (n/8 calls), UP (uptime) — honest numbers only; CPU%
    // needs FreeRTOS runtime stats (off in sdkconfig) so it is omitted, not faked.
    {
        std::string mem = "MEM "; { int x = 0; appendVitalsBar(ms.memUsedPct, mem, x); }
        char poolb[24]; std::snprintf(poolb, sizeof(poolb), "POOL  %d/%d calls",
                                      ms.poolUsed, ms.maxCalls);
        std::string vitals[3] = {
            mem, col(Role::Text, poolb),
            col(Role::Text, "UP  " + fmtClock(ms.uptimeSec))
        };

        for (int i = 0; i < 3; ++i)
        {
            std::string body;
            auto emitEntry = [&](size_t idx, int width) {
                if (idx < ms.roster.size())
                {
                    const RosterRow& rr = ms.roster[idx];
                    std::string lbl = rr.ext;
                    if (!rr.name.empty()) lbl += " " + rr.name;
                    if ((int)lbl.size() > width) lbl.resize((size_t)width);
                    char b[40]; std::snprintf(b, sizeof(b), " %-*s ", width, lbl.c_str());
                    body += col(Role::Text, b);
                    int vis = 0; appendStateChip(rr.state, body, vis);
                }
                else
                {
                    int w = 1 + width + 1 + stateChipVis(State::Online);
                    body += std::string((size_t)w, ' ');
                }
            };
            emitEntry((size_t)i * 2, 13);
            body += "  ";
            emitEntry((size_t)i * 2 + 1, 13);
            int gap = 49 - dispWidth(body); if (gap < 2) gap = 2;
            body += std::string((size_t)gap, ' ');
            body += vitals[i];
            bodyRow(body);
        }
    }

    bodyBlank();   // row 16

    // Row 17: the sanctioned flavor + ring-test hint (brand §4.4 / M4).
    {
        std::string ell = _unicode ? "\xe2\x80\xa6" : "...";
        bodyRow(col(Role::Text,
            " Patching you through" + ell +
            "   place a call on any phone to watch it light up."));
    }

    // Rows 18..21: four blanks → body total = 18 (rows 4..21).
    for (int i = 0; i < 4; ++i) bodyBlank();

    drawFooter("[F] Freeze  [C] Clear  [Esc] Main  [?] Help");
    _lastClock = fmtClock(ms.uptimeSec);
}

// Repaint the monitor's live region by homing the cursor and rewriting the frame
// in place — never a full clear (renderer contract §6.4). Frozen ([F]) is a no-op
// (the screen reads "❚❚ FROZEN" from the last full draw). Called ~1 Hz while the
// monitor is the active screen.
void Tui::repaintMonitorLive()
{
    if (_screen != Screen::Monitor) return;
    if (_monFrozen) return;              // [F]: frozen by design
    home();
    drawMonitorFrame();
}

// ─────────────────────────────────────────────────────────────────────────────
// B2b-3 screens: [2] NETWORK · [4] SECURITY · [5] REPORTS · [6] ABOUT (+ modals).
// All reuse the 3-zone spine + the framed-row discipline from the screens above:
// drawSpineTop(mode), framed body rows padded via dispWidth(), drawFooter(keys).
// Tabular/aligned content routes through padCols(); status chips through the
// glyph+LABEL lexicon (never colour alone). The local `chip()` lambda below maps a
// CdrResult → its sanctioned (glyph, role, label) in ONE place (§3.11 / IA §6.2).
// ─────────────────────────────────────────────────────────────────────────────

// Map a CdrResult to its sanctioned glyph+LABEL chip (colored), appended to `out`.
// The label is authoritative (accessibility: color is removable). Glyphs per §3.11:
//   ✓ answered ‹accent› · ⊘ busy ‹amber› · … cancelled ‹brass› · ○ unavailable ‹dim›
//   ▲ failed ‹red›.  ASCII fallbacks: [v] [/] ... ( ) /!\.
static const char* cdrLabel(Tui::CdrResult r)
{
    switch (r)
    {
        case Tui::CdrResult::Answered:    return "answered";
        case Tui::CdrResult::Busy:        return "busy";
        case Tui::CdrResult::Cancelled:   return "cancelled";
        case Tui::CdrResult::Unavailable: return "unavailable";
        case Tui::CdrResult::Failed:      return "failed";
    }
    return "failed";
}

// The status-lexicon State → a plain (uncoloured) "glyph LABEL" string. Used inside a
// reverse-video selected row where an inner SGR colour reset would break the highlight
// span (so we keep the glyph+label but drop the per-state colour — the label still
// carries the meaning, accessibility §). The instance method `glyph()` resolves the
// unicode/ASCII tier, so this is a member helper defined inline below.
std::string Tui::stateLabelPlain(State s) const
{
    Glyph g; const char* label;
    switch (s)
    {
        case State::Online:  g = Glyph::Online;  label = "ONLINE";  break;
        case State::Unreach: g = Glyph::Unreach; label = "UNREACH"; break;
        case State::Ringing: g = Glyph::Ringing; label = "RINGING"; break;
        case State::Active:  g = Glyph::Active;  label = "ACTIVE";  break;
        case State::Dnd:     g = Glyph::Dnd;     label = "DND";     break;
        default:             g = Glyph::Unreach; label = "idle";    break;
    }
    return std::string(glyph(g)) + " " + label;
}

// Compose a CdrResult chip "‹glyph› ‹label›" into `out`, colored by role. Returns
// the visible column width so callers can pad the RESULT column to a fixed width.
// `_unicode`/`_color` are honored via the instance; this is a member-style helper
// implemented inline in renderReports/renderCdrDetail through the `resChip` lambda.

// ── [2] NETWORK (tui-style §3.4) ──────────────────────────────────────────────
void Tui::renderNetwork()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("NETWORK", st);

    const std::string v = glyph(Glyph::BoxV);
    const std::string dot = _unicode ? "\xc2\xb7" : ".";
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    // " Label ....... value" leader row (dotted leader, §3.4 alignment).
    auto leader = [&](const char* label, const std::string& value) {
        std::string l = " ";
        l += label;
        l += " ";
        int dots = 11 - (int)std::strlen(label);   // align colons/values at col ~14
        if (dots < 1) dots = 1;
        l += std::string((size_t)dots, '.');
        l += " ";
        bodyRow(col(Role::Text, l) + value);
    };

    int bodyUsed = 0;
    auto emit = [&](const std::string& body) { bodyRow(body); ++bodyUsed; };
    auto emitBlank = [&]() { bodyBlank(); ++bodyUsed; };

    emit(col(Role::Header, " NETWORK STATUS"));
    // Underline rule (── under the heading, §3.4).
    {
        std::string r = " ";
        const char* H = glyph(Glyph::BoxH);
        for (int i = 0; i < 14; ++i) r += H;
        bodyRow(col(Role::Border, r));
        ++bodyUsed;
    }

    // Mode line — accent glyph+LABEL chip, then the wifi_mode tag (§3.4).
    {
        const char* modeWord; const char* modeTail;
        if (st.netMode == 1)      { modeWord = "CLIENT (STATION)"; modeTail = "wifi_mode 1"; }
        else if (st.netMode == 0) { modeWord = "SETUP (captive)";  modeTail = "wifi_mode 0"; }
        else                      { modeWord = "STANDALONE HOTSPOT (SoftAP)"; modeTail = "wifi_mode 2"; }
        std::string body = col(Role::Text, " Mode ......... ");
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Lamp) + "m";
        body += glyph(Glyph::Active);   // ◆
        body += " ";
        body += modeWord;
        if (_color) body += "\x1b[0m";
        int gap = 56 - dispWidth(body); if (gap < 2) gap = 2;
        body += std::string((size_t)gap, ' ');
        body += col(Role::Dim, modeTail);
        emit(body);
    }
    leader("SSID", col(Role::Text, st.ssid.empty() ? std::string("(unset)") : st.ssid)); ++bodyUsed;
    leader("Host", col(Role::Text, st.host)); ++bodyUsed;
    {
        std::string addr = st.ip;
        if (!st.mac.empty()) addr += "   " + dot + "   " + st.mac;
        leader("Address", col(Role::Text, addr)); ++bodyUsed;
    }
    // Link / client count chip line (● UP · n phones registered).
    {
        std::string body = col(Role::Text, " Link ......... ");
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Lamp) + "m";
        body += glyph(Glyph::Online);   // ●
        body += " UP";
        if (_color) body += "\x1b[0m";
        char b[48];
        std::snprintf(b, sizeof(b), "   %s   %d phone%s registered",
                      dot.c_str(), st.online, st.online == 1 ? "" : "s");
        body += col(Role::Text, b);
        emit(body);
    }
    emitBlank();
    emit(col(Role::Text, " To move pocket-dial onto your office network, switch"));
    emit(col(Role::Text, " modes below. Phones re-register after the link returns."));
    emitBlank();
    // The guarded mode-switch action line.
    {
        std::string body;
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Header) + "m";
        body += " [M] Switch network mode";
        if (_color) body += "\x1b[0m";
        body += col(Role::Alert, _unicode ? "  [A!]" : "  [A!]");
        const char* to = (st.netMode == 1) ? "  \xe2\x86\x92  Hotspot (SoftAP)"
                                            : "  \xe2\x86\x92  Client (join Wi-Fi, DHCP)";
        const char* toAscii = (st.netMode == 1) ? "  ->  Hotspot (SoftAP)"
                                                 : "  ->  Client (join Wi-Fi, DHCP)";
        body += col(Role::Text, _unicode ? to : toAscii);
        emit(body);
    }

    for (int i = bodyUsed; i < 18; ++i) bodyBlank();
    drawFooter("[M] Mode switch  [Esc] Back  [?] Help");
    _lastClock = fmtClock(st.uptimeSec);
}

// ── [4] SECURITY (tui-style §3.10) ────────────────────────────────────────────
void Tui::renderSecurity()
{
    LiveStats st = stats();
    SecurityInfo si = security();
    clearScreen();
    hideCursor();
    drawSpineTop("SECURITY", st);

    const std::string v = glyph(Glyph::BoxV);
    const std::string dot = _unicode ? "\xc2\xb7" : ".";
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    auto rule = [&](int n) {
        std::string r = " ";
        const char* H = glyph(Glyph::BoxH);
        for (int i = 0; i < n; ++i) r += H;
        bodyRow(col(Role::Border, r));
    };

    int bodyUsed = 0;
    auto emit = [&](const std::string& body) { bodyRow(body); ++bodyUsed; };

    emit(col(Role::Header, " ADMIN ACCESS"));
    rule(12); ++bodyUsed;
    // Master PIN: ● SET / ○ none chip.
    {
        std::string body = col(Role::Text, " Master PIN ... ");
        if (si.provisioned)
        {
            if (_color) body += std::string("\x1b[") + sgrFor(Role::Lamp) + "m";
            body += glyph(Glyph::Online);
            body += " SET";
            if (_color) body += "\x1b[0m";
            body += col(Role::Text, "   (salted 50,000-round SHA-256, on-box)");
        }
        else
        {
            if (_color) body += std::string("\x1b[") + sgrFor(Role::Dim) + "m";
            body += glyph(Glyph::Unreach);
            body += " none";
            if (_color) body += "\x1b[0m";
            body += col(Role::Text, "   (SSH is OPEN until a PIN is set)");
        }
        emit(body);
    }
    {
        std::string body = col(Role::Text, " Admin ext .... ");
        body += col(Role::Text, si.adminExt);
        body += col(Role::Dim, "   (owns the DTMF *PIN#code menu)");
        emit(body);
    }
    // SSH login chip: enabled (PIN required / OPEN) vs disabled.
    {
        std::string body = col(Role::Text, " SSH login .... ");
        if (si.sshEnabled)
        {
            if (_color) body += std::string("\x1b[") + sgrFor(Role::Lamp) + "m";
            body += glyph(Glyph::Online);
            body += si.provisioned ? " ON" : " ON";
            if (_color) body += "\x1b[0m";
            body += col(Role::Text, si.provisioned ? "    PIN required" : "    OPEN (no PIN yet)");
        }
        else
        {
            if (_color) body += std::string("\x1b[") + sgrFor(Role::Dim) + "m";
            body += glyph(Glyph::Unreach);
            body += " OFF";
            if (_color) body += "\x1b[0m";
            body += col(Role::Text, "   (engine disabled)");
        }
        emit(body);
    }
    bodyBlank(); ++bodyUsed;
    emit(col(Role::Header, " SESSION"));
    rule(7); ++bodyUsed;
    {
        std::string body = col(Role::Text, " You .......... ");
        body += col(Role::Text, si.sessionUser + " @ " + si.sessionPeer);
        if (!si.sinceClock.empty())
            body += col(Role::Text, "   " + dot + "   up " + si.sinceClock);
        emit(body);
    }
    emit(col(Role::Text, " Logging ...... all SSH sessions are recorded to the CDR"));
    bodyBlank(); ++bodyUsed;
    // Action row.
    {
        std::string body;
        auto act = [&](const char* keyHot, const char* rest, bool danger) {
            if (_color) body += std::string("\x1b[") + sgrFor(Role::Header) + "m";
            body += keyHot;
            if (_color) body += "\x1b[0m";
            body += col(Role::Text, rest);
            if (danger) body += col(Role::Alert, " [A!]");
        };
        // Keep the action row ≤77 display cols (the body budget) — the verbose
        // labels ("Change admin PIN" / "Registrar/Devices") summed to ~84 and broke
        // the 80-col frame on hardware. Shortened; the keys and intent still read.
        body += " ";
        act("[P]", " Change PIN", false);
        body += "  ";
        act("[K]", " SSH access", false);
        body += "  ";
        act("[D]", " Registrar", false);
        body += "  ";
        act("[X]", " Factory reset", true);
        emit(body);
    }

    for (int i = bodyUsed; i < 18; ++i) bodyBlank();
    // Keep ≤58 cols (Phosphor theme-label budget) so the footer truncator never
    // drops [?] Help: dropped the explicit "[Esc] Back" chip (Esc-to-back still works).
    drawFooter("[P] PIN  [K] SSH  [D] Devices  [X] Reset  [?] Help");
    _lastClock = fmtClock(st.uptimeSec);
}

// ── [5] REPORTS — Recent Calls (CDR) / Event Log, [Tab] flips (tui-style §3.11) ─
void Tui::renderReports()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("REPORTS", st);

    const std::string v = glyph(Glyph::BoxV);
    const std::string dot = _unicode ? "\xc2\xb7" : ".";
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    // The CdrResult chip (glyph+LABEL+role) appended to `out`, advancing the visible
    // width counter. Glyph + label per §3.11; ASCII degrades via the glyph table.
    auto resChip = [&](CdrResult r, std::string& out, int& vis) {
        Glyph g; Role role;
        switch (r)
        {
            case CdrResult::Answered:    g = Glyph::Ready;   role = Role::Lamp;   break;  // ✓→◆ accent
            case CdrResult::Busy:        g = Glyph::Dnd;     role = Role::Dnd;    break;  // ⊘ amber
            case CdrResult::Cancelled:   g = Glyph::Unreach; role = Role::Text;   break;  // … →brass
            case CdrResult::Unavailable: g = Glyph::Unreach; role = Role::Dim;    break;  // ○ dim
            case CdrResult::Failed:      g = Glyph::Alert;   role = Role::Alert;  break;  // ▲ red
            default:                     g = Glyph::Alert;   role = Role::Alert;  break;
        }
        // Cancelled uses an ellipsis glyph rather than ○; emit it explicitly.
        std::string gl = (r == CdrResult::Cancelled)
                       ? (_unicode ? std::string("\xe2\x80\xa6") : std::string("..."))
                       : (r == CdrResult::Answered
                          ? (_unicode ? std::string("\xe2\x9c\x93") : std::string("[v]"))  // ✓
                          : std::string(glyph(g)));
        if (_color) out += std::string("\x1b[") + sgrFor(role) + "m";
        out += gl;                       vis += dispWidth(gl);
        out += " ";                      vis += 1;
        out += cdrLabel(r);              vis += (int)std::strlen(cdrLabel(r));
        if (_color) out += "\x1b[0m";
    };

    // View selector line: Recent Calls ◂▸ Event Log  (active=bright, other=dim).
    {
        std::string body = col(Role::Text, " VIEW:  ");
        const char* a = "Recent Calls";
        const char* b = "Event Log";
        std::string arrows = _unicode ? " \xe2\x97\x82\xe2\x96\xb8 " : " <> ";
        body += col(_reportsEventLog ? Role::Dim : Role::Header, a);
        body += col(Role::Dim, arrows);
        body += col(_reportsEventLog ? Role::Header : Role::Dim, b);
        std::string note = _reportsEventLog
            ? (_unicode ? "   (live tail \xc2\xb7 newest at bottom)" : "   (live tail . newest at bottom)")
            : (_unicode ? "   (newest first \xc2\xb7 ring of 32)" : "   (newest first . ring of 32)");
        body += col(Role::Dim, note);
        bodyRow(body);
    }

    int bodyUsed = 1;
    if (!_reportsEventLog)
    {
        // ── Recent Calls (CDR) ───────────────────────────────────────────────
        ReportsSnapshot rs = reports();
        // Column header + rule (§3.11.1): TIME(8) FROM→TO(20) RESULT(15) TALK(6).
        bodyRow(col(Role::Header, " TIME      FROM \xe2\x86\x92 TO            RESULT           TALK"));
        ++bodyUsed;
        {
            const char* H = glyph(Glyph::BoxH);
            std::string r = " ";
            auto run = [&](int n) { for (int i = 0; i < n; ++i) r += H; };
            run(8); r += "  "; run(20); r += "  "; run(15); r += "  "; run(6);
            bodyRow(col(Role::Border, r));
            ++bodyUsed;
        }
        int total = (int)rs.cdr.size();
        // Clamp selection + scroll window to the data.
        if (_cdrSel >= total) _cdrSel = total > 0 ? total - 1 : 0;
        if (_cdrSel < 0) _cdrSel = 0;
        if (_cdrSel < _cdrTop) _cdrTop = _cdrSel;
        if (_cdrSel >= _cdrTop + kCdrRows) _cdrTop = _cdrSel - kCdrRows + 1;
        if (_cdrTop < 0) _cdrTop = 0;

        if (total == 0)
        {
            bodyRow(col(Role::Dim, " (no calls recorded yet \xe2\x80\x94 place a call to populate the CDR)"));
            ++bodyUsed;
            for (int i = 0; i < kCdrRows - 1; ++i) { bodyBlank(); ++bodyUsed; }
        }
        else
        {
            for (int i = 0; i < kCdrRows; ++i)
            {
                int idx = _cdrTop + i;
                if (idx >= total) { bodyBlank(); ++bodyUsed; continue; }
                const CdrEntry& e = rs.cdr[idx];
                bool sel = (idx == _cdrSel);
                // TIME (uptime HH:MM:SS of startMs — honest, no RTC).
                std::string tm = fmtClock(e.startMs / 1000ULL);
                std::string fromTo = e.caller + " " + (_unicode ? "\xe2\x86\x92" : "->") + " " + e.callee;
                std::string talk = (e.result == CdrResult::Answered && e.durationSec > 0)
                                 ? fmtDur((int)e.durationSec)
                                 : (_unicode ? std::string("\xe2\x80\x94") : std::string("-"));
                // Fixed column grid via padCols (UTF-8 safe). TIME(8) FROM→TO(20)
                // RESULT(15) TALK(6), preceded by the ▸ selector cell.
                std::string grid;
                grid += (sel ? (_unicode ? "\xe2\x96\xb8" : ">") : " ");  // ▸ selector
                grid += " ";
                grid += padCols(tm, 8) + "  ";
                grid += padCols(fromTo, 20) + "  ";
                std::string line;
                if (sel && _color)
                {
                    // Selected row: ONE reverse-video span over the whole row so the
                    // highlight is unbroken (no inner SGR resets). The RESULT word is
                    // still the authoritative chip — reverse video carries selection,
                    // not the result state (color is never the sole signal).
                    std::string res; int rvis = 0;
                    res += (_unicode && e.result == CdrResult::Answered) ? "\xe2\x9c\x93"
                         : (_unicode && e.result == CdrResult::Cancelled) ? "\xe2\x80\xa6"
                         : std::string(glyph(
                             e.result == CdrResult::Busy        ? Glyph::Dnd :
                             e.result == CdrResult::Failed      ? Glyph::Alert :
                             e.result == CdrResult::Answered    ? Glyph::Ready :
                                                                  Glyph::Unreach));
                    res += " "; res += cdrLabel(e.result);
                    int rvw = dispWidth(res); (void)rvis;
                    res += std::string((size_t)std::max(0, 15 - rvw), ' ');
                    line += "\x1b[7m";
                    line += grid + res + "  " + padCols(talk, 6);
                    line += "\x1b[0m";
                }
                else
                {
                    std::string res; int rvis = 0; resChip(e.result, res, rvis);
                    res += std::string((size_t)std::max(0, 15 - rvis), ' ');
                    line += col(Role::Text, grid);
                    line += res;
                    line += col(Role::Text, "  " + padCols(talk, 6));
                }
                bodyRow(line);
                ++bodyUsed;
            }
        }
        // Headroom n/32 (bottom-right of the data block).
        {
            char b[24]; std::snprintf(b, sizeof(b), "%d/%d", total, POCKETDIAL_CDR_RECORDS_UI);
            std::string body;
            int gap = (kCols - 2) - 1 - (int)std::strlen(b) - 2;
            if (gap < 1) gap = 1;
            body += std::string((size_t)gap, ' ');
            body += col(Role::Text, b);
            bodyRow(body);
            ++bodyUsed;
        }
        for (int i = bodyUsed; i < 18; ++i) bodyBlank();
        drawFooter("[Tab] Log  [\xe2\x86\x91/\xe2\x86\x93] Sel  [Enter] Detail  [Esc] Back  [?] Help");
    }
    else
    {
        // ── Event Log tail (§3.11.3) ─────────────────────────────────────────
        // No persistent event-log ring exists yet (honesty clause): rather than fake
        // a scroll of invented events, we synthesize a SMALL, honest tail from the
        // facts we DO have (the CDR ring, newest at bottom) and clearly label the
        // view as derived. When a real log ring lands it drops in here unchanged.
        {
            const char* H = glyph(Glyph::BoxH);
            std::string r = " ";
            for (int i = 0; i < kCols - 4; ++i) r += H;
            bodyRow(col(Role::Border, r));
            ++bodyUsed;
        }
        ReportsSnapshot rs = reports();
        // Newest at the BOTTOM (§3.11.3): the CDR is newest-first (index 0 = newest),
        // so print the most-recent kLogRows window with the OLDER index at the top and
        // index 0 last — i.e. walk indices high→low. `win` is the oldest index shown.
        int shown = 0;
        const int kLogRows = 10;
        int win = std::min((int)rs.cdr.size(), kLogRows) - 1;   // oldest visible index
        for (int idx = win; idx >= 0; --idx)
        {
            const CdrEntry& e = rs.cdr[idx];
            std::string tm = fmtClock(e.startMs / 1000ULL);
            std::string fromTo = e.caller + " " + (_unicode ? "\xe2\x86\x92" : "->") + " " + e.callee;
            std::string body;
            body += " ";
            body += col(Role::Text, padCols(tm, 8) + "  ");
            body += col(Role::Header, padCols("CDR", 6)) + " ";
            body += col(Role::Text, padCols(fromTo, 18) + " ");
            int rvis = 0; std::string res; resChip(e.result, res, rvis);
            body += res;
            bodyRow(body);
            ++bodyUsed; ++shown;
        }
        if (shown == 0)
        {
            bodyRow(col(Role::Dim, " (live log \xe2\x80\x94 newest at bottom; no events yet)"));
            ++bodyUsed;
        }
        else
        {
            bodyRow(col(Role::Dim, " (derived from the CDR ring \xe2\x80\x94 a dedicated event log lands in a later build)"));
            ++bodyUsed;
        }
        for (int i = bodyUsed; i < 18; ++i) bodyBlank();
        drawFooter("[Tab] Recent Calls  [Esc] Back  [?] Help");
    }
    _lastClock = fmtClock(st.uptimeSec);
}

// ── [5] CDR detail modal (tui-style §3.11.2) ──────────────────────────────────
void Tui::renderCdrDetail()
{
    LiveStats st = stats();
    ReportsSnapshot rs = reports();
    clearScreen();
    hideCursor();
    drawSpineTop("REPORTS", st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };

    const int boxW = 56;
    const int indent = (kCols - 2 - boxW) / 2;
    const char* TL = glyph(Glyph::BoxTL); const char* TR = glyph(Glyph::BoxTR);
    const char* BL = glyph(Glyph::BoxBL); const char* BR = glyph(Glyph::BoxBR);
    const char* VR = glyph(Glyph::BoxVR); const char* VL = glyph(Glyph::BoxVL);
    const std::string H = glyph(Glyph::BoxH);

    auto frame = [&](bool top, bool sep, const char* title) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        if (sep) put(VR); else put(top ? TL : BL);
        int used = 0;
        if (top && title) { put(H); put(" "); put(title); put(" "); used = 1 + 1 + dispWidth(title) + 1; }
        for (int i = 0; i < boxW - 2 - used; ++i) put(H);
        if (sep) put(VL); else put(top ? TR : BR);
        if (_color) put("\x1b[0m");
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto line = [&](const std::string& inner) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        put(" ");
        put(inner);
        int ipad = (boxW - 2) - 1 - dispWidth(inner); if (ipad < 0) ipad = 0;
        put(std::string((size_t)ipad, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };

    // Resolve the selected record (clamp; empty CDR shows a placeholder).
    int total = (int)rs.cdr.size();
    int sel = _cdrSel; if (sel >= total) sel = total - 1; if (sel < 0) sel = 0;

    bodyBlank(); bodyBlank();
    frame(true, false, "Call detail");
    if (total == 0)
    {
        line(col(Role::Dim, "No records."));
    }
    else
    {
        const CdrEntry& e = rs.cdr[sel];
        auto resChip = [&](CdrResult r) -> std::string {
            std::string out; int vis = 0;
            Role role;
            std::string gl;
            switch (r)
            {
                case CdrResult::Answered:    role = Role::Lamp;  gl = _unicode ? "\xe2\x9c\x93" : "[v]"; break;
                case CdrResult::Busy:        role = Role::Dnd;   gl = glyph(Glyph::Dnd); break;
                case CdrResult::Cancelled:   role = Role::Text;  gl = _unicode ? "\xe2\x80\xa6" : "..."; break;
                case CdrResult::Unavailable: role = Role::Dim;   gl = glyph(Glyph::Unreach); break;
                default:                     role = Role::Alert; gl = glyph(Glyph::Alert); break;
            }
            (void)vis;
            if (_color) out += std::string("\x1b[") + sgrFor(role) + "m";
            out += gl + " " + cdrLabel(r);
            if (_color) out += "\x1b[0m";
            return out;
        };
        line(col(Role::Text, "Started .... up ") + col(Role::Text, fmtClock(e.startMs / 1000ULL)));
        line(col(Role::Text, "From ....... ") + col(Role::Text, e.caller));
        line(col(Role::Text, "To ......... ") + col(Role::Text, e.callee));
        line(col(Role::Text, "Result ..... ") + resChip(e.result));
        {
            std::string talk = (e.result == CdrResult::Answered && e.durationSec > 0)
                             ? fmtDur((int)e.durationSec)
                             : (_unicode ? std::string("\xe2\x80\x94") : std::string("-"));
            line(col(Role::Text, "Talk time .. ") + col(Role::Text, talk));
        }
        line(col(Role::Text, "Codec ...... ") + col(Role::Text, e.codec.empty() ? "PCMU" : e.codec));
        line(col(Role::Text, "Call-ID .... ") + col(Role::Text, e.callId.empty() ? "\xe2\x80\x94" : e.callId));
    }
    frame(false, true, nullptr);
    line(col(Role::Dim, "[\xe2\x86\x91/\xe2\x86\x93] Records   [Esc] Back   [?] Help"));
    frame(false, false, nullptr);

    // Body so far: 2 blank + frame + (1 or 8 lines) + frame + key + frame.
    int used = (total == 0) ? (2 + 1 + 1 + 1 + 1 + 1) : (2 + 1 + 8 + 1 + 1 + 1);
    for (int i = used; i < 18; ++i) bodyBlank();
    drawFooter("[\xe2\x86\x91/\xe2\x86\x93] Records  [Esc] Back  [?] Help");
}

// ── [6] ABOUT — the honesty card (tui-style §3.12) ────────────────────────────
void Tui::renderAbout()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("ABOUT", st);

    const std::string v = glyph(Glyph::BoxV);
    const std::string dot = _unicode ? "\xc2\xb7" : ".";
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    auto leader = [&](const char* label, const std::string& value) {
        std::string l = "  ";
        l += label;
        l += " ";
        int dots = 10 - (int)std::strlen(label);
        if (dots < 1) dots = 1;
        l += std::string((size_t)dots, '.');
        l += " ";
        bodyRow(col(Role::Text, l) + value);
    };

    int bodyUsed = 0;
    auto emit = [&](const std::string& body) { bodyRow(body); ++bodyUsed; };

    // Brand wordmark + descriptor (▌▐ POCKET-DIAL ▐▌ · sysop terminal ·).
    {
        std::string body = "  ";
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Header) + "m";
        body += glyph(Glyph::WordmarkL);
        body += " POCKET-DIAL ";
        body += glyph(Glyph::WordmarkR);
        if (_color) body += "\x1b[0m";
        body += col(Role::Text, std::string("  ") + dot + " sysop terminal " + dot);
        emit(body);
    }
    emit(col(Role::Text, "  single-board SIP PBX \xe2\x80\x94 operator on duty"));
    {
        const char* H = glyph(Glyph::BoxH);
        std::string r = "  ";
        for (int i = 0; i < kCols - 6; ++i) r += H;
        bodyRow(col(Role::Border, r));
        ++bodyUsed;
    }
    leader("Hardware", col(Role::Text, std::string("ESP32-S3 ") + dot +
            " Guition JC3248W535 " + dot + " 8 MB PSRAM " + dot + " 16 MB flash")); ++bodyUsed;
    leader("Firmware", col(Role::Text, std::string("POCKET-DIAL ") + st.fw +
            "   " + dot + "   build " + __DATE__)); ++bodyUsed;
    leader("Host", col(Role::Text, st.host + "   " + dot + "   " + st.mac)); ++bodyUsed;
    leader("Uptime", col(Role::Text, fmtClock(st.uptimeSec) +
            (st.heapUsedPct > 0
             ? ("   " + dot + "   heap " + std::to_string(st.heapUsedPct) + "% used")
             : std::string("")))); ++bodyUsed;
    leader("Capacity", col(Role::Text, "32 extensions   " + dot + "   2\xe2\x80\x93" "8 concurrent calls")); ++bodyUsed;
    leader("Voicemail", col(Role::Dim, "arrives in v2 (needs an RTP record path)")); ++bodyUsed;
    leader("Storage", col(Role::Dim, "on-device flash only \xe2\x80\x94 no SD card")); ++bodyUsed;
    leader("License", col(Role::Text, "see LICENSE in the source tree")); ++bodyUsed;
    bodyBlank(); ++bodyUsed;
    emit(col(Role::Text, "  This is an ESP32-S3. It does what it says, and says what it does."));

    for (int i = bodyUsed; i < 18; ++i) bodyBlank();
    drawFooter("[Esc] Back  [?] Help");
    _lastClock = fmtClock(st.uptimeSec);
}

// ── Shared guarded-confirm box (reused by ModeConfirm + FactoryConfirm) ───────
// Mirrors renderRebootConfirm()'s geometry: a centered ┌─ <title> ─┐ box with an
// alert chip line, consequence text, a destructive/safe button pair (safe default
// reverse-video focused), a rule, and a key line — on the real 3-zone spine.
void Tui::drawConfirmBox(const char* title, const char* glyphLabel, Role glyphRole,
                         const std::vector<std::string>& lines,
                         const char* destructiveBtn, const char* safeBtn,
                         bool destructiveFocused, const char* keyLine)
{
    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    const int boxW = 58;
    const int indent = (kCols - 2 - boxW) / 2;
    const char* TL = glyph(Glyph::BoxTL); const char* TR = glyph(Glyph::BoxTR);
    const char* BL = glyph(Glyph::BoxBL); const char* BR = glyph(Glyph::BoxBR);
    const char* VR = glyph(Glyph::BoxVR); const char* VL = glyph(Glyph::BoxVL);
    const std::string Hh = glyph(Glyph::BoxH);

    auto boxFrame = [&](bool top, bool sep) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        if (sep) put(VR); else put(top ? TL : BL);
        int used = 0;
        if (top) { put(Hh); put(" "); put(title); put(" "); used = 1 + 1 + dispWidth(title) + 1; }
        for (int i = 0; i < boxW - 2 - used; ++i) put(Hh);
        if (sep) put(VL); else put(top ? TR : BR);
        if (_color) put("\x1b[0m");
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto boxLine = [&](const std::string& inner) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        put(" ");
        put(inner);
        int ipad = (boxW - 2) - 1 - dispWidth(inner); if (ipad < 0) ipad = 0;
        put(std::string((size_t)ipad, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };

    int used = 0;
    bodyBlank(); ++used;
    bodyBlank(); ++used;
    boxFrame(true, false); ++used;
    // Alert chip line.
    {
        std::string inner;
        if (_color) inner += std::string("\x1b[") + sgrFor(glyphRole) + "m";
        inner += glyphLabel;
        if (_color) inner += "\x1b[0m";
        boxLine(inner); ++used;
    }
    boxLine(""); ++used;
    for (const auto& l : lines) { boxLine(col(Role::Text, l)); ++used; }
    boxLine(""); ++used;
    // Buttons: destructive on the left, safe on the right; focused = reverse video.
    {
        std::string inner = "   ";
        if (destructiveFocused && _color) inner += "\x1b[7m";
        else if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += destructiveBtn;
        if (_color) inner += "\x1b[0m";
        inner += "        ";
        if (!destructiveFocused && _color) inner += "\x1b[7m";
        else if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += safeBtn;
        if (_color) inner += "\x1b[0m";
        boxLine(inner); ++used;
    }
    boxFrame(false, true); ++used;
    boxLine(col(Role::Dim, keyLine)); ++used;
    boxFrame(false, false); ++used;

    for (int i = used; i < 18; ++i) bodyBlank();
}

// ── [2]/[M] Wi-Fi mode switch confirm (reboots) ───────────────────────────────
void Tui::renderModeConfirm()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("NETWORK", st);
    const char* to = (st.netMode == 1) ? "Hotspot (SoftAP)" : "Client (join Wi-Fi)";
    std::string l1 = std::string("Switch to ") + to + " and reboot now?";
    std::vector<std::string> lines = {
        l1,
        "Wi-Fi drops for ~8 seconds while the board restarts.",
        "Phones re-register after the link comes back."
    };
    std::string alert = std::string(glyph(Glyph::Alert)) + " ALERT   MODE SWITCH";
    drawConfirmBox("Confirm mode switch", alert.c_str(), Role::Alert, lines,
                   "< Switch >", "[ Stay ]", /*destructiveFocused=*/false,
                   _unicode ? "[\xe2\x86\x90/\xe2\x86\x92] Choose   [Enter] Confirm   y/N   [Esc] Cancel"
                            : "[</>] Choose   [Enter] Confirm   y/N   [Esc] Cancel");
    drawFooter("[\xe2\x86\x90/\xe2\x86\x92] Choose  [Enter] Confirm  [Esc] Cancel");
}

// ── [4]/[X] Factory reset DOUBLE-confirm (§3.10 note / IA §2) ──────────────────
void Tui::renderFactoryConfirm()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("SECURITY", st);

    std::vector<std::string> lines;
    const char* title;
    std::string alert;
    if (_factoryStep == 0)
    {
        title = "Confirm factory reset (1 of 2)";
        alert = std::string(glyph(Glyph::Alert)) + " ALERT   FACTORY RESET";
        lines = {
            "Erase ALL config: extensions, PINs, Wi-Fi, CDR.",
            "The board returns to first-run setup.",
            "This cannot be undone."
        };
    }
    else
    {
        title = "Confirm factory reset (2 of 2)";
        alert = std::string(glyph(Glyph::Alert)) + " ALERT   LAST CHANCE";
        lines = {
            "Press y once more to ERASE and restart.",
            "Everything provisioned on this box is wiped.",
            "Esc still cancels."
        };
    }
    drawConfirmBox(title, alert.c_str(), Role::Alert, lines,
                   "< Erase >", "[ Keep ]", /*destructiveFocused=*/false,
                   _unicode ? "[\xe2\x86\x90/\xe2\x86\x92] Choose   [Enter] Confirm   y/N   [Esc] Cancel"
                            : "[</>] Choose   [Enter] Confirm   y/N   [Esc] Cancel");
    drawFooter("[\xe2\x86\x90/\xe2\x86\x92] Choose  [Enter] Confirm  [Esc] Cancel");
}

// ── [4]/[P] Change admin PIN modal — never echoed (tui-style §3.10.1) ──────────
void Tui::renderChangePin()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("SECURITY", st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };

    const int boxW = 56;
    const int indent = (kCols - 2 - boxW) / 2;
    const char* TL = glyph(Glyph::BoxTL); const char* TR = glyph(Glyph::BoxTR);
    const char* BL = glyph(Glyph::BoxBL); const char* BR = glyph(Glyph::BoxBR);
    const char* VR = glyph(Glyph::BoxVR); const char* VL = glyph(Glyph::BoxVL);
    const std::string H = glyph(Glyph::BoxH);

    auto frame = [&](bool top, bool sep, const char* title) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        if (sep) put(VR); else put(top ? TL : BL);
        int used = 0;
        if (top && title) { put(H); put(" "); put(title); put(" "); used = 1 + 1 + dispWidth(title) + 1; }
        for (int i = 0; i < boxW - 2 - used; ++i) put(H);
        if (sep) put(VL); else put(top ? TR : BR);
        if (_color) put("\x1b[0m");
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto line = [&](const std::string& inner) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        put(" ");
        put(inner);
        int ipad = (boxW - 2) - 1 - dispWidth(inner); if (ipad < 0) ipad = 0;
        put(std::string((size_t)ipad, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    // A field row: "Label .... [ •••• ]   focused→bracket reverse".
    auto field = [&](const char* label, const std::string& value, int idx) {
        std::string inner = label;
        inner += " ";
        int dots = 12 - (int)std::strlen(label); if (dots < 1) dots = 1;
        inner += std::string((size_t)dots, '.');
        inner += " ";
        // Render entered digits as the • glyph (U+2022) — never echo the PIN; width
        // math stays per-column because each bullet is one display cell.
        std::string dotsStr;
        for (size_t i = 0; i < value.size(); ++i) dotsStr += _unicode ? "\xe2\x80\xa2" : "*";
        std::string boxIn = std::string("[ ") + dotsStr;
        // pad the entry box to a stable inner width (10 cells).
        int padN = 10 - (int)value.size(); if (padN < 0) padN = 0;
        boxIn += std::string((size_t)padN, ' ');
        boxIn += " ]";
        std::string row;
        row += col(Role::Text, inner);
        if (idx == _pinField && _color) row += "\x1b[7m";   // focused field reverse
        row += col(Role::Text, boxIn);
        if (idx == _pinField && _color) row += "\x1b[0m";
        line(row);
    };

    bodyBlank(); bodyBlank();
    frame(true, false, "Change admin PIN");
    field("Current PIN", _pinCur, 0);
    field("New PIN", _pinNew, 1);
    field("Confirm", _pinConf, 2);
    line("");
    if (!_pinMsg.empty())
        line(col(Role::Alert, _pinMsg));
    else
        line(col(Role::Dim, "4\xe2\x80\x93" "10 digits \xc2\xb7 never echoed \xc2\xb7 [Tab] next field"));
    line("");
    {
        // Apply / Cancel buttons (Apply focused once all three fields have input).
        std::string inner = "        ";
        if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += "< Apply >";
        if (_color) inner += "\x1b[0m";
        inner += "        ";
        if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += "[ Cancel ]";
        if (_color) inner += "\x1b[0m";
        line(inner);
    }
    frame(false, true, nullptr);
    line(col(Role::Dim, "[Tab] Field  [Enter] Apply  [Esc] Cancel  [?] Help"));
    frame(false, false, nullptr);

    // Body: 2 blank + frame + 3 fields + blank + msg + blank + buttons + frame + key
    // + frame = 13. Pad to 18.
    for (int i = 13; i < 18; ++i) bodyBlank();
    drawFooter("[Tab] Field  [Enter] Apply  [Esc] Cancel  [?] Help");
}

// ── Screen router ──
// Set the active screen and draw it. Entering the monitor clears any stale freeze
// so [1] always opens live; leaving it is handled by the keys.
void Tui::gotoScreen(Screen s)
{
    if (s == Screen::Monitor && _screen != Screen::Monitor)
        _monFrozen = false;            // fresh entry -> always live (un-freeze)
    // Remember where Help was opened FROM so its Esc returns to the origin screen,
    // not always the hub (tui-ia §3: context help never leaves the current screen).
    // Guard against Help→Help (Ctrl-L redraw) clobbering the saved origin.
    if (s == Screen::Help && _screen != Screen::Help)
        _helpReturn = _screen;
    _screen = s;
    switch (s)
    {
        case Screen::Banner:         renderBanner();         break;
        case Screen::Hub:            renderHub();            break;
        case Screen::Help:           renderHelp();           break;
        case Screen::RebootConfirm:  renderRebootConfirm();  break;
        case Screen::Placeholder:    renderPlaceholder();    break;
        case Screen::Monitor:        renderMonitor();        break;
        case Screen::Network:        renderNetwork();        break;
        case Screen::Security:       renderSecurity();       break;
        case Screen::Reports:        renderReports();        break;
        case Screen::About:          renderAbout();          break;
        case Screen::ModeConfirm:    renderModeConfirm();    break;
        case Screen::FactoryConfirm: renderFactoryConfirm(); break;
        case Screen::ChangePin:      renderChangePin();      break;
        case Screen::CdrDetail:      renderCdrDetail();      break;
        case Screen::PbxConfig:      renderPbxConfig();      break;
        case Screen::PbxAddMenu:     renderPbxAddMenu();     break;
        case Screen::PbxAddSingle:   renderPbxAddSingle();   break;
        case Screen::PbxAddRange:    renderPbxAddRange();    break;
        case Screen::PbxGroupEdit:   renderPbxGroupEdit();   break;
        case Screen::PbxGroupDelete: renderPbxGroupDelete(); break;
        case Screen::PbxForwardEdit: renderPbxForwardEdit(); break;
        case Screen::PbxForwardPick: renderPbxForwardPick(); break;
        case Screen::PbxIvrEdit:     renderPbxIvrEdit();     break;
        case Screen::Devices:        renderDevices();        break;
        case Screen::RegModePick:    renderRegModePick();    break;
        case Screen::SecretEntry:    renderSecretEntry();    break;
        case Screen::DeviceForget:   renderDeviceForget();   break;
    }
}

void Tui::toggleTheme()
{
    _theme = (_theme == Theme::Brass) ? Theme::Phosphor : Theme::Brass;
}

void Tui::begin()
{
    _running = true;
    _screen  = Screen::Banner;
    gotoScreen(Screen::Banner);
}

void Tui::tickLive()
{
    // Drive whichever live screen is active. Each is a no-op off its own screen,
    // so this is safe to call unconditionally ~1 Hz from the session loop.
    repaintHubLive();
    repaintMonitorLive();
}

// ── Input decode + dispatch ──────────────────────────────────────────────────
bool Tui::feed(const char* data, size_t len)
{
    for (size_t i = 0; i < len && _running; ++i)
    {
        if (!feedByte(static_cast<unsigned char>(data[i]))) return false;
    }
    // A bare trailing ESC (no following '[') means the operator pressed Esc:
    // resolve it now so a lone Esc is never swallowed (accessibility T2 — bare
    // Esc must always cancel; here there is no more input this read, so flush).
    if (_decode == Decode::GotEsc)
    {
        _decode = Decode::Normal;
        // Route a logical Esc to the active screen. Help returns to the screen it was
        // opened FROM (context help; tui-ia §3) — route through the per-screen key
        // handlers so this flush path and the inline GotEsc path stay identical.
        switch (_screen)
        {
            case Screen::Help:           onKeyHelp(Key::Esc, 0); break;
            case Screen::Placeholder:    onKeyPlaceholder(Key::Esc, 0); break;
            case Screen::RebootConfirm:  onKeyConfirm(Key::Esc, 0); break;
            case Screen::Monitor:        onKeyMonitor(Key::Esc, 0); break;
            case Screen::Network:        onKeyNetwork(Key::Esc, 0); break;
            case Screen::Security:       onKeySecurity(Key::Esc, 0); break;
            case Screen::Reports:        onKeyReports(Key::Esc, 0); break;
            case Screen::About:          onKeyAbout(Key::Esc, 0); break;
            case Screen::ModeConfirm:    onKeyModeConfirm(Key::Esc, 0); break;
            case Screen::FactoryConfirm: onKeyFactoryConfirm(Key::Esc, 0); break;
            case Screen::ChangePin:      onKeyChangePin(Key::Esc, 0); break;
            case Screen::CdrDetail:      onKeyCdrDetail(Key::Esc, 0); break;
            case Screen::PbxConfig:      onKeyPbxConfig(Key::Esc, 0); break;
            case Screen::PbxAddMenu:     onKeyPbxAddMenu(Key::Esc, 0); break;
            case Screen::PbxAddSingle:   onKeyPbxAddSingle(Key::Esc, 0); break;
            case Screen::PbxAddRange:    onKeyPbxAddRange(Key::Esc, 0); break;
            case Screen::PbxGroupEdit:   onKeyPbxGroupEdit(Key::Esc, 0); break;
            case Screen::PbxGroupDelete: onKeyPbxGroupDelete(Key::Esc, 0); break;
            case Screen::PbxForwardEdit: onKeyPbxForwardEdit(Key::Esc, 0); break;
            case Screen::PbxForwardPick: onKeyPbxForwardPick(Key::Esc, 0); break;
            case Screen::PbxIvrEdit:     onKeyPbxIvrEdit(Key::Esc, 0); break;
            case Screen::Devices:        onKeyDevices(Key::Esc, 0); break;
            case Screen::RegModePick:    onKeyRegModePick(Key::Esc, 0); break;
            case Screen::SecretEntry:    onKeySecretEntry(Key::Esc, 0); break;
            case Screen::DeviceForget:   onKeyDeviceForget(Key::Esc, 0); break;
            case Screen::Banner:         gotoScreen(Screen::Hub); break;
            case Screen::Hub:            /* Esc = no-op at home */ break;
        }
    }
    return _running;
}

bool Tui::feedByte(unsigned char c)
{
    // ── Escape-sequence state machine ────────────────────────────────────────
    if (_decode == Decode::GotEsc)
    {
        if (c == '[' || c == 'O')   // CSI / SS3 introducer
        {
            _decode = Decode::GotCsi;
            return _running;
        }
        // ESC followed by a non-'[' byte: treat the ESC as a logical Esc, then
        // process this byte fresh in Normal state.
        _decode = Decode::Normal;
        Key esc = Key::Esc;
        switch (_screen)
        {
            case Screen::Help:           onKeyHelp(esc, 0); break;
            case Screen::Placeholder:    onKeyPlaceholder(esc, 0); break;
            case Screen::RebootConfirm:  onKeyConfirm(esc, 0); break;
            case Screen::Monitor:        onKeyMonitor(esc, 0); break;
            case Screen::Network:        onKeyNetwork(esc, 0); break;
            case Screen::Security:       onKeySecurity(esc, 0); break;
            case Screen::Reports:        onKeyReports(esc, 0); break;
            case Screen::About:          onKeyAbout(esc, 0); break;
            case Screen::ModeConfirm:    onKeyModeConfirm(esc, 0); break;
            case Screen::FactoryConfirm: onKeyFactoryConfirm(esc, 0); break;
            case Screen::ChangePin:      onKeyChangePin(esc, 0); break;
            case Screen::CdrDetail:      onKeyCdrDetail(esc, 0); break;
            case Screen::PbxConfig:      onKeyPbxConfig(esc, 0); break;
            case Screen::PbxAddMenu:     onKeyPbxAddMenu(esc, 0); break;
            case Screen::PbxAddSingle:   onKeyPbxAddSingle(esc, 0); break;
            case Screen::PbxAddRange:    onKeyPbxAddRange(esc, 0); break;
            case Screen::PbxGroupEdit:   onKeyPbxGroupEdit(esc, 0); break;
            case Screen::PbxGroupDelete: onKeyPbxGroupDelete(esc, 0); break;
            case Screen::PbxForwardEdit: onKeyPbxForwardEdit(esc, 0); break;
            case Screen::PbxForwardPick: onKeyPbxForwardPick(esc, 0); break;
            case Screen::PbxIvrEdit:     onKeyPbxIvrEdit(esc, 0); break;
            case Screen::Devices:        onKeyDevices(esc, 0); break;
            case Screen::RegModePick:    onKeyRegModePick(esc, 0); break;
            case Screen::SecretEntry:    onKeySecretEntry(esc, 0); break;
            case Screen::DeviceForget:   onKeyDeviceForget(esc, 0); break;
            case Screen::Hub:            onKeyHub(esc, 0); break;
            case Screen::Banner:         gotoScreen(Screen::Hub); break;
        }
        // fall through to handle `c` below
    }
    else if (_decode == Decode::GotCsi)
    {
        _decode = Decode::Normal;
        Key k = Key::None;
        switch (c)
        {
            case 'A': k = Key::Up;    break;
            case 'B': k = Key::Down;  break;
            case 'C': k = Key::Right; break;
            case 'D': k = Key::Left;  break;
            default:  k = Key::None;  break;   // ignore unhandled CSI finals
        }
        if (k != Key::None)
        {
            switch (_screen)
            {
                case Screen::Hub:            onKeyHub(k, 0); break;
                case Screen::Help:           onKeyHelp(k, 0); break;
                case Screen::RebootConfirm:  onKeyConfirm(k, 0); break;
                case Screen::Placeholder:    onKeyPlaceholder(k, 0); break;
                case Screen::Monitor:        onKeyMonitor(k, 0); break;
                case Screen::Network:        onKeyNetwork(k, 0); break;
                case Screen::Security:       onKeySecurity(k, 0); break;
                case Screen::Reports:        onKeyReports(k, 0); break;
                case Screen::About:          onKeyAbout(k, 0); break;
                case Screen::ModeConfirm:    onKeyModeConfirm(k, 0); break;
                case Screen::FactoryConfirm: onKeyFactoryConfirm(k, 0); break;
                case Screen::ChangePin:      onKeyChangePin(k, 0); break;
                case Screen::CdrDetail:      onKeyCdrDetail(k, 0); break;
                case Screen::PbxConfig:      onKeyPbxConfig(k, 0); break;
                case Screen::PbxAddMenu:     onKeyPbxAddMenu(k, 0); break;
                case Screen::PbxAddSingle:   onKeyPbxAddSingle(k, 0); break;
                case Screen::PbxAddRange:    onKeyPbxAddRange(k, 0); break;
                case Screen::PbxGroupEdit:   onKeyPbxGroupEdit(k, 0); break;
                case Screen::PbxGroupDelete: onKeyPbxGroupDelete(k, 0); break;
                case Screen::PbxForwardEdit: onKeyPbxForwardEdit(k, 0); break;
                case Screen::PbxForwardPick: onKeyPbxForwardPick(k, 0); break;
                case Screen::PbxIvrEdit:     onKeyPbxIvrEdit(k, 0); break;
                case Screen::Devices:        onKeyDevices(k, 0); break;
                case Screen::RegModePick:    onKeyRegModePick(k, 0); break;
                case Screen::SecretEntry:    onKeySecretEntry(k, 0); break;
                case Screen::DeviceForget:   onKeyDeviceForget(k, 0); break;
                case Screen::Banner:         break;
            }
        }
        return _running;
    }

    // ── Normal byte ──────────────────────────────────────────────────────────
    if (c == 0x1b) { _decode = Decode::GotEsc; return _running; }

    Key k = Key::Char;
    if (c == '\r' || c == '\n') k = Key::Enter;
    else if (c == 0x7f || c == 0x08) k = Key::Backspace;
    else if (c == 0x0c) k = Key::CtrlL;     // Ctrl-L → redraw
    else if (c == '\t') k = Key::Tab;

    // Banner: any key advances into the hub (brief greeting → hub).
    if (_screen == Screen::Banner)
    {
        gotoScreen(Screen::Hub);
        return _running;
    }

    switch (_screen)
    {
        case Screen::Hub:            onKeyHub(k, c); break;
        case Screen::Help:           onKeyHelp(k, c); break;
        case Screen::RebootConfirm:  onKeyConfirm(k, c); break;
        case Screen::Placeholder:    onKeyPlaceholder(k, c); break;
        case Screen::Monitor:        onKeyMonitor(k, c); break;
        case Screen::Network:        onKeyNetwork(k, c); break;
        case Screen::Security:       onKeySecurity(k, c); break;
        case Screen::Reports:        onKeyReports(k, c); break;
        case Screen::About:          onKeyAbout(k, c); break;
        case Screen::ModeConfirm:    onKeyModeConfirm(k, c); break;
        case Screen::FactoryConfirm: onKeyFactoryConfirm(k, c); break;
        case Screen::ChangePin:      onKeyChangePin(k, c); break;
        case Screen::CdrDetail:      onKeyCdrDetail(k, c); break;
        case Screen::PbxConfig:      onKeyPbxConfig(k, c); break;
        case Screen::PbxAddMenu:     onKeyPbxAddMenu(k, c); break;
        case Screen::PbxAddSingle:   onKeyPbxAddSingle(k, c); break;
        case Screen::PbxAddRange:    onKeyPbxAddRange(k, c); break;
        case Screen::PbxGroupEdit:   onKeyPbxGroupEdit(k, c); break;
        case Screen::PbxGroupDelete: onKeyPbxGroupDelete(k, c); break;
        case Screen::PbxForwardEdit: onKeyPbxForwardEdit(k, c); break;
        case Screen::PbxForwardPick: onKeyPbxForwardPick(k, c); break;
        case Screen::PbxIvrEdit:     onKeyPbxIvrEdit(k, c); break;
        case Screen::Devices:        onKeyDevices(k, c); break;
        case Screen::RegModePick:    onKeyRegModePick(k, c); break;
        case Screen::SecretEntry:    onKeySecretEntry(k, c); break;
        case Screen::DeviceForget:   onKeyDeviceForget(k, c); break;
        case Screen::Banner:         break;   // handled above
    }
    return _running;
}

// Hub typeahead (tui-ia §3.2): single key, no Enter.
void Tui::onKeyHub(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::Hub); return; }  // redraw
    if (k == Key::Esc)   { /* no-op at home (tui-ia §0/D7) */ return; }

    if (k == Key::Char)
    {
        switch (ch)
        {
            case '1':
                gotoScreen(Screen::Monitor);  // router un-freezes on fresh entry
                return;
            case '2':
                gotoScreen(Screen::Network);
                return;
            case '4':
                gotoScreen(Screen::Security);
                return;
            case '5':
                _reportsEventLog = false; _cdrSel = 0; _cdrTop = 0;  // open on Recent Calls
                gotoScreen(Screen::Reports);
                return;
            case '6':
                gotoScreen(Screen::About);
                return;
            case '3':                           // PBX CONFIG — the tabbed panel (B2b-4)
                _pbxTab = PbxTab::Extensions;   // 3 → 1 lands on the default tab (IA §2)
                _pbxSel = 0; _pbxTop = 0;
                gotoScreen(Screen::PbxConfig);
                return;
            case 'R': case 'r':
                gotoScreen(Screen::RebootConfirm);
                return;
            case 'L': case 'l':
                _running = false;          // logout → SshServer tears down
                return;
            case 'T': case 't':
                toggleTheme();
                gotoScreen(Screen::Hub);   // repaint with the new palette + label
                return;
            case '?':
                gotoScreen(Screen::Help);
                return;
            default:
                return;                    // ignore unbound keys (no beep spam)
        }
    }
}

void Tui::onKeyHelp(Key k, unsigned char ch)
{
    (void)ch;
    if (k == Key::CtrlL) { gotoScreen(Screen::Help); return; }
    // Any Esc (or '?') dismisses help back to the screen it was opened FROM
    // (tui-ia §3: context help is scoped to the current screen; Esc returns there,
    // it does not jump home).
    if (k == Key::Esc) { gotoScreen(_helpReturn); return; }
    if (k == Key::Char && ch == '?') { gotoScreen(_helpReturn); return; }
}

void Tui::onKeyConfirm(Key k, unsigned char ch)
{
    // Safe default ([ Stay up ]) pre-focused. Esc / n / N → cancel (safe).
    // Enter on the focused safe button or y/Y → execute. (For B2b-1 we keep the
    // safe button focused and treat Enter as "Stay up"; y/Y is the explicit
    // reboot shortcut per the [y/N] inline contract.)
    if (k == Key::CtrlL) { gotoScreen(Screen::RebootConfirm); return; }
    if (k == Key::Esc) { gotoScreen(Screen::Hub); return; }

    if (k == Key::Char)
    {
        if (ch == 'y' || ch == 'Y')
        {
            // Confirmed reboot.
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
            // Tell the operator, then restart. The channel teardown is implicit
            // (the board resets); SshServer's loop will see the disconnect.
            put("\r\n");
            roled(Role::Alert, "Rebooting\xe2\x80\xa6 phones drop for ~8s.\r\n");
            esp_restart();
#else
            // Host build: there is nothing to reboot — just return home.
            gotoScreen(Screen::Hub);
#endif
            return;
        }
        if (ch == 'n' || ch == 'N')
        {
            gotoScreen(Screen::Hub);       // safe
            return;
        }
    }
    if (k == Key::Enter)
    {
        // Enter confirms the FOCUSED choice, which is the safe default.
        gotoScreen(Screen::Hub);
        return;
    }
    // [←/→] would move focus between buttons in a later increment; for B2b-1 the
    // safe button stays focused and the explicit y/n shortcuts drive the action.
    (void)0;
}

void Tui::onKeyPlaceholder(Key k, unsigned char ch)
{
    (void)ch;
    if (k == Key::CtrlL) { gotoScreen(Screen::Placeholder); return; }
    if (k == Key::Esc) { gotoScreen(Screen::Hub); return; }
    // [?] help is reachable even from a placeholder (global contract).
    if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
}

// Live monitor keys (tui-ia §3.6): [F] freeze/unfreeze the 1 Hz refresh, [C] clear
// stale rows, [Esc] back to hub, [?] help, Ctrl-L redraw. [P] is intentionally
// UNbound (no on-device PCAP — honesty clause).
void Tui::onKeyMonitor(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::Monitor); return; }   // full redraw
    if (k == Key::Esc)   { gotoScreen(Screen::Hub); return; }       // back to hub
    if (k == Key::Char)
    {
        switch (ch)
        {
            case 'F': case 'f':
                _monFrozen = !_monFrozen;   // toggle the 1 Hz repaint
                gotoScreen(Screen::Monitor);  // redraw so the badge flips ⟳↔FROZEN
                return;
            case 'C': case 'c':
                // Clear stale/torn-down rows. For B2b-2 the snapshot is rebuilt
                // each draw (no persistent stale-row store yet), so a clear is a
                // fresh repaint — the visible effect once a torn-down row exists.
                gotoScreen(Screen::Monitor);
                return;
            case '?':
                gotoScreen(Screen::Help);
                return;
            default:
                return;                     // ignore unbound keys (incl. P — honesty)
        }
    }
}

// ── [2] NETWORK keys (tui-ia §3.2): [M] guarded mode switch, Esc back, ? help ──
void Tui::onKeyNetwork(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::Network); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::Hub); return; }
    if (k == Key::Char)
    {
        switch (ch)
        {
            case 'M': case 'm': gotoScreen(Screen::ModeConfirm); return;
            case '?':           gotoScreen(Screen::Help);        return;
            default:            return;
        }
    }
}

// ── [2]/[M] mode-switch confirm (guarded; reboots). Safe default focused. ──────
void Tui::onKeyModeConfirm(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::ModeConfirm); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::Network); return; }   // cancel → back
    if (k == Key::Char)
    {
        if (ch == 'y' || ch == 'Y') { applyModeSwitch(); return; }
        if (ch == 'n' || ch == 'N') { gotoScreen(Screen::Network); return; }
        if (ch == '?')              { gotoScreen(Screen::Help); return; }
    }
    if (k == Key::Enter) { gotoScreen(Screen::Network); return; }   // focused = safe
}

// Toggle wifi_mode (STATION 1 ↔ AP 2) in NVS and restart. Mirrors cmd_set_topology
// (SshServer.cpp). SETUP (0) is treated as "→ STATION" since the operator is asking
// to join a network. Host build: nothing to switch — return to the network screen.
void Tui::applyModeSwitch()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    LiveStats st = stats();
    uint8_t next = (st.netMode == 1) ? 2 : 1;   // STATION→AP, else →STATION
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_u8(h, "wifi_mode", next);
        nvs_commit(h);
        nvs_close(h);
    }
    put("\r\n");
    roled(Role::Alert, "Switching network mode\xe2\x80\xa6 Wi-Fi drops for ~8s.\r\n");
    esp_restart();
#else
    gotoScreen(Screen::Network);
#endif
}

// ── [4] SECURITY keys: [P] change PIN, [K] SSH toggle, [X] factory reset, etc. ─
void Tui::onKeySecurity(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::Security); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::Hub); return; }
    if (k == Key::Char)
    {
        switch (ch)
        {
            case 'P': case 'p':
                _pinCur.clear(); _pinNew.clear(); _pinConf.clear();
                _pinField = 0; _pinMsg.clear();
                gotoScreen(Screen::ChangePin);
                return;
            case 'K': case 'k':
            {
                // Toggle SSH access. Read the current state from the provider, flip it.
                SecurityInfo si = security();
                if (_sshToggle) _sshToggle(!si.sshEnabled);
                gotoScreen(Screen::Security);   // repaint with the new chip
                return;
            }
            case 'D': case 'd':
                _devSel = 0; _devTop = 0; _devMsg.clear();
                gotoScreen(Screen::Devices);
                return;
            case 'X': case 'x':
                _factoryStep = 0;               // start the double-confirm at step 1
                gotoScreen(Screen::FactoryConfirm);
                return;
            case '?':
                gotoScreen(Screen::Help);
                return;
            default:
                return;
        }
    }
}

// ── [4]/[X] factory reset DOUBLE-confirm. Two affirmatives erase + restart. ────
void Tui::onKeyFactoryConfirm(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::FactoryConfirm); return; }
    if (k == Key::Esc)   { _factoryStep = 0; gotoScreen(Screen::Security); return; }
    if (k == Key::Char)
    {
        if (ch == 'y' || ch == 'Y')
        {
            if (_factoryStep == 0)
            {
                _factoryStep = 1;               // advance to the final confirm
                gotoScreen(Screen::FactoryConfirm);
            }
            else
            {
                applyFactoryReset();            // second yes → erase + restart
            }
            return;
        }
        if (ch == 'n' || ch == 'N') { _factoryStep = 0; gotoScreen(Screen::Security); return; }
        if (ch == '?')              { gotoScreen(Screen::Help); return; }
    }
    if (k == Key::Enter) { _factoryStep = 0; gotoScreen(Screen::Security); return; }  // safe
}

// nvs_flash_erase() + esp_restart() — mirrors cmd_factory_reset (SshServer.cpp).
// Host build: nothing to erase — return to the security screen.
void Tui::applyFactoryReset()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    put("\r\n");
    roled(Role::Alert, "Factory reset\xe2\x80\xa6 erasing NVS and restarting.\r\n");
    nvs_flash_erase();
    esp_restart();
#else
    _factoryStep = 0;
    gotoScreen(Screen::Security);
#endif
}

// ── [4]/[P] change-PIN modal keys (never echoed; §3.10.1). Tab cycles fields, ──
// digits append to the focused field, Backspace edits, Enter applies, Esc cancels.
void Tui::onKeyChangePin(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::ChangePin); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::Security); return; }
    auto curField = [&]() -> std::string& {
        return (_pinField == 0) ? _pinCur : (_pinField == 1) ? _pinNew : _pinConf;
    };
    if (k == Key::Tab || k == Key::Down) { _pinField = (_pinField + 1) % 3; _pinMsg.clear(); gotoScreen(Screen::ChangePin); return; }
    if (k == Key::Up)                    { _pinField = (_pinField + 2) % 3; _pinMsg.clear(); gotoScreen(Screen::ChangePin); return; }
    if (k == Key::Backspace)
    {
        std::string& f = curField();
        if (!f.empty()) f.pop_back();
        _pinMsg.clear();
        gotoScreen(Screen::ChangePin);
        return;
    }
    if (k == Key::Enter)
    {
        // Apply: validate locally, then hand current+new to the wired changer.
        if (_pinNew.size() < 4 || _pinNew.size() > 10) { _pinMsg = "New PIN must be 4\xe2\x80\x93" "10 digits."; gotoScreen(Screen::ChangePin); return; }
        if (_pinNew != _pinConf)                       { _pinMsg = "New PIN and confirm do not match."; gotoScreen(Screen::ChangePin); return; }
        if (!_pinChanger)                              { _pinMsg = "PIN change not available on this build."; gotoScreen(Screen::ChangePin); return; }
        std::string err = _pinChanger(_pinCur, _pinNew);
        if (err.empty())
        {
            _pinCur.clear(); _pinNew.clear(); _pinConf.clear(); _pinField = 0; _pinMsg.clear();
            gotoScreen(Screen::Security);   // success → back to the security screen
        }
        else
        {
            _pinMsg = err;                  // show the changer's error inline
            gotoScreen(Screen::ChangePin);
        }
        return;
    }
    if (k == Key::Char)
    {
        if (ch == '?') { gotoScreen(Screen::Help); return; }
        // Accept digits only (PINs are numeric); cap each field at 10 chars.
        if (ch >= '0' && ch <= '9')
        {
            std::string& f = curField();
            if (f.size() < 10) f.push_back(static_cast<char>(ch));
            _pinMsg.clear();
            gotoScreen(Screen::ChangePin);
        }
        return;
    }
}

// ── [5] REPORTS keys: [Tab] flips view, [↑/↓] select, [Enter] detail, Esc back ─
void Tui::onKeyReports(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::Reports); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::Hub); return; }
    if (k == Key::Tab)   { _reportsEventLog = !_reportsEventLog; gotoScreen(Screen::Reports); return; }
    if (!_reportsEventLog)   // Recent Calls view drives the selection/detail
    {
        if (k == Key::Up)    { if (_cdrSel > 0) --_cdrSel; gotoScreen(Screen::Reports); return; }
        if (k == Key::Down)  { ++_cdrSel; gotoScreen(Screen::Reports); return; }  // clamped in render
        if (k == Key::Enter) { gotoScreen(Screen::CdrDetail); return; }
    }
    if (k == Key::Char)
    {
        if (ch == '?')                 { gotoScreen(Screen::Help); return; }
        if (ch == '\t')                { _reportsEventLog = !_reportsEventLog; gotoScreen(Screen::Reports); return; }
    }
}

// ── [5] CDR detail modal keys: [↑/↓] walk records, Esc back to the list ────────
void Tui::onKeyCdrDetail(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::CdrDetail); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::Reports); return; }
    if (k == Key::Up)    { if (_cdrSel > 0) --_cdrSel; gotoScreen(Screen::CdrDetail); return; }
    if (k == Key::Down)  { ++_cdrSel; gotoScreen(Screen::CdrDetail); return; }   // clamped in render
    if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
}

// ── [6] ABOUT keys: Esc back, ? help (a static honesty card; no other actions) ─
void Tui::onKeyAbout(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::About); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::Hub); return; }
    if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
}

// ═══════════════════════════════════════════════════════════════════════════════
// [4]/[D] REGISTRAR · DEVICES — digest-auth admission mode + adopted-device roster
// (STAGE 3). One screen surfaces the runtime RegistrarMode (Standalone/Learn/New) and
// the getAdoptedDevices() list with the device-state lexicon (● ONLINE / ◐ LEARNED /
// ◆ SECURED — glyph+LABEL, never colour alone). Actions: [M] change mode (a chooser),
// [A] assign/rotate the selected device's SIP secret (never-echoed modal), [S] secure/
// lock a learned device, [F] forget (guarded). All wired to RequestsHandler/SipSecret-
// Store via the STAGE-2 accessors. Every framed row pads via dispWidth(); tabular
// cells route through padCols().
// ═══════════════════════════════════════════════════════════════════════════════

// Map a RegMode → its operator-facing word + one-line description (shared by the
// Devices header and the mode chooser so the copy never drifts).
static const char* regModeWord(Tui::RegMode m)
{
    switch (m)
    {
        case Tui::RegMode::Open:   return "STANDALONE (open)";
        case Tui::RegMode::Learn:  return "LEARN (adopt phones)";
        case Tui::RegMode::Secure: return "NEW (secure)";
    }
    return "STANDALONE (open)";
}
static const char* regModeBlurb(Tui::RegMode m)
{
    switch (m)
    {
        case Tui::RegMode::Open:
            return "Accept every REGISTER, no password. Fastest bring-up; trust the LAN.";
        case Tui::RegMode::Learn:
            return "Adopt unknown phones on sight, lock each to its MAC. Migrate live.";
        case Tui::RegMode::Secure:
            return "Require a SIP digest password for every provisioned extension.";
    }
    return "";
}

void Tui::renderDevices()
{
    LiveStats st = stats();
    RegistrarInfo ri = registrar();
    clearScreen();
    hideCursor();
    drawSpineTop("SECURITY", st);

    const std::string v = glyph(Glyph::BoxV);
    const std::string dot = _unicode ? "\xc2\xb7" : ".";
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    auto rule = [&](int n) {
        std::string r = " ";
        const char* H = glyph(Glyph::BoxH);
        for (int i = 0; i < n; ++i) r += H;
        bodyRow(col(Role::Border, r));
    };

    int bodyUsed = 0;
    auto emit = [&](const std::string& body) { bodyRow(body); ++bodyUsed; };

    // ── Mode block ──
    emit(col(Role::Header, " REGISTRAR MODE"));
    rule(14); ++bodyUsed;
    {
        std::string body = col(Role::Text, " Mode ......... ");
        if (_color) body += std::string("\x1b[") + sgrFor(Role::Lamp) + "m";
        body += glyph(Glyph::Active);   // ◆ accent
        body += " ";
        body += regModeWord(ri.mode);
        if (_color) body += "\x1b[0m";
        emit(body);
    }
    emit(col(Role::Dim, std::string(" ") + regModeBlurb(ri.mode)));
    {
        std::string body = col(Role::Text, " Realm ........ ");
        body += col(Role::Text, ri.realm);
        body += col(Role::Dim, "   (digest auth realm)");
        emit(body);
    }
    bodyBlank(); ++bodyUsed;

    // ── Adopted-device roster ──
    emit(col(Role::Header, " ADOPTED DEVICES"));
    // Column header: MAC(14) EXT(6) STATE(...).
    emit(col(Role::Header, " MAC             EXT     STATE"));
    {
        const char* H = glyph(Glyph::BoxH);
        std::string r = " ";
        auto run = [&](int n) { for (int i = 0; i < n; ++i) r += H; };
        run(14); r += "  "; run(5); r += "  "; run(18);
        bodyRow(col(Role::Border, r)); ++bodyUsed;
    }

    int total = (int)ri.devices.size();
    if (_devSel >= total) _devSel = total > 0 ? total - 1 : 0;
    if (_devSel < 0) _devSel = 0;
    if (_devSel < _devTop) _devTop = _devSel;
    if (_devSel >= _devTop + kDevRows) _devTop = _devSel - kDevRows + 1;
    if (_devTop < 0) _devTop = 0;

    // Resolve a device row → its lexicon (glyph, role, label). Online beats the stored
    // state for the live chip; the persisted Secured/Learned state is the base word.
    auto devChip = [&](const DeviceRow& d, std::string& out, int& vis) {
        Glyph g; Role r; const char* lbl;
        if (d.secured)      { g = Glyph::Active;  r = Role::Lamp; lbl = "SECURED"; }   // ◆
        else                { g = Glyph::Ringing; r = Role::Dim;  lbl = "LEARNED"; }   // ◐
        if (_color) out += std::string("\x1b[") + sgrFor(r) + "m";
        out += glyph(g); vis += _unicode ? 1 : (int)std::strlen(glyph(g));
        out += " "; out += lbl; vis += 1 + (int)std::strlen(lbl);
        if (_color) out += "\x1b[0m";
        // Live ONLINE marker appended after the base state word.
        out += "  "; vis += 2;
        if (d.online)
        {
            if (_color) out += std::string("\x1b[") + sgrFor(Role::Lamp) + "m";
            out += glyph(Glyph::Online); vis += _unicode ? 1 : (int)std::strlen(glyph(Glyph::Online));
            out += " ONLINE"; vis += 7;
            if (_color) out += "\x1b[0m";
        }
        else
        {
            out += col(Role::Dim, dot); vis += 1;
        }
    };

    if (total == 0)
    {
        emit(col(Role::Dim, " (no devices adopted yet \xe2\x80\x94 set Learn mode and register a phone)"));
        for (int i = 0; i < kDevRows - 1; ++i) { bodyBlank(); ++bodyUsed; }
    }
    else
    {
        for (int i = 0; i < kDevRows; ++i)
        {
            int idx = _devTop + i;
            if (idx >= total) { bodyBlank(); ++bodyUsed; continue; }
            const DeviceRow& d = ri.devices[idx];
            bool sel = (idx == _devSel);

            std::string grid;
            grid += (sel ? (_unicode ? "\xe2\x96\xb8" : ">") : " ");   // ▸ selector
            grid += " ";
            grid += padCols(d.mac.empty() ? (_unicode ? "\xe2\x80\x94" : "-") : d.mac, 14) + "  ";
            grid += padCols(d.ext.empty() ? (_unicode ? "\xe2\x80\x94" : "-") : d.ext, 5) + "  ";

            if (sel && _color)
            {
                // One reverse span over the row (selection = position+▸, not colour).
                std::string st2; int vis2 = 0;
                // Plain (uncoloured) chip inside the reverse span.
                const char* lbl = d.secured ? "SECURED" : "LEARNED";
                const char* g   = d.secured ? glyph(Glyph::Active) : glyph(Glyph::Ringing);
                st2 += g; st2 += " "; st2 += lbl; vis2 += (_unicode ? 1 : (int)std::strlen(g)) + 1 + (int)std::strlen(lbl);
                st2 += "  "; vis2 += 2;
                if (d.online) { st2 += std::string(glyph(Glyph::Online)) + " ONLINE"; vis2 += (_unicode ? 1 : 3) + 7; }
                else          { st2 += dot; vis2 += 1; }
                std::string line = "\x1b[7m" + grid + st2 + "\x1b[0m";
                bodyRow(line); ++bodyUsed;
            }
            else
            {
                std::string line = col(Role::Text, grid);
                std::string chip; int vis = 0; devChip(d, chip, vis);
                line += chip;
                bodyRow(line); ++bodyUsed;
            }
        }
    }

    // Headroom + inline result line.
    {
        char b[24]; std::snprintf(b, sizeof(b), "dev %d", total);
        std::string body;
        int gap = (kCols - 2) - 1 - (int)std::strlen(b) - 2;
        if (gap < 1) gap = 1;
        body += std::string((size_t)gap, ' ');
        body += col(Role::Text, b);
        bodyRow(body); ++bodyUsed;
    }
    // Always reserve the result row (blank when empty) so the body row count is
    // stable at 18 whether or not a message is showing — a non-reserved row pushed
    // the frame past the 24-line floor when _devMsg was set.
    if (_devMsg.empty()) { bodyBlank(); ++bodyUsed; }
    else                 { emit(col(Role::Text, " " + _devMsg)); }

    for (int i = bodyUsed; i < 18; ++i) bodyBlank();
    drawFooter("[M] Mode  [A] Secret  [S] Secure  [F] Forget  [?] Help");
    _lastClock = fmtClock(st.uptimeSec);
}

// ── [4]/[D]/[M] registrar mode chooser (Standalone / Learn / New) ──────────────
void Tui::renderRegModePick()
{
    LiveStats st = stats();
    RegistrarInfo ri = registrar();
    clearScreen();
    hideCursor();
    drawSpineTop("SECURITY", st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };

    int bodyUsed = 0;
    auto emit = [&](const std::string& body) { bodyRow(body); ++bodyUsed; };

    emit(col(Role::Header, " CHOOSE REGISTRAR MODE"));
    {
        std::string r = " ";
        const char* H = glyph(Glyph::BoxH);
        for (int i = 0; i < 21; ++i) r += H;
        bodyRow(col(Role::Border, r)); ++bodyUsed;
    }
    emit(col(Role::Dim, " How new phones are admitted. Choose, then [Enter] to apply."));
    bodyBlank(); ++bodyUsed;

    auto option = [&](int idx, RegMode m) {
        bool sel = (idx == _regPickSel);
        bool cur = (m == ri.mode);
        // Header line: ▸ <radio> WORD            (current)
        std::string head;
        head += (sel ? (_unicode ? "\xe2\x96\xb8" : ">") : " ");   // ▸ selector
        head += " ";
        head += (cur ? (_unicode ? "(\xe2\x97\x89)" : "(*)") : "( )");  // (◉) radio
        head += " ";
        head += regModeWord(m);
        if (sel && _color)
            bodyRow("\x1b[7m" + head + "\x1b[0m");
        else
            bodyRow(col(Role::Text, head));
        ++bodyUsed;
        // Description line (dim, indented under the option).
        emit(col(Role::Dim, std::string("      ") + regModeBlurb(m)));
        if (cur) { emit(col(Role::Dim, "      \xe2\x86\xb3 current mode")); }
        bodyBlank(); ++bodyUsed;
    };
    option(0, RegMode::Open);
    option(1, RegMode::Learn);
    option(2, RegMode::Secure);

    if (!_regModeSet)
        emit(col(Role::Alert, " Mode change not available on this build."));

    for (int i = bodyUsed; i < 18; ++i) bodyBlank();
    drawFooter(_unicode ? "[\xe2\x86\x91/\xe2\x86\x93] Choose  [Enter] Apply  [Esc] Cancel  [?] Help"
                        : "[Up/Dn] Choose  [Enter] Apply  [Esc] Cancel  [?] Help");
    _lastClock = fmtClock(st.uptimeSec);
}

// ── Never-echoed SIP secret assign/rotate modal (shared by Devices + Extensions) ─
// Mirrors renderChangePin's centered box + bullet-rendered (never echoed) entry.
void Tui::renderSecretEntry()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop(_secretReturn == Screen::PbxConfig ? "PBX CONFIG" : "SECURITY", st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };

    const int boxW = 56;
    const int indent = (kCols - 2 - boxW) / 2;
    const char* TL = glyph(Glyph::BoxTL); const char* TR = glyph(Glyph::BoxTR);
    const char* BL = glyph(Glyph::BoxBL); const char* BR = glyph(Glyph::BoxBR);
    const char* VR = glyph(Glyph::BoxVR); const char* VL = glyph(Glyph::BoxVL);
    const std::string H = glyph(Glyph::BoxH);

    auto frame = [&](bool top, bool sep, const char* title) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        if (sep) put(VR); else put(top ? TL : BL);
        int used = 0;
        if (top && title) { put(H); put(" "); put(title); put(" "); used = 1 + 1 + dispWidth(title) + 1; }
        for (int i = 0; i < boxW - 2 - used; ++i) put(H);
        if (sep) put(VL); else put(top ? TR : BR);
        if (_color) put("\x1b[0m");
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto line = [&](const std::string& inner) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        put(" ");
        put(inner);
        int ipad = (boxW - 2) - 1 - dispWidth(inner); if (ipad < 0) ipad = 0;
        put(std::string((size_t)ipad, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };

    bodyBlank(); bodyBlank();
    std::string title = "SIP secret \xc2\xb7 ext " + _secretExt;
    frame(true, false, title.c_str());
    // Split the note across two lines: the full string is 62 cols but the box inner
    // is only 53 (boxW 56 − 2 frame − 1 lead), so a single line broke the rail.
    line(col(Role::Dim, "Stored as HA1=MD5(ext:realm:secret)."));
    line(col(Role::Dim, "Shown once \xe2\x80\x94 never again."));
    line("");
    // The never-echoed entry box (bullets, like the PIN modal). Stable 34-cell width.
    {
        std::string inner = col(Role::Text, "Secret ");
        int dots = 8; inner += col(Role::Text, std::string((size_t)dots, '.'));
        inner += " ";
        std::string bul;
        for (size_t i = 0; i < _secretBuf.size(); ++i) bul += _unicode ? "\xe2\x80\xa2" : "*";
        std::string boxIn = std::string("[ ") + bul;
        int padN = 32 - (int)_secretBuf.size(); if (padN < 0) padN = 0;
        boxIn += std::string((size_t)padN, ' ');
        boxIn += " ]";
        if (_color) inner += "\x1b[7m";
        inner += col(Role::Text, boxIn);
        if (_color) inner += "\x1b[0m";
        line(inner);
    }
    line("");
    if (!_secretMsg.empty())
        line(col(Role::Alert, _secretMsg));
    else
        line(col(Role::Dim, "6\xe2\x80\x93" "32 chars \xc2\xb7 never echoed \xc2\xb7 [G] generate one"));
    line("");
    {
        std::string inner = "        ";
        if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += "< Apply >";
        if (_color) inner += "\x1b[0m";
        inner += "        ";
        if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += "[ Cancel ]";
        if (_color) inner += "\x1b[0m";
        line(inner);
    }
    frame(false, true, nullptr);
    line(col(Role::Dim, "[G] Generate  [Enter] Apply  [Esc] Cancel  [?] Help"));
    frame(false, false, nullptr);

    // Body rows: 2 blank + frame + note(2) + blank + entry + blank + msg + blank +
    // buttons + frame + key + frame = 14. Pad to 18.
    for (int i = 14; i < 18; ++i) bodyBlank();
    drawFooter("[G] Generate  [Enter] Apply  [Esc] Cancel  [?] Help");
}

// ── [4]/[D]/[F] guarded forget-device confirm (shared confirm box) ─────────────
void Tui::renderDeviceForget()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("SECURITY", st);

    std::string l1 = "Forget device " + _devForgetTarget;
    if (!_devForgetExt.empty()) l1 += " (ext " + _devForgetExt + ")?";
    else                        l1 += "?";
    std::vector<std::string> lines = {
        l1,
        "Drops its adoption record. In Learn mode the next",
        "REGISTER re-learns it as a fresh, unsecured device."
    };
    std::string alert = std::string(glyph(Glyph::Alert)) + " ALERT   FORGET DEVICE";
    drawConfirmBox("Confirm forget device", alert.c_str(), Role::Alert, lines,
                   "< Forget >", "[ Keep ]", /*destructiveFocused=*/false,
                   _unicode ? "[\xe2\x86\x90/\xe2\x86\x92] Choose   [Enter] Confirm   y/N   [Esc] Cancel"
                            : "[</>] Choose   [Enter] Confirm   y/N   [Esc] Cancel");
    drawFooter("[\xe2\x86\x90/\xe2\x86\x92] Choose  [Enter] Confirm  [Esc] Cancel");
}

// ── [4]/[D] Devices keys: [M] mode, [A] secret, [S] secure, [F] forget ─────────
void Tui::onKeyDevices(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::Devices); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::Security); return; }
    if (k == Key::Up)    { if (_devSel > 0) --_devSel; _devMsg.clear(); gotoScreen(Screen::Devices); return; }
    if (k == Key::Down)  { ++_devSel; _devMsg.clear(); gotoScreen(Screen::Devices); return; }   // clamped in render

    if (k == Key::Char)
    {
        if (ch == '?') { gotoScreen(Screen::Help); return; }
        if (ch == 'M' || ch == 'm')
        {
            // Seed the chooser focus from the live mode.
            RegistrarInfo ri = registrar();
            _regPickSel = (int)ri.mode;
            gotoScreen(Screen::RegModePick);
            return;
        }
        // The device-targeted verbs need a selected row.
        RegistrarInfo ri = registrar();
        if (_devSel < 0 || _devSel >= (int)ri.devices.size())
        {
            if (ch=='A'||ch=='a'||ch=='S'||ch=='s'||ch=='F'||ch=='f')
            { _devMsg = "No device selected."; gotoScreen(Screen::Devices); }
            return;
        }
        const DeviceRow& d = ri.devices[_devSel];
        if (ch == 'A' || ch == 'a')
        {
            // Assign/rotate the SIP secret for this device's extension.
            if (d.ext.empty()) { _devMsg = "Device has no extension to secure."; gotoScreen(Screen::Devices); return; }
            _secretExt = d.ext; _secretBuf.clear(); _secretMsg.clear();
            _secretReturn = Screen::Devices;
            gotoScreen(Screen::SecretEntry);
            return;
        }
        if (ch == 'S' || ch == 's')
        {
            if (d.secured) { _devMsg = "Device is already secured."; gotoScreen(Screen::Devices); return; }
            if (!_deviceSecure) { _devMsg = "Secure not available on this build."; gotoScreen(Screen::Devices); return; }
            bool ok = _deviceSecure(d.mac);
            // A false here means the extension has no SIP secret yet (secureDevice
            // refuses to lock a phone out of an unprovisioned extension).
            _devMsg = ok ? ("Secured " + (d.ext.empty() ? d.mac : ("ext " + d.ext)) + ".")
                         : ("Assign a SIP secret to ext " + (d.ext.empty() ? d.mac : d.ext) + " first ([A]).");
            gotoScreen(Screen::Devices);
            return;
        }
        if (ch == 'F' || ch == 'f')
        {
            // Pin the victim BY MAC now — the confirm render + the y-handler each re-read
            // the live registrar, so the _devSel index alone could drift if the roster
            // changed between snapshots.
            _devForgetTarget = d.mac;
            _devForgetExt    = d.ext;
            gotoScreen(Screen::DeviceForget);
            return;
        }
    }
}

// ── [4]/[D]/[M] registrar mode chooser keys ────────────────────────────────────
void Tui::onKeyRegModePick(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::RegModePick); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::Devices); return; }
    if (k == Key::Up)    { if (_regPickSel > 0) --_regPickSel; gotoScreen(Screen::RegModePick); return; }
    if (k == Key::Down)  { if (_regPickSel < 2) ++_regPickSel; gotoScreen(Screen::RegModePick); return; }
    if (k == Key::Enter)
    {
        RegMode chosen = (RegMode)_regPickSel;
        if (_regModeSet) _regModeSet(chosen);
        _devMsg = std::string("Registrar mode \xe2\x86\x92 ") + regModeWord(chosen) + ".";
        gotoScreen(Screen::Devices);
        return;
    }
    if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
}

// ── Never-echoed secret modal keys (digits/letters append; [G] generates) ──────
void Tui::onKeySecretEntry(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::SecretEntry); return; }
    if (k == Key::Esc)   { gotoScreen(_secretReturn); return; }
    if (k == Key::Backspace)
    {
        if (!_secretBuf.empty()) _secretBuf.pop_back();
        _secretMsg.clear();
        gotoScreen(Screen::SecretEntry);
        return;
    }
    if (k == Key::Enter)
    {
        if (_secretBuf.size() < kSecretMin)
        { _secretMsg = "Secret must be at least 6 characters."; gotoScreen(Screen::SecretEntry); return; }
        if (!_secretSet)
        { _secretMsg = "Secret store not available on this build."; gotoScreen(Screen::SecretEntry); return; }
        std::string err = _secretSet(_secretExt, _secretBuf);
        if (err.empty())
        {
            // Report success on the return screen's inline line; never echo the secret.
            _devMsg = "Secret set for ext " + _secretExt + ".";
            _secretBuf.clear(); _secretMsg.clear();
            gotoScreen(_secretReturn);
        }
        else
        {
            _secretMsg = err;
            gotoScreen(Screen::SecretEntry);
        }
        return;
    }
    if (k == Key::Char)
    {
        if (ch == '?') { gotoScreen(Screen::Help); return; }
        if (ch == 'G' || ch == 'g')
        {
            // Operator chose an auto-generated secret. Use the SINGLE audited CSPRNG
            // generator (per-char esp_random()/random_device with rejection sampling,
            // ~138 bits at the default length) — never a stretched LCG. We fill the
            // entry buffer so the operator reviews length-by-bullets and [Enter]s to
            // apply; the buffer is capped at kSecretMax, so request a length that fits.
            int want = (int)kSecretMax < SipSecretStore::kGeneratedSecretLen
                       ? (int)kSecretMax : SipSecretStore::kGeneratedSecretLen;
            _secretBuf = SipSecretStore::makeRandomSecret(want);
            _secretMsg = "Generated \xe2\x80\x94 [Enter] to apply (shown once).";
            gotoScreen(Screen::SecretEntry);
            return;
        }
        // Accept printable, non-space ASCII for the secret; cap at kSecretMax.
        if (ch > 0x20 && ch < 0x7f)
        {
            if (_secretBuf.size() < kSecretMax) _secretBuf.push_back((char)ch);
            _secretMsg.clear();
            gotoScreen(Screen::SecretEntry);
        }
        return;
    }
}

// ── [4]/[D]/[F] forget-device confirm keys (guarded; safe default focused) ─────
void Tui::onKeyDeviceForget(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::DeviceForget); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::Devices); return; }   // cancel → back
    if (k == Key::Char)
    {
        if (ch == 'y' || ch == 'Y')
        {
            bool ok = _deviceForget ? _deviceForget(_devForgetTarget) : false;
            _devMsg = ok ? ("Forgot " + (_devForgetExt.empty() ? _devForgetTarget : ("ext " + _devForgetExt)) + ".")
                         : "Forget failed (not available on this build).";
            _devSel = 0; _devTop = 0;
            gotoScreen(Screen::Devices);
            return;
        }
        if (ch == 'n' || ch == 'N') { gotoScreen(Screen::Devices); return; }
        if (ch == '?')              { gotoScreen(Screen::Help); return; }
    }
    if (k == Key::Enter) { gotoScreen(Screen::Devices); return; }   // focused = safe
}

// ═══════════════════════════════════════════════════════════════════════════════
// [3] PBX CONFIG — the tabbed panel (B2b-4). tui-style §2.5 + §3.5–§3.9, IA §2/§4.
//
// One Screen::PbxConfig hosts all five tabs; `_pbxTab` selects the body. The shared
// 3-zone spine + framed-row discipline from the other screens is reused verbatim
// (drawSpineTop("PBX CONFIG") · framed rows padded via dispWidth · drawFooter). Every
// tabular cell routes through padCols() and every status through the glyph+LABEL
// lexicon (never colour alone). Modals reuse the centered-box geometry + the shared
// drawConfirmBox for guarded deletes.
//
// HONESTY (per the ticket): Ring Groups, Forwards/DND and Features are FULLY backed by
// RequestsHandler and wired. Extensions lists the live registered roster + feature
// state and wires DND/forward/group membership, but the firmware has NO persistent
// provisioning store, so the add/range flow is an explicit in-screen STUB. IVR has no
// menu store yet — the layout is built and the unsupported persistence is stub-noted.
// ═══════════════════════════════════════════════════════════════════════════════

// Shared little framed-row helpers, duplicated locally per render (the engine has no
// shared member for them — each screen defines its own bodyRow/bodyBlank/col lambdas).
// To avoid repeating five copies, these macros-as-lambdas are created inside each
// render via a common prelude. We instead factor the tab strip (which is identical
// across all five tabs) into drawPbxTabStrip(), emitting two body rows and bumping
// bodyUsed.  It assumes the caller has already drawn the spine.
void Tui::drawPbxTabStrip(int& bodyUsed)
{
    const std::string v = glyph(Glyph::BoxV);
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };

    // The five tab names + the active-tab underline (§2.5: underline AND bright name —
    // never colour alone). Separators '│' in the border role.
    static const char* kTabs[5] = { "Extensions", "Ring Groups", "Forwards/DND", "IVR", "Features" };
    int active = (int)_pbxTab;
    const std::string sep = std::string(" ") + glyph(Glyph::BoxV) + " ";
    std::string strip;
    // Track the start column of the active tab name so the underline lands under it.
    int activeStartCol = 0, activeWidth = 0, runCol = 0;
    for (int i = 0; i < 5; ++i)
    {
        if (i)
        {
            strip += col(Role::Border, sep);
            runCol += 3;   // " │ "
        }
        int w = (int)std::strlen(kTabs[i]);
        if (i == active) { activeStartCol = runCol; activeWidth = w; }
        strip += col(i == active ? Role::Header : Role::Text, kTabs[i]);
        runCol += w;
    }
    bodyRow(strip);
    ++bodyUsed;

    // Underline rule directly beneath the active tab name (the ═══ under §2.5).
    {
        std::string under;
        for (int i = 0; i < activeStartCol; ++i) under += " ";
        const char* HH = _unicode ? "\xe2\x95\x90" : "=";   // ═ / =
        std::string bar;
        for (int i = 0; i < activeWidth; ++i) bar += HH;
        under += col(Role::Header, bar);
        bodyRow(under);
        ++bodyUsed;
    }
}

// Dispatch: draw the spine + tab strip, then the active tab's body, then the footer.
void Tui::renderPbxConfig()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("PBX CONFIG", st);

    int bodyUsed = 0;
    drawPbxTabStrip(bodyUsed);

    switch (_pbxTab)
    {
        case PbxTab::Extensions: renderPbxExtensions(bodyUsed); break;
        case PbxTab::RingGroups: renderPbxRingGroups(bodyUsed); break;
        case PbxTab::Forwards:   renderPbxForwards(bodyUsed);   break;
        case PbxTab::Ivr:        renderPbxIvr(bodyUsed);        break;
        case PbxTab::Features:   renderPbxFeatures(bodyUsed);   break;
    }

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    for (int i = bodyUsed; i < 18; ++i) bodyBlank();

    // Per-tab footer (names exactly the keys live on this tab; ends in the theme
    // label via drawFooter). [←/→]/[Tab] always move tabs.
    const char* foot;
    switch (_pbxTab)
    {
        case PbxTab::Extensions:
            foot = "[\xe2\x86\x90/\xe2\x86\x92]Tabs [\xe2\x86\x91/\xe2\x86\x93]Sel [Enter]Fwd [S]Secret [A]Add [Esc]Back";
            break;
        case PbxTab::RingGroups:
            foot = "[\xe2\x86\x90/\xe2\x86\x92]Tabs [\xe2\x86\x91/\xe2\x86\x93]Sel [Enter]Edit [A]Add [D]Del [Esc]Back";
            break;
        case PbxTab::Forwards:
            foot = "[\xe2\x86\x90/\xe2\x86\x92]Tabs [\xe2\x86\x91/\xe2\x86\x93]Sel [Space]DND [Enter]Forwards [Esc]Back";
            break;
        case PbxTab::Ivr:
            foot = "[\xe2\x86\x90/\xe2\x86\x92]Tabs [\xe2\x86\x91/\xe2\x86\x93]Sel [Enter]Edit [Esc]Back";
            break;
        default:   // Features (read-only)
            foot = "[\xe2\x86\x90/\xe2\x86\x92] Tabs  [Esc] Back  [?] Help";
            break;
    }
    drawFooter(foot);
    _lastClock = fmtClock(st.uptimeSec);
}

// ── Extensions tab body (tui-style §3.5) ──────────────────────────────────────
// Lists the live REGISTERED extensions with their SIP-credential state (◆ SECURED /
// · none, from SipSecretStore), DND, and FWD status. STAGE 3 replaced the honest
// "no provisioning store" stub with a real per-extension secret state + an [S] assign/
// rotate-secret action (a never-echoed modal). [A]dd still opens the §3.5.1 submenu.
void Tui::renderPbxExtensions(int& bodyUsed)
{
    PbxConfigSnapshot cfg = pbxConfig();
    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
        ++bodyUsed;
    };
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
        ++bodyUsed;
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };

    // Column header + rule: EXT(5) NAME(9) STATE(9) SEC(9) DND(5) FWD(...).
    // SEC is 9, not 8: "◆ SECURED" is 9 display cols (◆ glyph 1 + " SECURED" 8); an
    // 8-wide cell let it bleed one col into DND on secured rows.
    bodyRow(col(Role::Header, " EXT    NAME       STATE      SEC        DND    FWD"));
    {
        const char* H = glyph(Glyph::BoxH);
        std::string r = " ";
        auto run = [&](int n) { for (int i = 0; i < n; ++i) r += H; };
        run(5); r += "  "; run(9); r += "  "; run(9); r += "  "; run(9); r += "  "; run(5); r += "  "; run(8);
        bodyRow(col(Role::Border, r));
    }

    int total = (int)cfg.extensions.size();
    if (_pbxSel >= total) _pbxSel = total > 0 ? total - 1 : 0;
    if (_pbxSel < 0) _pbxSel = 0;
    if (_pbxSel < _pbxTop) _pbxTop = _pbxSel;
    if (_pbxSel >= _pbxTop + kPbxRows) _pbxTop = _pbxSel - kPbxRows + 1;
    if (_pbxTop < 0) _pbxTop = 0;

    if (total == 0)
    {
        bodyRow(col(Role::Dim, " (no extensions registered yet \xe2\x80\x94 register a phone to populate this list)"));
        for (int i = 0; i < kPbxRows - 1; ++i) bodyBlank();
    }
    else
    {
        for (int i = 0; i < kPbxRows; ++i)
        {
            int idx = _pbxTop + i;
            if (idx >= total) { bodyBlank(); continue; }
            const PbxExt& e = cfg.extensions[idx];
            bool sel = (idx == _pbxSel);

            // Build the FWD cell: ↳ <target> for CFU if set, else group membership,
            // else "·". A group target renders "grp:<name>"; an ext target the number.
            std::string fwd;
            if (!e.cfu.empty())      fwd = e.cfu;
            else if (!e.ringGroup.empty()) fwd = "grp:" + e.ringGroup;

            // SEC chip text (◆ SECURED / · none) — the SipSecretStore credential state.
            std::string secPlain = e.secured
                ? (std::string(glyph(Glyph::Active)) + " SECURED")
                : (std::string(_unicode ? "\xc2\xb7" : ".") + " none");

            std::string grid;
            grid += (sel ? (_unicode ? "\xe2\x96\xb8" : ">") : " ");  // ▸ selector
            grid += " ";
            grid += padCols(e.ext, 5) + "  ";
            grid += padCols(e.name.empty() ? (_unicode ? "\xe2\x80\x94" : "-") : e.name, 9) + "  ";

            if (sel && _color)
            {
                // One reverse span over the row (selection = position+▸, not colour).
                std::string st;
                st += stateLabelPlain(e.state);
                st += std::string((size_t)std::max(0, 9 - dispWidth(st)), ' ') + "  ";
                st += padCols(secPlain, 9) + "  ";
                std::string dndc = e.dnd ? (std::string(glyph(Glyph::Dnd)) + " DND") : (_unicode ? "\xc2\xb7" : ".");
                st += padCols(dndc, 5) + "  ";
                std::string fwdc = fwd.empty() ? (_unicode ? "\xc2\xb7" : ".")
                                               : (std::string(glyph(Glyph::Fwd)) + " " + fwd);
                st += fwdc;
                std::string line = "\x1b[7m" + grid + st + "\x1b[0m";
                bodyRow(line);
            }
            else
            {
                std::string line = col(Role::Text, grid);
                // STATE chip (glyph+label, coloured by state).
                int vis = 0; std::string chip; appendStateChip(e.state, chip, vis);
                chip += std::string((size_t)std::max(0, 9 - vis), ' ') + "  ";
                line += chip;
                // SEC chip (◆ SECURED in accent / · none in dim) — glyph+LABEL, padded.
                if (e.secured) line += col(Role::Lamp, secPlain);
                else           line += col(Role::Dim,  secPlain);
                line += std::string((size_t)std::max(0, 9 - dispWidth(secPlain)), ' ') + "  ";
                // DND chip.
                if (e.dnd) { line += col(Role::Dnd, std::string(glyph(Glyph::Dnd)) + " DND"); line += "  "; }
                else       { line += col(Role::Dim, _unicode ? "\xc2\xb7" : "."); line += "      "; }
                // FWD chip.
                if (fwd.empty()) line += col(Role::Dim, _unicode ? "\xc2\xb7" : ".");
                else             line += col(Role::Text, std::string(glyph(Glyph::Fwd)) + " " + fwd);
                bodyRow(line);
            }
        }
    }

    // Headroom + honesty note.
    {
        char b[24]; std::snprintf(b, sizeof(b), "ext %d/%d", total, cfg.maxExt);
        std::string body;
        int gap = (kCols - 2) - 1 - (int)std::strlen(b) - 2;
        if (gap < 1) gap = 1;
        body += std::string((size_t)gap, ' ');
        body += col(Role::Text, b);
        bodyRow(body);
    }
    // Real secret-state summary (replaces the old "no provisioning store" stub).
    {
        int secured = 0;
        for (const auto& e : cfg.extensions) if (e.secured) ++secured;
        char b[48];
        std::snprintf(b, sizeof(b), " %d/%d extension%s have a SIP secret.",
                      secured, total, total == 1 ? "" : "s");
        bodyRow(col(Role::Dim, b));
    }
    bodyRow(col(Role::Dim, " [Enter] forwards/DND \xc2\xb7 [S] assign/rotate SIP secret \xc2\xb7 [A] add."));
}

// ── Ring Groups tab body (tui-style §3.6) — FULLY backed + wired ──────────────
void Tui::renderPbxRingGroups(int& bodyUsed)
{
    PbxConfigSnapshot cfg = pbxConfig();
    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
        ++bodyUsed;
    };
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
        ++bodyUsed;
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };

    bodyRow(col(Role::Header, " NAME           MODE              MEMBERS   STATUS"));
    {
        const char* H = glyph(Glyph::BoxH);
        std::string r = " ";
        auto run = [&](int n) { for (int i = 0; i < n; ++i) r += H; };
        run(12); r += "  "; run(15); r += "  "; run(7); r += "  "; run(16);
        bodyRow(col(Role::Border, r));
    }

    int total = (int)cfg.groups.size();
    if (_pbxSel >= total) _pbxSel = total > 0 ? total - 1 : 0;
    if (_pbxSel < 0) _pbxSel = 0;
    if (_pbxSel < _pbxTop) _pbxTop = _pbxSel;
    if (_pbxSel >= _pbxTop + kPbxRows) _pbxTop = _pbxSel - kPbxRows + 1;
    if (_pbxTop < 0) _pbxTop = 0;

    if (total == 0)
    {
        bodyRow(col(Role::Dim, " (no ring groups yet \xe2\x80\x94 press [A] to create one)"));
        for (int i = 0; i < kPbxRows - 1; ++i) bodyBlank();
    }
    else
    {
        for (int i = 0; i < kPbxRows; ++i)
        {
            int idx = _pbxTop + i;
            if (idx >= total) { bodyBlank(); continue; }
            const PbxGroup& g = cfg.groups[idx];
            bool sel = (idx == _pbxSel);
            // Mode reads in plain language (G3): never "RingAll/Hunt".
            std::string mode = g.ringAll ? "Ring everyone" : "One at a time";
            char mc[16]; std::snprintf(mc, sizeof(mc), "%d", (int)g.members.size());

            std::string grid;
            grid += (sel ? (_unicode ? "\xe2\x96\xb8" : ">") : " ");
            grid += " ";
            grid += padCols(g.name, 12) + "  ";
            grid += padCols(mode, 15) + "  ";
            grid += padCols(mc, 7) + "  ";

            // STATUS: ● OK, or ⚠ n NOT AN EXTENSION (G6 integrity flag, red).
            if (sel && _color)
            {
                std::string statusPlain;
                if (g.badMembers > 0)
                {
                    char wb[40]; std::snprintf(wb, sizeof(wb), "%s %d NOT AN EXTENSION",
                                               _unicode ? "\xe2\x9a\xa0" : "/!\\", g.badMembers);
                    statusPlain = wb;
                }
                else statusPlain = std::string(glyph(Glyph::Online)) + " OK";
                bodyRow(std::string("\x1b[7m") + grid + statusPlain + "\x1b[0m");
            }
            else
            {
                std::string line = col(Role::Text, grid);
                if (g.badMembers > 0)
                {
                    char wb[40]; std::snprintf(wb, sizeof(wb), "%s %d NOT AN EXTENSION",
                                               _unicode ? "\xe2\x9a\xa0" : "/!\\", g.badMembers);
                    line += col(Role::Alert, wb);
                }
                else line += col(Role::Lamp, std::string(glyph(Glyph::Online)) + " OK");
                bodyRow(line);
            }
        }
    }

    bodyRow(col(Role::Dim, " Mode is plain language: \"Ring everyone\" / \"One at a time\"."));
    {
        char b[24]; std::snprintf(b, sizeof(b), "groups %d", total);
        std::string body;
        int gap = (kCols - 2) - 1 - (int)std::strlen(b) - 2;
        if (gap < 1) gap = 1;
        body += std::string((size_t)gap, ' ');
        body += col(Role::Text, b);
        bodyRow(body);
    }
}

// ── Forwards/DND tab body (tui-style §3.7) — FULLY backed + wired ─────────────
void Tui::renderPbxForwards(int& bodyUsed)
{
    PbxConfigSnapshot cfg = pbxConfig();
    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
        ++bodyUsed;
    };
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
        ++bodyUsed;
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    // Render one forward cell: ↳ <target> (grp: token kept) or "·" when unset.
    auto fwdCell = [&](const std::string& t, int width) -> std::string {
        if (t.empty()) return padCols(_unicode ? "\xc2\xb7" : ".", width);
        return padCols(std::string(glyph(Glyph::Fwd)) + " " + t, width);
    };

    bodyRow(col(Role::Header, " EXT    NAME          DND     CFU(all)\xe2\x86\x92  CFB(busy)\xe2\x86\x92  CFNA(no-ans)\xe2\x86\x92"));
    {
        const char* H = glyph(Glyph::BoxH);
        std::string r = " ";
        auto run = [&](int n) { for (int i = 0; i < n; ++i) r += H; };
        run(5); r += "  "; run(11); r += "  "; run(5); r += "  "; run(10); r += "  "; run(9); r += "  "; run(11);
        bodyRow(col(Role::Border, r));
    }

    int total = (int)cfg.extensions.size();
    if (_pbxSel >= total) _pbxSel = total > 0 ? total - 1 : 0;
    if (_pbxSel < 0) _pbxSel = 0;
    if (_pbxSel < _pbxTop) _pbxTop = _pbxSel;
    if (_pbxSel >= _pbxTop + kPbxRows) _pbxTop = _pbxSel - kPbxRows + 1;
    if (_pbxTop < 0) _pbxTop = 0;

    if (total == 0)
    {
        bodyRow(col(Role::Dim, " (no extensions registered yet \xe2\x80\x94 register a phone to set forwards/DND)"));
        for (int i = 0; i < kPbxRows - 1; ++i) bodyBlank();
    }
    else
    {
        for (int i = 0; i < kPbxRows; ++i)
        {
            int idx = _pbxTop + i;
            if (idx >= total) { bodyBlank(); continue; }
            const PbxExt& e = cfg.extensions[idx];
            bool sel = (idx == _pbxSel);

            std::string grid;
            grid += (sel ? (_unicode ? "\xe2\x96\xb8" : ">") : " ");
            grid += " ";
            grid += padCols(e.ext, 5) + "  ";
            grid += padCols(e.name.empty() ? (_unicode ? "\xe2\x80\x94" : "-") : e.name, 11) + "  ";
            std::string dnd = e.dnd ? (std::string(glyph(Glyph::Dnd)) + " DND") : (_unicode ? "\xc2\xb7" : ".");
            grid += padCols(dnd, 5) + "  ";
            grid += fwdCell(e.cfu, 10) + "  ";
            grid += fwdCell(e.cfb, 9) + "  ";
            grid += fwdCell(e.cfna, 11);

            if (sel && _color) bodyRow(std::string("\x1b[7m") + grid + "\x1b[0m");
            else               bodyRow(col(Role::Text, grid));
        }
    }

    bodyRow(col(Role::Dim, " [Space] flips DND on the selected row \xc2\xb7 [Enter] sets the three forwards."));
    {
        char b[24]; std::snprintf(b, sizeof(b), "ext %d/%d", total, cfg.maxExt);
        std::string body;
        int gap = (kCols - 2) - 1 - (int)std::strlen(b) - 2;
        if (gap < 1) gap = 1;
        body += std::string((size_t)gap, ' ');
        body += col(Role::Text, b);
        bodyRow(body);
    }
}

// ── IVR tab body (tui-style §3.8) — layout built; menu store is an honest stub ─
void Tui::renderPbxIvr(int& bodyUsed)
{
    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
        ++bodyUsed;
    };
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
        ++bodyUsed;
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };

    // Header facts (answer point / greeting) — honest "(unset)" since nothing
    // persists. The old "[T] set / test" affordance was removed: its handler is a
    // no-op stub (no IVR menu store), so advertising the key would be dishonest.
    // The STUB disclosure at the foot of this body states the same plainly.
    {
        std::string body = col(Role::Text, " Answer point .. ");
        body += col(Role::Dim, "(unset)");
        bodyRow(body);
    }
    bodyRow(col(Role::Text, " Greeting ...... ") + col(Role::Dim, "(none on flash yet)"));
    bodyBlank();

    bodyRow(col(Role::Header, " DIGIT   ACTION                  TARGET"));
    {
        const char* H = glyph(Glyph::BoxH);
        std::string r = " ";
        auto run = [&](int n) { for (int i = 0; i < n; ++i) r += H; };
        run(5); r += "  "; run(20); r += "  "; run(17);
        bodyRow(col(Role::Border, r));
    }

    // A flat digit map, all unset (no menu store) — the §3.8 layout, honestly empty.
    static const char* kDigits[6] = { "1", "2", "3", "0", "*", "#" };
    int selRow = _pbxSel; if (selRow < 0) selRow = 0; if (selRow > 5) selRow = 5;
    for (int i = 0; i < 6; ++i)
    {
        bool sel = (i == selRow);
        std::string grid;
        grid += (sel ? (_unicode ? "\xe2\x96\xb8" : ">") : " ");
        grid += " ";
        grid += padCols(kDigits[i], 4) + "  ";
        grid += padCols(_unicode ? "\xe2\x80\x94 (unset)" : "- (unset)", 20) + "  ";
        grid += padCols(_unicode ? "\xe2\x80\x94" : "-", 17);
        if (sel && _color) bodyRow(std::string("\x1b[7m") + grid + "\x1b[0m");
        else               bodyRow(col(Role::Dim, grid));
    }

    bodyRow(col(Role::Text, " One menu, one level deep \xe2\x80\x94 DTMF digit to action. No call queues."));
    bodyRow(col(Role::Dim, " STUB: no IVR menu store yet \xe2\x80\x94 lands with the IVR media increment."));
}

// ── Features tab body (tui-style §3.9) — read-only star-code reference (real) ──
// These are EXACTLY the CLASS/feature codes RequestsHandler.cpp implements (grep'd):
// *60 DND on · *80 DND off · *72<ext> CFU on · *73 CFU off · *69 last caller ·
// *11 echo test. No invented codes (honesty clause / IA §6.1).
void Tui::renderPbxFeatures(int& bodyUsed)
{
    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
        ++bodyUsed;
    };
    auto bodyRow = [&](const std::string& body) {
        roled(Role::Border, v);
        put(" ");
        put(body);
        int pad = (kCols - 2) - 1 - dispWidth(body);
        if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
        ++bodyUsed;
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };

    bodyRow(col(Role::Header, " STAR CODES  \xe2\x80\x94  dial these from any registered phone (read-only)"));
    {
        const char* H = glyph(Glyph::BoxH);
        std::string r = " ";
        for (int i = 0; i < kCols - 4; ++i) r += H;
        bodyRow(col(Role::Border, r));
    }
    auto code = [&](const char* c, const char* desc) {
        std::string body = " ";
        body += col(Role::Header, c);
        int gap = 14 - (int)std::strlen(c); if (gap < 2) gap = 2;
        body += std::string((size_t)gap, ' ');
        body += col(Role::Text, desc);
        bodyRow(body);
    };
    code("*60",       "Do-Not-Disturb ON   (for the dialing extension)");
    code("*80",       "Do-Not-Disturb OFF");
    code("*72<ext>",  "Forward-all ON to <ext>");
    code("*73",       "Forward-all OFF");
    code("*69",       "Speak the last caller's extension");
    code("*11",       "Echo test (line check)");
    bodyBlank();
    bodyRow(col(Role::Text, " These are dialed on the phones, not set here. To change forwarding"));
    bodyRow(col(Role::Text, " from the terminal, use the Forwards/DND tab."));
}

// ── Shared modal-box prelude ──────────────────────────────────────────────────
// Every PBX modal centers a box of a given width on the PBX-CONFIG spine and pads to
// 18 body rows. To avoid repeating the frame/line lambdas in each render, this helper
// draws the spine + an empty top margin and returns the box geometry via out-params.
// The caller then uses the returned lambdas-equivalent inline. We instead inline the
// box helpers in each modal (matching renderChangePin/renderCdrDetail) for clarity.

// ── §3.5.1 Add-extension submenu (single | range) ─────────────────────────────
void Tui::renderPbxAddMenu()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("PBX CONFIG", st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    const int boxW = 44;
    const int indent = (kCols - 2 - boxW) / 2;
    const char* TL = glyph(Glyph::BoxTL); const char* TR = glyph(Glyph::BoxTR);
    const char* BL = glyph(Glyph::BoxBL); const char* BR = glyph(Glyph::BoxBR);
    const char* VR = glyph(Glyph::BoxVR); const char* VL = glyph(Glyph::BoxVL);
    const std::string H = glyph(Glyph::BoxH);
    auto frame = [&](bool top, bool sep, const char* title) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        if (sep) put(VR); else put(top ? TL : BL);
        int used = 0;
        if (top && title) { put(H); put(" "); put(title); put(" "); used = 1 + 1 + dispWidth(title) + 1; }
        for (int i = 0; i < boxW - 2 - used; ++i) put(H);
        if (sep) put(VL); else put(top ? TR : BR);
        if (_color) put("\x1b[0m");
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto line = [&](const std::string& inner) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        put(" ");
        put(inner);
        int ipad = (boxW - 2) - 1 - dispWidth(inner); if (ipad < 0) ipad = 0;
        put(std::string((size_t)ipad, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };

    int used = 0;
    bodyBlank(); ++used; bodyBlank(); ++used;
    frame(true, false, "Add extension"); ++used;
    line(col(Role::Text, "How many?")); ++used;
    {
        // Focus is shown two ways so it survives monochrome: reverse-video (when
        // colour is on) AND a ▸ marker glyph before the focused choice. The marker
        // occupies the choice's 2-col lead (▸ + space), so column widths are stable
        // whether or not a choice is focused.
        const std::string mark = std::string(glyph(Glyph::Marker)) + " ";  // "▸ " / "> "
        std::string inner;
        inner += (_pbxAddChoice == 0) ? mark : "  ";
        if (_pbxAddChoice == 0 && _color) inner += "\x1b[7m";
        else if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += "< Single >";
        if (_color) inner += "\x1b[0m";
        inner += "   ";
        inner += (_pbxAddChoice == 1) ? mark : "  ";
        if (_pbxAddChoice == 1 && _color) inner += "\x1b[7m";
        else if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += "[ Range (batch) ]";
        if (_color) inner += "\x1b[0m";
        line(inner); ++used;
    }
    frame(false, true, nullptr); ++used;
    line(col(Role::Dim, "[\xe2\x86\x90/\xe2\x86\x92] Choose  [Enter] Next  [Esc] Cancel")); ++used;
    frame(false, false, nullptr); ++used;

    for (int i = used; i < 18; ++i) bodyBlank();
    drawFooter("[\xe2\x86\x90/\xe2\x86\x92] Choose  [Enter] Next  [Esc] Cancel");
}

// ── §3.5.2 Add single — honest stub form ──────────────────────────────────────
void Tui::renderPbxAddSingle()
{
    LiveStats st = stats();
    PbxConfigSnapshot cfg = pbxConfig();
    clearScreen();
    hideCursor();
    drawSpineTop("PBX CONFIG", st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    const int boxW = 54;
    const int indent = (kCols - 2 - boxW) / 2;
    const char* TL = glyph(Glyph::BoxTL); const char* TR = glyph(Glyph::BoxTR);
    const char* BL = glyph(Glyph::BoxBL); const char* BR = glyph(Glyph::BoxBR);
    const char* VR = glyph(Glyph::BoxVR); const char* VL = glyph(Glyph::BoxVL);
    const std::string H = glyph(Glyph::BoxH);
    auto frame = [&](bool top, bool sep, const char* title) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        if (sep) put(VR); else put(top ? TL : BL);
        int used = 0;
        if (top && title) { put(H); put(" "); put(title); put(" "); used = 1 + 1 + dispWidth(title) + 1; }
        for (int i = 0; i < boxW - 2 - used; ++i) put(H);
        if (sep) put(VL); else put(top ? TR : BR);
        if (_color) put("\x1b[0m");
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto line = [&](const std::string& inner) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        put(" ");
        put(inner);
        int ipad = (boxW - 2) - 1 - dispWidth(inner); if (ipad < 0) ipad = 0;
        put(std::string((size_t)ipad, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };

    int used = 0;
    bodyBlank(); ++used; bodyBlank(); ++used;
    frame(true, false, "Add extension \xc2\xb7 single"); ++used;
    line(col(Role::Text, "Number .... ") + col(Role::Dim, "[            ]   3\xe2\x80\x93" "6 digits")); ++used;
    line(col(Role::Text, "Name ...... ") + col(Role::Dim, "[            ]")); ++used;
    line(col(Role::Text, "PIN ....... ") + col(Role::Dim, "[            ]   never echoed")); ++used;
    line(""); ++used;
    {
        char b[40]; std::snprintf(b, sizeof(b), "Pool now: %d/%d", (int)cfg.extensions.size(), cfg.maxExt);
        line(col(Role::Text, b)); ++used;
    }
    line(""); ++used;
    // The honesty stub line — there is NO provisioning store to write to.
    line(col(Role::Alert, std::string(glyph(Glyph::Alert)) + " no provisioning store")); ++used;
    line(col(Role::Dim, "Extensions self-register (open registrar). A single-add")); ++used;
    line(col(Role::Dim, "store lands with a future increment \xe2\x80\x94 nothing persists yet.")); ++used;
    frame(false, true, nullptr); ++used;
    line(col(Role::Dim, "[Esc] Back")); ++used;
    frame(false, false, nullptr); ++used;

    for (int i = used; i < 18; ++i) bodyBlank();
    drawFooter("[Esc] Back  [?] Help");
}

// ── §3.5.3 Add range (batch) — honest stub form (the marquee D2 layout) ───────
void Tui::renderPbxAddRange()
{
    LiveStats st = stats();
    PbxConfigSnapshot cfg = pbxConfig();
    clearScreen();
    hideCursor();
    drawSpineTop("PBX CONFIG", st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    const int boxW = 54;
    const int indent = (kCols - 2 - boxW) / 2;
    const char* TL = glyph(Glyph::BoxTL); const char* TR = glyph(Glyph::BoxTR);
    const char* BL = glyph(Glyph::BoxBL); const char* BR = glyph(Glyph::BoxBR);
    const char* VR = glyph(Glyph::BoxVR); const char* VL = glyph(Glyph::BoxVL);
    const std::string H = glyph(Glyph::BoxH);
    auto frame = [&](bool top, bool sep, const char* title) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        if (sep) put(VR); else put(top ? TL : BL);
        int used = 0;
        if (top && title) { put(H); put(" "); put(title); put(" "); used = 1 + 1 + dispWidth(title) + 1; }
        for (int i = 0; i < boxW - 2 - used; ++i) put(H);
        if (sep) put(VL); else put(top ? TR : BR);
        if (_color) put("\x1b[0m");
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto line = [&](const std::string& inner) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        put(" ");
        put(inner);
        int ipad = (boxW - 2) - 1 - dispWidth(inner); if (ipad < 0) ipad = 0;
        put(std::string((size_t)ipad, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };

    int used = 0;
    bodyBlank(); ++used; bodyBlank(); ++used;
    frame(true, false, "Add extensions \xc2\xb7 range (batch)"); ++used;
    line(col(Role::Text, "Range ........ ") + col(Role::Dim, "[ 101-124   ]")); ++used;
    line(col(Role::Text, "PIN policy ... ") + col(Role::Dim, "(\xe2\x80\xa2) Match number (v1 default)")); ++used;
    line(col(Role::Text, "Add to group . ") + col(Role::Dim, "[ (none) \xe2\x96\xbe ]")); ++used;
    line(""); ++used;
    {
        char b[40]; std::snprintf(b, sizeof(b), "Pool now: %d/%d", (int)cfg.extensions.size(), cfg.maxExt);
        line(col(Role::Text, b)); ++used;
    }
    line(""); ++used;
    line(col(Role::Alert, std::string(glyph(Glyph::Alert)) + " no provisioning store")); ++used;
    line(col(Role::Dim, "Range provisioning needs a persistent extension store the")); ++used;
    line(col(Role::Dim, "firmware does not ship yet \xe2\x80\x94 nothing is written. The match-")); ++used;
    line(col(Role::Dim, "number PIN policy is the locked v1 default.")); ++used;
    frame(false, true, nullptr); ++used;
    line(col(Role::Dim, "[Esc] Back")); ++used;
    frame(false, false, nullptr); ++used;

    for (int i = used; i < 18; ++i) bodyBlank();
    drawFooter("[Esc] Back  [?] Help");
}

// ── §3.8.1 IVR digit editor — honest stub ─────────────────────────────────────
void Tui::renderPbxIvrEdit()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("PBX CONFIG", st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    const int boxW = 52;
    const int indent = (kCols - 2 - boxW) / 2;
    const char* TL = glyph(Glyph::BoxTL); const char* TR = glyph(Glyph::BoxTR);
    const char* BL = glyph(Glyph::BoxBL); const char* BR = glyph(Glyph::BoxBR);
    const char* VR = glyph(Glyph::BoxVR); const char* VL = glyph(Glyph::BoxVL);
    const std::string H = glyph(Glyph::BoxH);
    auto frame = [&](bool top, bool sep, const std::string& title) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        if (sep) put(VR); else put(top ? TL : BL);
        int used = 0;
        if (top && !title.empty()) { put(H); put(" "); put(title); put(" "); used = 1 + 1 + dispWidth(title) + 1; }
        for (int i = 0; i < boxW - 2 - used; ++i) put(H);
        if (sep) put(VL); else put(top ? TR : BR);
        if (_color) put("\x1b[0m");
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto line = [&](const std::string& inner) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        put(" ");
        put(inner);
        int ipad = (boxW - 2) - 1 - dispWidth(inner); if (ipad < 0) ipad = 0;
        put(std::string((size_t)ipad, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto radio = [&](int idx, const char* label, const char* tail) {
        std::string inner = "  ";
        inner += (_ivrAction == idx) ? (_unicode ? "(\xe2\x80\xa2)" : "(*)") : "( )";
        inner += " ";
        inner += (_ivrAction == idx) ? col(Role::Text, label) : col(Role::Dim, label);
        if (tail && tail[0]) { inner += "  "; inner += col(Role::Dim, tail); }
        line(inner);
    };

    int used = 0;
    bodyBlank(); ++used; bodyBlank(); ++used;
    frame(true, false, std::string("Digit  ") + _ivrDigit); ++used;
    line(col(Role::Text, std::string("When the caller presses ") + _ivrDigit + ", do:")); ++used;
    radio(0, "Ring a group", "[ \xe2\x96\xbe ]"); ++used;
    radio(1, "Ring extension", "[ ___ ]"); ++used;
    radio(2, "Play a prompt", "[ ____.wav \xe2\x96\xbe ]"); ++used;
    radio(3, "Nothing (unset)", ""); ++used;
    line(""); ++used;
    line(col(Role::Alert, std::string(glyph(Glyph::Alert)) + " no IVR menu store")); ++used;
    line(col(Role::Dim, "DTMF collection exists; a persistent menu does not yet.")); ++used;
    line(col(Role::Dim, "This editor lands wired with the IVR media increment.")); ++used;
    frame(false, true, ""); ++used;
    line(col(Role::Dim, "[\xe2\x86\x91/\xe2\x86\x93] Action  [Esc] Back")); ++used;
    frame(false, false, ""); ++used;

    for (int i = used; i < 18; ++i) bodyBlank();
    drawFooter("[\xe2\x86\x91/\xe2\x86\x93] Action  [Esc] Back  [?] Help");
}

// ── §2.8 Ring Group delete — guarded confirm (reuses drawConfirmBox) ──────────
void Tui::renderPbxGroupDelete()
{
    LiveStats st = stats();
    PbxConfigSnapshot cfg = pbxConfig();
    clearScreen();
    hideCursor();
    drawSpineTop("PBX CONFIG", st);

    // Render from the NAME captured at [D]-press (not _pbxSel) so the confirm box
    // always names the same victim the y-handler will delete, even if the live
    // config shifted between the two independent snapshots. Member count is looked
    // up by name in this snapshot (0 if the group already vanished).
    std::string name = _grpDelName.empty() ? "(none)" : _grpDelName;
    int memberCount = 0;
    for (const auto& g : cfg.groups)
        if (g.name == _grpDelName) { memberCount = (int)g.members.size(); break; }
    char mc[64];
    std::snprintf(mc, sizeof(mc), "Its %d member%s detach (their phones keep working).",
                  memberCount, memberCount == 1 ? "" : "s");
    std::vector<std::string> lines = {
        std::string("Delete ring group \"") + name + "\"?",
        mc,
        "This cannot be undone."
    };
    std::string alert = std::string(glyph(Glyph::Alert)) + " ALERT   DELETE GROUP";
    drawConfirmBox("Confirm delete", alert.c_str(), Role::Alert, lines,
                   "< Delete >", "[ Keep, go back ]", /*destructiveFocused=*/false,
                   _unicode ? "[\xe2\x86\x90/\xe2\x86\x92] Choose   [Enter] Confirm   y/N   [Esc] Cancel"
                            : "[</>] Choose   [Enter] Confirm   y/N   [Esc] Cancel");
    drawFooter("[\xe2\x86\x90/\xe2\x86\x92] Choose  [Enter] Confirm  [Esc] Cancel");
}

// ── §3.6.1 / §3.6.2 Ring Group editor (create + edit) ─────────────────────────
// Mode radios + member checklist + Hunt-order column. One model, two entry points:
// _grpCreate=true is the CREATE flow (< Create >, empty name, zero members), false is
// EDIT (< Apply >, pre-filled). Geometry mirrors §3.6.1 verbatim per the spec.
void Tui::renderPbxGroupEdit()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("PBX CONFIG", st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    const int boxW = 58;
    const int indent = (kCols - 2 - boxW) / 2;
    const char* TL = glyph(Glyph::BoxTL); const char* TR = glyph(Glyph::BoxTR);
    const char* BL = glyph(Glyph::BoxBL); const char* BR = glyph(Glyph::BoxBR);
    const char* VR = glyph(Glyph::BoxVR); const char* VL = glyph(Glyph::BoxVL);
    const std::string H = glyph(Glyph::BoxH);
    auto frame = [&](bool top, bool sep, const std::string& title) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        if (sep) put(VR); else put(top ? TL : BL);
        int used = 0;
        if (top && !title.empty()) { put(H); put(" "); put(title); put(" "); used = 1 + 1 + dispWidth(title) + 1; }
        for (int i = 0; i < boxW - 2 - used; ++i) put(H);
        if (sep) put(VL); else put(top ? TR : BR);
        if (_color) put("\x1b[0m");
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto line = [&](const std::string& inner) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        put(" ");
        put(inner);
        int ipad = (boxW - 2) - 1 - dispWidth(inner); if (ipad < 0) ipad = 0;
        put(std::string((size_t)ipad, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };

    int selected = 0;
    for (size_t i = 0; i < _grpChecked.size(); ++i) if (_grpChecked[i]) ++selected;

    int used = 0;
    bodyBlank(); ++used;
    std::string title = _grpCreate ? "Add ring group"
                                   : std::string("Edit ring group \xc2\xb7 ") + _grpOrigName;
    frame(true, false, title); ++used;

    // Name field (focus 0).
    {
        std::string nm = std::string("[ ") + (_grpName.empty() ? "_" : _grpName);
        int padN = 12 - dispWidth(_grpName); if (padN < 1) padN = 1;
        nm += std::string((size_t)padN, ' ') + "]";
        std::string row = col(Role::Text, "Name ..... ");
        if (_grpFocus == 0 && _color) row += "\x1b[7m";
        row += col(Role::Text, nm);
        if (_grpFocus == 0 && _color) row += "\x1b[0m";
        row += col(Role::Dim, "  3\xe2\x80\x93" "20 chars, unique");
        line(row); ++used;
    }
    // Mode radios (focus 1).
    {
        std::string row = col(Role::Text, "Mode ..... ");
        std::string a = std::string(_grpRingAll ? (_unicode ? "(\xe2\x80\xa2)" : "(*)") : "( )") + " Ring everyone";
        std::string b = std::string(!_grpRingAll ? (_unicode ? "(\xe2\x80\xa2)" : "(*)") : "( )") + " One at a time";
        if (_grpFocus == 1 && _color) row += "\x1b[7m";
        row += a; row += "   "; row += b;
        if (_grpFocus == 1 && _color) row += "\x1b[0m";
        line(_grpFocus == 1 ? row : (col(Role::Text, "Mode ..... ") + a + "   " + b)); ++used;
    }
    line(""); ++used;
    // Members header with live count.
    {
        char cnt[24]; std::snprintf(cnt, sizeof(cnt), "(%d selected)", selected);
        std::string row = col(Role::Header, "Members  ");
        row += (selected == 0) ? col(Role::Dim, cnt) : col(Role::Text, cnt);
        row += col(Role::Dim, "  (Space toggles)");
        int gap = (boxW - 2) - 1 - dispWidth(row) - 10;
        if (gap < 1) gap = 1;
        row += std::string((size_t)gap, ' ');
        row += col(Role::Header, "Hunt order");
        line(row); ++used;
    }
    // Member checklist (focus 2). Show up to 5 rows; the focused row is reverse.
    {
        int n = (int)_grpMembers.size();
        const int kShow = 5;
        int top = 0;
        if (_grpMemberSel >= kShow) top = _grpMemberSel - kShow + 1;
        if (top < 0) top = 0;
        for (int i = 0; i < kShow; ++i)
        {
            int idx = top + i;
            if (idx >= n) { line(""); ++used; continue; }
            bool checked = (idx < (int)_grpChecked.size()) && _grpChecked[idx];
            bool memSel = (_grpFocus == 2 && idx == _grpMemberSel);
            std::string box = checked ? "[x]" : "[ ]";
            std::string lbl = _grpMembers[idx];
            // Order column: live only in Hunt mode for checked members.
            std::string order = "\xe2\x80\x94";
            if (!_grpRingAll && checked && idx < (int)_grpOrder.size() && _grpOrder[idx] > 0)
                order = std::to_string(_grpOrder[idx]);
            else if (!_unicode) order = "-";
            std::string rowInner = box + " " + padCols(lbl, 22);
            // pad to put order near the right edge.
            int gap = (boxW - 2) - 1 - dispWidth(rowInner) - dispWidth(order) - 1;
            if (gap < 1) gap = 1;
            rowInner += std::string((size_t)gap, ' ') + order;
            if (memSel && _color) line(std::string("\x1b[7m") + rowInner + "\x1b[0m");
            else                  line(col(Role::Text, rowInner));
            ++used;
        }
    }
    line(""); ++used;
    if (!_grpMsg.empty())
        line(col(Role::Alert, _grpMsg));
    else
        line(col(Role::Dim, _grpRingAll
            ? "Ring everyone: all members ring at once."
            : "One at a time: callers hunt 1\xe2\x86\x92" "2\xe2\x86\x92" "3 until one answers."));
    ++used;
    // Buttons (focus 3).
    {
        const char* actBtn = _grpCreate ? "< Create >" : "< Apply >";
        std::string inner = "        ";
        bool actFocus = (_grpFocus == 3 && _grpBtn == 0);
        bool canFocus = (_grpFocus == 3 && _grpBtn == 1);
        if (actFocus && _color) inner += "\x1b[7m";
        else if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += actBtn;
        if (_color) inner += "\x1b[0m";
        inner += "        ";
        if (canFocus && _color) inner += "\x1b[7m";
        else if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += "[ Cancel ]";
        if (_color) inner += "\x1b[0m";
        line(inner); ++used;
    }
    frame(false, true, ""); ++used;
    line(col(Role::Dim, std::string("[Tab] Field [Space] Toggle [\xe2\x86\x91/\xe2\x86\x93] Member [Enter] ")
         + (_grpCreate ? "Create" : "Apply"))); ++used;
    frame(false, false, ""); ++used;

    for (int i = used; i < 18; ++i) bodyBlank();
    drawFooter(_grpCreate
        ? "[Tab] Field  [Space] Toggle  [Enter] Create  [Esc] Cancel"
        : "[Tab] Field  [Space] Toggle  [Enter] Apply  [Esc] Cancel");
}

// ── §3.7.1 Forward editor (DND + CFU/CFB/CFNA) ────────────────────────────────
void Tui::renderPbxForwardEdit()
{
    LiveStats st = stats();
    clearScreen();
    hideCursor();
    drawSpineTop("PBX CONFIG", st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    const int boxW = 58;
    const int indent = (kCols - 2 - boxW) / 2;
    const char* TL = glyph(Glyph::BoxTL); const char* TR = glyph(Glyph::BoxTR);
    const char* BL = glyph(Glyph::BoxBL); const char* BR = glyph(Glyph::BoxBR);
    const char* VR = glyph(Glyph::BoxVR); const char* VL = glyph(Glyph::BoxVL);
    const std::string H = glyph(Glyph::BoxH);
    auto frame = [&](bool top, bool sep, const std::string& title) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        if (sep) put(VR); else put(top ? TL : BL);
        int used = 0;
        if (top && !title.empty()) { put(H); put(" "); put(title); put(" "); used = 1 + 1 + dispWidth(title) + 1; }
        for (int i = 0; i < boxW - 2 - used; ++i) put(H);
        if (sep) put(VL); else put(top ? TR : BR);
        if (_color) put("\x1b[0m");
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto line = [&](const std::string& inner) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        put(" ");
        put(inner);
        int ipad = (boxW - 2) - 1 - dispWidth(inner); if (ipad < 0) ipad = 0;
        put(std::string((size_t)ipad, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    // A picker-style forward field: "Label .... [ <target>      ▾ ]".
    auto fwdField = [&](const char* label, const std::string& target, int focusIdx, const char* tag) {
        std::string row = col(Role::Text, label);
        std::string box = std::string("[ ") + padCols(target.empty() ? "" : target, 18) + " " +
                          (_unicode ? "\xe2\x96\xbe" : "v") + " ]";
        if (_fwdFocus == focusIdx && _color) row += "\x1b[7m";
        row += col(Role::Text, box);
        if (_fwdFocus == focusIdx && _color) row += "\x1b[0m";
        row += col(Role::Dim, std::string("  ") + tag);
        line(row);
    };

    int used = 0;
    bodyBlank(); ++used;
    frame(true, false, std::string("Forwards \xc2\xb7 ext ") + _fwdExt); ++used;
    // DND row (focus 0).
    {
        std::string row = col(Role::Text, "Do-Not-Disturb ... ");
        std::string boxd = _fwdDnd ? "[x] on " : "[ ] off";
        if (_fwdFocus == 0 && _color) row += "\x1b[7m";
        row += _fwdDnd ? col(Role::Dnd, boxd) : col(Role::Text, boxd);
        if (_fwdFocus == 0 && _color) row += "\x1b[0m";
        row += col(Role::Dim, "  (Space toggles)");
        line(row); ++used;
    }
    line(""); ++used;
    fwdField("Forward ALL calls .... ", _fwdCfu, 1, "CFU"); ++used;
    fwdField("Forward when BUSY .... ", _fwdCfb, 2, "CFB"); ++used;
    fwdField("Forward NO-ANSWER .... ", _fwdCfna, 3, "CFNA"); ++used;
    line(""); ++used;
    line(col(Role::Dim, "A target is another registered extension.")); ++used;
    line(col(Role::Dim, "Open a field with [Space] to pick; blank clears it.")); ++used;
    line(""); ++used;
    if (!_fwdMsg.empty()) { line(col(Role::Alert, _fwdMsg)); ++used; }
    // Buttons (focus 4).
    {
        std::string inner = "        ";
        bool aFocus = (_fwdFocus == 4 && _fwdBtn == 0);
        bool cFocus = (_fwdFocus == 4 && _fwdBtn == 1);
        if (aFocus && _color) inner += "\x1b[7m";
        else if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += "< Apply >";
        if (_color) inner += "\x1b[0m";
        inner += "        ";
        if (cFocus && _color) inner += "\x1b[7m";
        else if (_color) inner += std::string("\x1b[") + sgrFor(Role::Text) + "m";
        inner += "[ Cancel ]";
        if (_color) inner += "\x1b[0m";
        line(inner); ++used;
    }
    frame(false, true, ""); ++used;
    line(col(Role::Dim, "[Tab] Field [Space] Open/DND [Enter] Apply [Esc] Back")); ++used;
    frame(false, false, ""); ++used;

    for (int i = used; i < 18; ++i) bodyBlank();
    drawFooter("[Tab] Field  [Space] Open/DND  [Enter] Apply  [Esc] Back");
}

// ── §3.7.2 Forward target picker (extensions + ring groups + clear) ───────────
void Tui::renderPbxForwardPick()
{
    LiveStats st = stats();
    PbxConfigSnapshot cfg = pbxConfig();
    clearScreen();
    hideCursor();
    drawSpineTop("PBX CONFIG", st);

    const std::string v = glyph(Glyph::BoxV);
    auto bodyBlank = [&]() {
        roled(Role::Border, v);
        put(std::string((size_t)(kCols - 2), ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto col = [&](Role r, const std::string& s) -> std::string {
        if (!_color) return s;
        return std::string("\x1b[") + sgrFor(r) + "m" + s + "\x1b[0m";
    };
    const int boxW = 58;
    const int indent = (kCols - 2 - boxW) / 2;
    const char* TL = glyph(Glyph::BoxTL); const char* TR = glyph(Glyph::BoxTR);
    const char* BL = glyph(Glyph::BoxBL); const char* BR = glyph(Glyph::BoxBR);
    const char* VR = glyph(Glyph::BoxVR); const char* VL = glyph(Glyph::BoxVL);
    const std::string H = glyph(Glyph::BoxH);
    auto frame = [&](bool top, bool sep, const std::string& title) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        if (_color) sgr(sgrFor(Role::Border));
        if (sep) put(VR); else put(top ? TL : BL);
        int used = 0;
        if (top && !title.empty()) { put(H); put(" "); put(title); put(" "); used = 1 + 1 + dispWidth(title) + 1; }
        for (int i = 0; i < boxW - 2 - used; ++i) put(H);
        if (sep) put(VL); else put(top ? TR : BR);
        if (_color) put("\x1b[0m");
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };
    auto line = [&](const std::string& inner) {
        roled(Role::Border, v);
        put(std::string((size_t)indent, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        put(" ");
        put(inner);
        int ipad = (boxW - 2) - 1 - dispWidth(inner); if (ipad < 0) ipad = 0;
        put(std::string((size_t)ipad, ' '));
        roled(Role::Border, glyph(Glyph::BoxV));
        int pad = (kCols - 2) - indent - boxW; if (pad < 0) pad = 0;
        put(std::string((size_t)pad, ' '));
        roled(Role::Border, v);
        put("\r\n");
    };

    auto list = buildForwardPickList(cfg);
    int total = (int)list.size();
    if (_fwdPickSel >= total) _fwdPickSel = total > 0 ? total - 1 : 0;
    if (_fwdPickSel < 0) _fwdPickSel = 0;

    const char* whichWord = (_fwdPickField == 1) ? "ALL" : (_fwdPickField == 2) ? "BUSY" : "NO-ANSWER";
    int used = 0;
    bodyBlank(); ++used;
    frame(true, false, std::string("Forward ") + whichWord + " \xe2\x86\x92 pick a target"); ++used;

    // Render: EXTENSIONS section, then the clear sentinel (ring groups excluded).
    // Window the list to fit (12 rows visible).
    const int kShow = 12;
    int top = 0;
    if (_fwdPickSel >= kShow) top = _fwdPickSel - kShow + 1;
    if (top < 0) top = 0;
    bool extHeaderShown = false;
    int shown = 0;
    // The list holds extensions then the trailing clear sentinel (ring groups are
    // excluded — see buildForwardPickList). Show an EXTENSIONS header first if any
    // extension row is in view; the clear sentinel renders without a header.
    for (int i = top; i < total && shown < kShow; ++i)
    {
        const PickEntry& p = list[i];
        bool sel = (i == _fwdPickSel);
        // Insert section labels inline (they consume a visible row only when first seen).
        if (p.token.empty())
        {
            // The clear sentinel.
            std::string inner = (sel ? (_unicode ? "\xe2\x96\xb8 " : "> ") : "  ");
            inner += (_unicode ? "\xe2\x80\x94 (clear this forward)" : "- (clear this forward)");
            if (sel && _color) line(std::string("\x1b[7m") + inner + "\x1b[0m");
            else               line(col(Role::Dim, inner));
            ++used; ++shown;
            continue;
        }
        if (!extHeaderShown && shown < kShow)
        {
            line(col(Role::Header, "EXTENSIONS")); ++used; ++shown; extHeaderShown = true;
            if (shown >= kShow) break;
        }
        std::string inner = (sel ? (_unicode ? "\xe2\x96\xb8 " : "> ") : "  ");
        inner += "  ";
        inner += padCols(p.label, 22);
        // Extension row: bare label + a state chip (glyph+label).
        if (sel && _color)
        {
            inner += stateLabelPlain(p.state);
            line(std::string("\x1b[7m") + inner + "\x1b[0m");
        }
        else
        {
            std::string rowc = col(Role::Text, inner);
            int vis = 0; std::string chip; appendStateChip(p.state, chip, vis);
            rowc += chip;
            line(rowc);
        }
        ++used; ++shown;
    }

    frame(false, true, ""); ++used;
    line(col(Role::Dim, "[\xe2\x86\x91/\xe2\x86\x93] Pick   [Enter] Choose   [Esc] Keep current")); ++used;
    frame(false, false, ""); ++used;

    for (int i = used; i < 18; ++i) bodyBlank();
    drawFooter("[\xe2\x86\x91/\xe2\x86\x93] Pick  [Enter] Choose  [Esc] Keep current");
}

// ═══════════════════════════════════════════════════════════════════════════════
// [3] PBX CONFIG — input handlers
// ═══════════════════════════════════════════════════════════════════════════════

// Move the active tab left/right (wrapping), resetting the per-list cursor so a tab
// switch never lands the selection out of range on a shorter list.
static int pbxTabCount() { return 5; }

// Open the Forward editor pre-filled from the selected extension's current config.
// (Defined as a member-style helper here so onKeyPbxConfig can reuse it.)
void Tui::onKeyPbxConfig(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::PbxConfig); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::Hub); return; }

    // Tab movement is global across the panel.
    auto moveTab = [&](int delta) {
        int n = pbxTabCount();
        int t = ((int)_pbxTab + delta % n + n) % n;
        _pbxTab = (PbxTab)t;
        _pbxSel = 0; _pbxTop = 0;
        gotoScreen(Screen::PbxConfig);
    };
    if (k == Key::Tab)   { moveTab(+1); return; }
    if (k == Key::Left)  { moveTab(-1); return; }
    if (k == Key::Right) { moveTab(+1); return; }

    PbxConfigSnapshot cfg = pbxConfig();
    // Row count for the active table tab (Features has no selectable rows).
    int rows = 0;
    switch (_pbxTab)
    {
        case PbxTab::Extensions: rows = (int)cfg.extensions.size(); break;
        case PbxTab::RingGroups: rows = (int)cfg.groups.size();     break;
        case PbxTab::Forwards:   rows = (int)cfg.extensions.size(); break;
        case PbxTab::Ivr:        rows = 6;                          break;  // fixed digit map
        case PbxTab::Features:   rows = 0;                          break;
    }
    if (k == Key::Up)   { if (_pbxSel > 0) --_pbxSel; gotoScreen(Screen::PbxConfig); return; }
    if (k == Key::Down) { if (_pbxSel < rows - 1) ++_pbxSel; gotoScreen(Screen::PbxConfig); return; }

    if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }

    // Per-tab verbs.
    if (_pbxTab == PbxTab::Extensions)
    {
        if (k == Key::Char && (ch == 'A' || ch == 'a')) { _pbxAddChoice = 0; gotoScreen(Screen::PbxAddMenu); return; }
        if (k == Key::Char && (ch == 'D' || ch == 'd')) { /* no provisioning store → no delete (honest) */ return; }
        if (k == Key::Char && (ch == 'S' || ch == 's'))
        {
            // Assign/rotate this extension's SIP digest secret (never-echoed modal).
            if (_pbxSel >= 0 && _pbxSel < (int)cfg.extensions.size())
            {
                _secretExt = cfg.extensions[_pbxSel].ext;
                _secretBuf.clear(); _secretMsg.clear();
                _secretReturn = Screen::PbxConfig;
                gotoScreen(Screen::SecretEntry);
            }
            return;
        }
        if (k == Key::Enter)
        {
            // Enter on an extension → its Forwards/DND editor (the real wired path).
            if (_pbxSel >= 0 && _pbxSel < (int)cfg.extensions.size())
            {
                const PbxExt& e = cfg.extensions[_pbxSel];
                _fwdExt = e.ext; _fwdDnd = e.dnd;
                _fwdCfu = e.cfu; _fwdCfb = e.cfb; _fwdCfna = e.cfna;
                _fwdFocus = 1; _fwdBtn = 0; _fwdMsg.clear();
                _pbxReturn = Screen::PbxConfig;
                gotoScreen(Screen::PbxForwardEdit);
            }
            return;
        }
    }
    else if (_pbxTab == PbxTab::RingGroups)
    {
        if (k == Key::Char && (ch == 'A' || ch == 'a'))
        {
            // CREATE flow: empty box, roster from registered extensions (+admin ext).
            _grpCreate = true; _grpName.clear(); _grpOrigName.clear();
            _grpRingAll = true; _grpMsg.clear();
            _grpMembers.clear(); _grpChecked.clear(); _grpOrder.clear();
            for (const auto& e : cfg.extensions) _grpMembers.push_back(
                e.ext + (e.name.empty() ? "" : (" " + e.name)));
            // Guarantee at least the admin ext appears (never an empty-list dead-end).
            if (_grpMembers.empty()) _grpMembers.push_back(cfg.adminExt);
            _grpChecked.assign(_grpMembers.size(), false);
            _grpOrder.assign(_grpMembers.size(), 0);
            _grpNextOrder = 1; _grpFocus = 0; _grpMemberSel = 0; _grpBtn = 0;
            gotoScreen(Screen::PbxGroupEdit);
            return;
        }
        if (k == Key::Enter)
        {
            if (_pbxSel >= 0 && _pbxSel < (int)cfg.groups.size())
            {
                // EDIT flow: pre-fill from the selected group. The checklist is the
                // union of (registered roster) ∪ (stored members) so a stored member
                // that is currently unregistered still shows (and can be unchecked).
                const PbxGroup& g = cfg.groups[_pbxSel];
                _grpCreate = false; _grpName = g.name; _grpOrigName = g.name;
                _grpRingAll = g.ringAll; _grpMsg.clear();
                _grpMembers.clear(); _grpChecked.clear(); _grpOrder.clear();
                // Members already in the group come first, in stored order.
                std::vector<std::string> seen;
                for (const auto& m : g.members) { _grpMembers.push_back(m); seen.push_back(m); }
                for (const auto& e : cfg.extensions)
                {
                    bool dup = false;
                    for (const auto& s : seen) if (s == e.ext) { dup = true; break; }
                    if (!dup) _grpMembers.push_back(e.ext);
                }
                _grpChecked.assign(_grpMembers.size(), false);
                _grpOrder.assign(_grpMembers.size(), 0);
                _grpNextOrder = 1;
                for (size_t i = 0; i < g.members.size() && i < _grpMembers.size(); ++i)
                {
                    _grpChecked[i] = true;
                    _grpOrder[i] = _grpNextOrder++;   // stored order = hunt order
                }
                _grpFocus = 0; _grpMemberSel = 0; _grpBtn = 0;
                gotoScreen(Screen::PbxGroupEdit);
            }
            return;
        }
        if (k == Key::Char && (ch == 'D' || ch == 'd'))
        {
            if (_pbxSel >= 0 && _pbxSel < (int)cfg.groups.size())
            {
                // Pin the victim BY NAME now — the confirm render and the y-handler
                // each re-read the live config, so the _pbxSel index alone could
                // drift if the roster changed between snapshots.
                _grpDelName = cfg.groups[_pbxSel].name;
                gotoScreen(Screen::PbxGroupDelete);
            }
            return;
        }
        // NOTE: the old [F]→Fwd cross-link ("point an ext at this ring group") was
        // removed. It pre-filled CFU with "grp:<name>", but the SIP layer cannot
        // resolve a group forward target (see buildForwardPickList), so the cross-
        // link advertised a forward that would silently no-op at call time.
        // Re-add once forward-to-ring-group routing is wired in the SIP layer.
    }
    else if (_pbxTab == PbxTab::Forwards)
    {
        if (k == Key::Char && ch == ' ')   // [Space] flips DND on the selected row (live)
        {
            if (_pbxSel >= 0 && _pbxSel < (int)cfg.extensions.size())
            {
                const PbxExt& e = cfg.extensions[_pbxSel];
                if (_dndSet) _dndSet(e.ext, !e.dnd);
                gotoScreen(Screen::PbxConfig);   // repaint — provider re-reads live DND
            }
            return;
        }
        if (k == Key::Enter)
        {
            if (_pbxSel >= 0 && _pbxSel < (int)cfg.extensions.size())
            {
                const PbxExt& e = cfg.extensions[_pbxSel];
                _fwdExt = e.ext; _fwdDnd = e.dnd;
                _fwdCfu = e.cfu; _fwdCfb = e.cfb; _fwdCfna = e.cfna;
                _fwdFocus = 1; _fwdBtn = 0; _fwdMsg.clear();
                _pbxReturn = Screen::PbxConfig;
                gotoScreen(Screen::PbxForwardEdit);
            }
            return;
        }
    }
    else if (_pbxTab == PbxTab::Ivr)
    {
        if (k == Key::Enter)
        {
            static const char kDigits[6] = { '1', '2', '3', '0', '*', '#' };
            _ivrDigit = kDigits[_pbxSel < 0 ? 0 : (_pbxSel > 5 ? 5 : _pbxSel)];
            _ivrAction = 0;
            gotoScreen(Screen::PbxIvrEdit);
            return;
        }
        if (k == Key::Char && (ch == 'T' || ch == 't')) { /* set/test: no store yet (stub) */ return; }
    }
    // Features tab: read-only — only Tabs/Esc/?, all handled above.
}

// ── §3.5.1 Add submenu keys ───────────────────────────────────────────────────
void Tui::onKeyPbxAddMenu(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::PbxAddMenu); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::PbxConfig); return; }
    if (k == Key::Left)  { _pbxAddChoice = 0; gotoScreen(Screen::PbxAddMenu); return; }
    if (k == Key::Right) { _pbxAddChoice = 1; gotoScreen(Screen::PbxAddMenu); return; }
    if (k == Key::Enter)
    {
        gotoScreen(_pbxAddChoice == 0 ? Screen::PbxAddSingle : Screen::PbxAddRange);
        return;
    }
    if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
}

// Add-single / add-range are stub forms: Esc backs out, ? help. No mutation path.
void Tui::onKeyPbxAddSingle(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::PbxAddSingle); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::PbxAddMenu); return; }
    if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
}
void Tui::onKeyPbxAddRange(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::PbxAddRange); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::PbxAddMenu); return; }
    if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
}

// ── §3.6.1/§3.6.2 Ring Group editor keys ──────────────────────────────────────
void Tui::onKeyPbxGroupEdit(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::PbxGroupEdit); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::PbxConfig); return; }   // D7: no write

    // Tab cycles Name → Mode → checklist → buttons.
    if (k == Key::Tab)
    {
        _grpFocus = (_grpFocus + 1) % 4; _grpMsg.clear();
        gotoScreen(Screen::PbxGroupEdit); return;
    }

    // Apply / Create: validate, build CSV, call the setter.
    auto applyGroup = [&]() {
        // Trim the name.
        std::string name = _grpName;
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (name.size() < 3) { _grpMsg = std::string(glyph(Glyph::Alert)) + " name needs 3+ chars"; gotoScreen(Screen::PbxGroupEdit); return; }
        // Collect checked members in HUNT order (or any order for RingAll).
        std::vector<std::pair<int,std::string>> picked;
        for (size_t i = 0; i < _grpMembers.size(); ++i)
        {
            if (i < _grpChecked.size() && _grpChecked[i])
            {
                // Extract the bare ext (the label may be "101 Maria").
                std::string ext = _grpMembers[i];
                size_t sp = ext.find(' ');
                if (sp != std::string::npos) ext.resize(sp);
                int ord = (i < _grpOrder.size()) ? _grpOrder[i] : 0;
                picked.push_back({ ord > 0 ? ord : 1000000 + (int)i, ext });
            }
        }
        if (picked.empty())
        {
            _grpMsg = std::string("Pick at least one extension to ring. ") + glyph(Glyph::Alert) + " no members";
            gotoScreen(Screen::PbxGroupEdit); return;
        }
        std::sort(picked.begin(), picked.end(),
                  [](const auto& a, const auto& b){ return a.first < b.first; });
        std::string csv;
        for (size_t i = 0; i < picked.size(); ++i) { if (i) csv += ','; csv += picked[i].second; }
        std::string mode = _grpRingAll ? "ringall" : "hunt";
        if (!_ringGroupSet) { _grpMsg = "Ring-group edit not available on this build."; gotoScreen(Screen::PbxGroupEdit); return; }
        std::string err = _ringGroupSet(name, csv, mode);
        if (!err.empty()) { _grpMsg = err; gotoScreen(Screen::PbxGroupEdit); return; }
        gotoScreen(Screen::PbxConfig);   // success → back to the Ring Groups list
    };
    if (k == Key::Enter) { applyGroup(); return; }

    // Field-specific handling.
    if (_grpFocus == 0)   // Name field
    {
        if (k == Key::Backspace) { if (!_grpName.empty()) _grpName.pop_back(); _grpMsg.clear(); gotoScreen(Screen::PbxGroupEdit); return; }
        if (k == Key::Char)
        {
            if (ch == '?') { gotoScreen(Screen::Help); return; }
            // Accept the AOR charset (alnum + a few punct). Cap at 20.
            if (_grpName.size() < 20 &&
                (std::isalnum((unsigned char)ch) || ch == '.' || ch == '-' || ch == '_'))
            { _grpName.push_back((char)ch); _grpMsg.clear(); gotoScreen(Screen::PbxGroupEdit); return; }
            return;
        }
    }
    else if (_grpFocus == 1)   // Mode radios
    {
        if (k == Key::Left)  { _grpRingAll = true;  gotoScreen(Screen::PbxGroupEdit); return; }
        if (k == Key::Right) { _grpRingAll = false; gotoScreen(Screen::PbxGroupEdit); return; }
        if (k == Key::Char && ch == ' ') { _grpRingAll = !_grpRingAll; gotoScreen(Screen::PbxGroupEdit); return; }
        if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
    }
    else if (_grpFocus == 2)   // Member checklist
    {
        int n = (int)_grpMembers.size();
        if (k == Key::Up)   { if (_grpMemberSel > 0) --_grpMemberSel; gotoScreen(Screen::PbxGroupEdit); return; }
        if (k == Key::Down) { if (_grpMemberSel < n - 1) ++_grpMemberSel; gotoScreen(Screen::PbxGroupEdit); return; }
        if (k == Key::Char && ch == ' ')
        {
            int i = _grpMemberSel;
            if (i >= 0 && i < n && i < (int)_grpChecked.size())
            {
                if (_grpChecked[i]) { _grpChecked[i] = false; _grpOrder[i] = 0; }
                else                { _grpChecked[i] = true;  _grpOrder[i] = _grpNextOrder++; }
                _grpMsg.clear();
            }
            gotoScreen(Screen::PbxGroupEdit); return;
        }
        if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
    }
    else   // buttons (focus 3)
    {
        if (k == Key::Left)  { _grpBtn = 0; gotoScreen(Screen::PbxGroupEdit); return; }
        if (k == Key::Right) { _grpBtn = 1; gotoScreen(Screen::PbxGroupEdit); return; }
        if (k == Key::Char && ch == ' ')
        {
            if (_grpBtn == 0) applyGroup();
            else gotoScreen(Screen::PbxConfig);
            return;
        }
        if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
    }
}

// ── §2.8 Ring Group delete confirm keys (guarded; safe default focused) ───────
void Tui::onKeyPbxGroupDelete(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::PbxGroupDelete); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::PbxConfig); return; }
    if (k == Key::Char)
    {
        if (ch == 'y' || ch == 'Y')
        {
            // Delete = set the group with an EMPTY member list (the setter deletes
            // it). Delete BY the NAME captured at [D]-press, not by _pbxSel index —
            // the index could now point at a different row in a freshly-shifted snapshot.
            if (!_grpDelName.empty() && _ringGroupSet)
                _ringGroupSet(_grpDelName, "", "ringall");
            _grpDelName.clear();
            _pbxSel = 0;
            gotoScreen(Screen::PbxConfig);
            return;
        }
        if (ch == 'n' || ch == 'N') { gotoScreen(Screen::PbxConfig); return; }
        if (ch == '?')              { gotoScreen(Screen::Help); return; }
    }
    if (k == Key::Enter) { gotoScreen(Screen::PbxConfig); return; }   // focused = safe
}

// ── §3.7.1 Forward editor keys ────────────────────────────────────────────────
void Tui::onKeyPbxForwardEdit(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::PbxForwardEdit); return; }
    if (k == Key::Esc)   { gotoScreen(_pbxReturn); return; }

    if (k == Key::Tab)
    {
        _fwdFocus = (_fwdFocus + 1) % 5; _fwdMsg.clear();
        gotoScreen(Screen::PbxForwardEdit); return;
    }

    auto applyForward = [&]() {
        if (!_forwardSet || !_dndSet)
        {
            _fwdMsg = "Forward edit not available on this build.";
            gotoScreen(Screen::PbxForwardEdit); return;
        }
        // DND + the three forwards. Empty target clears that trigger.
        _dndSet(_fwdExt, _fwdDnd);
        _forwardSet(_fwdExt, "always",   _fwdCfu);
        _forwardSet(_fwdExt, "busy",     _fwdCfb);
        _forwardSet(_fwdExt, "noanswer", _fwdCfna);
        gotoScreen(_pbxReturn);
    };
    if (k == Key::Enter) { applyForward(); return; }

    // [Space]: toggle DND on the DND row, or OPEN the picker on a forward row.
    if (k == Key::Char && ch == ' ')
    {
        if (_fwdFocus == 0) { _fwdDnd = !_fwdDnd; gotoScreen(Screen::PbxForwardEdit); return; }
        if (_fwdFocus >= 1 && _fwdFocus <= 3)
        {
            _fwdPickField = _fwdFocus;   // 1=CFU 2=CFB 3=CFNA
            _fwdPickSel = 0;
            gotoScreen(Screen::PbxForwardPick);
            return;
        }
        if (_fwdFocus == 4)   // Space on a button activates it.
        {
            if (_fwdBtn == 0) applyForward();
            else gotoScreen(_pbxReturn);
            return;
        }
    }
    if (_fwdFocus == 4)
    {
        if (k == Key::Left)  { _fwdBtn = 0; gotoScreen(Screen::PbxForwardEdit); return; }
        if (k == Key::Right) { _fwdBtn = 1; gotoScreen(Screen::PbxForwardEdit); return; }
    }
    if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
}

// ── §3.7.2 Forward picker keys ────────────────────────────────────────────────
void Tui::onKeyPbxForwardPick(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::PbxForwardPick); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::PbxForwardEdit); return; }   // keep current

    PbxConfigSnapshot cfg = pbxConfig();
    auto list = buildForwardPickList(cfg);
    int total = (int)list.size();
    if (k == Key::Up)   { if (_fwdPickSel > 0) --_fwdPickSel; gotoScreen(Screen::PbxForwardPick); return; }
    if (k == Key::Down) { if (_fwdPickSel < total - 1) ++_fwdPickSel; gotoScreen(Screen::PbxForwardPick); return; }
    if (k == Key::Enter)
    {
        if (_fwdPickSel >= 0 && _fwdPickSel < total)
        {
            const std::string& tok = list[_fwdPickSel].token;
            if (_fwdPickField == 1) _fwdCfu = tok;
            else if (_fwdPickField == 2) _fwdCfb = tok;
            else _fwdCfna = tok;
        }
        gotoScreen(Screen::PbxForwardEdit);
        return;
    }
    if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
}

// ── §3.8.1 IVR digit editor keys (stub — interactive layout, no persistence) ──
void Tui::onKeyPbxIvrEdit(Key k, unsigned char ch)
{
    if (k == Key::CtrlL) { gotoScreen(Screen::PbxIvrEdit); return; }
    if (k == Key::Esc)   { gotoScreen(Screen::PbxConfig); return; }
    if (k == Key::Up)    { if (_ivrAction > 0) --_ivrAction; gotoScreen(Screen::PbxIvrEdit); return; }
    if (k == Key::Down)  { if (_ivrAction < 3) ++_ivrAction; gotoScreen(Screen::PbxIvrEdit); return; }
    if (k == Key::Char && ch == '?') { gotoScreen(Screen::Help); return; }
}
