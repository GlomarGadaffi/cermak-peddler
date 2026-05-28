#ifndef INDEX_HTML_H
#define INDEX_HTML_H

static const char CGA_INDEX_HTML[] = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>CGA CRT SipServer v5.03</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=VT323&display=swap" rel="stylesheet">
<style>
/* ═══════════════════════════════════════════════════════════════
   CGA CRT STYLES — Premium Retro Console Stylesheet
   ═══════════════════════════════════════════════════════════════ */

:root {
  --bg:        #0000AA;
  --bg-dark:   #000088;
  --fg:        #FFFFFF;
  --cyan:      #55FFFF;
  --yellow:    #FFFF55;
  --red:       #FF5555;
  --green:     #55FF55;
  --magenta:   #FF55FF;
  --dk-cyan:   #00AAAA;
  --dk-yellow: #AA8800;
  --dk-green:  #00AA00;
  --dk-red:    #AA0000;
  --dk-mag:    #AA00AA;
  --gray:      #AAAAAA;
  --dk-gray:   #555555;
  --black:     #000000;
  --panel-bg:  rgba(0, 0, 100, 0.6);
  --glow-cyan: 0 0 8px rgba(85,255,255,0.6), 0 0 2px rgba(85,255,255,0.3);
  --glow-white:0 0 8px rgba(255,255,255,0.5), 0 0 2px rgba(255,255,255,0.2);
}

* { margin:0; padding:0; box-sizing:border-box; }

@font-face {
  font-family: 'CGA Fallback';
  src: local('Courier New'), local('Consolas'), local('monospace');
}

html, body {
  width: 100%; height: 100%;
  overflow: hidden;
  background: var(--black);
  font-family: 'VT323', 'Courier New', 'Consolas', monospace;
  font-size: 16px;
  color: var(--fg);
}

/* CRT Monitor Frame */
#crt-monitor {
  position: relative;
  width: 100%; height: 100%;
  background: var(--bg);
  border-radius: 12px;
  overflow: hidden;
}

/* Scanline overlay */
#crt-monitor::before {
  content: '';
  position: absolute;
  top: 0; left: 0;
  width: 100%; height: 100%;
  background: repeating-linear-gradient(
    0deg,
    transparent,
    transparent 2px,
    rgba(0, 0, 0, 0.12) 2px,
    rgba(0, 0, 0, 0.12) 4px
  );
  pointer-events: none;
  z-index: 1000;
}

/* CRT flicker */
#crt-monitor::after {
  content: '';
  position: absolute;
  top: 0; left: 0;
  width: 100%; height: 100%;
  background: rgba(0, 0, 170, 0.03);
  pointer-events: none;
  z-index: 999;
  animation: flicker 0.15s infinite alternate;
}

@keyframes flicker {
  0%   { opacity: 0.97; }
  50%  { opacity: 1.0; }
  100% { opacity: 0.98; }
}

/* Subtle screen curvature via vignette */
#vignette {
  position: absolute;
  top: 0; left: 0;
  width: 100%; height: 100%;
  background: radial-gradient(
    ellipse at center,
    transparent 60%,
    rgba(0,0,0,0.35) 100%
  );
  pointer-events: none;
  z-index: 998;
}

/* Phosphor glow on all text */
#crt-monitor .glow { text-shadow: var(--glow-white); }
#crt-monitor .glow-cyan { text-shadow: var(--glow-cyan); color: var(--cyan); }
#crt-monitor .glow-green { text-shadow: 0 0 8px rgba(85,255,85,0.6); color: var(--green); }
#crt-monitor .glow-yellow { text-shadow: 0 0 8px rgba(255,255,85,0.6); color: var(--yellow); }
#crt-monitor .glow-red { text-shadow: 0 0 8px rgba(255,85,85,0.6); color: var(--red); }
#crt-monitor .glow-magenta { text-shadow: 0 0 8px rgba(255,85,255,0.6); color: var(--magenta); }

/* Main layout */
#main-content {
  position: relative;
  width: 100%; height: 100%;
  display: flex;
  flex-direction: column;
  padding: 4px 8px;
  z-index: 1;
  overflow: hidden;
}

/* ─── HEADER BAR ─── */
#header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  background: var(--dk-cyan);
  color: var(--black);
  padding: 2px 8px;
  font-size: 18px;
  font-weight: bold;
  border-bottom: 2px solid var(--cyan);
  flex-shrink: 0;
  text-shadow: none;
}

#header .title {
  letter-spacing: 1px;
}

#header .menu-items {
  display: flex;
  gap: 6px;
}

.menu-btn {
  background: var(--bg-dark);
  color: var(--yellow);
  border: 1px solid var(--cyan);
  padding: 1px 8px;
  font-family: inherit;
  font-size: 15px;
  cursor: pointer;
  text-shadow: 0 0 6px rgba(255,255,85,0.4);
}
.menu-btn:hover {
  background: var(--dk-yellow);
  color: var(--black);
}
.menu-btn:active {
  background: var(--yellow);
  color: var(--black);
}

/* ─── BODY AREA: panels side by side ─── */
#body-area {
  display: flex;
  flex: 1;
  gap: 4px;
  min-height: 0;
  padding-top: 4px;
  overflow: hidden;
}

#left-panels {
  flex: 1;
  display: flex;
  flex-direction: column;
  gap: 4px;
  min-width: 0;
  overflow: hidden;
}

#right-panel {
  width: 220px;
  flex-shrink: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 4px;
}

/* ─── PANEL (box-drawing) ─── */
.panel {
  background: var(--panel-bg);
  position: relative;
  padding: 2px 6px;
  flex-shrink: 0;
  overflow: hidden;
}

.panel-title {
  color: var(--yellow);
  font-size: 17px;
  text-shadow: 0 0 6px rgba(255,255,85,0.5);
  margin-bottom: 2px;
}

.panel-border-top {
  color: var(--cyan);
  text-shadow: var(--glow-cyan);
  font-size: 16px;
  white-space: pre;
  overflow: hidden;
  line-height: 1.1;
}
.panel-border-bottom {
  color: var(--cyan);
  text-shadow: var(--glow-cyan);
  font-size: 16px;
  white-space: pre;
  overflow: hidden;
  line-height: 1.1;
}

.panel-body {
  padding: 2px 4px;
  font-size: 16px;
  line-height: 1.3;
}

/* ─── STATUS PANEL ─── */
.status-row {
  display: flex;
  justify-content: space-between;
  padding: 1px 0;
}
.status-label { color: var(--gray); }
.status-value { color: var(--green); text-shadow: 0 0 6px rgba(85,255,85,0.4); }
)rawhtml"
R"rawhtml(
/* ─── TABLES ─── */
.retro-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 15px;
}
.retro-table th {
  color: var(--yellow);
  text-align: left;
  padding: 1px 6px;
  border-bottom: 1px solid var(--dk-cyan);
  text-shadow: 0 0 4px rgba(255,255,85,0.3);
  white-space: nowrap;
}
.retro-table td {
  padding: 1px 6px;
  color: var(--fg);
  white-space: nowrap;
}
.retro-table tr:nth-child(even) {
  background: rgba(0,0,80,0.3);
}

.status-online {
  color: var(--green);
  text-shadow: 0 0 6px rgba(85,255,85,0.6);
}
.status-offline {
  color: var(--dk-gray);
}
.status-ringing {
  color: var(--yellow);
  animation: blinker 0.8s step-start infinite;
}

@keyframes blinker {
  50% { opacity: 0; }
}

/* Scrollable table containers */
.table-scroll {
  max-height: 120px;
  overflow-y: auto;
  scrollbar-width: thin;
  scrollbar-color: var(--dk-cyan) var(--bg-dark);
}
.table-scroll::-webkit-scrollbar { width: 8px; }
.table-scroll::-webkit-scrollbar-track { background: var(--bg-dark); }
.table-scroll::-webkit-scrollbar-thumb { background: var(--dk-cyan); }

/* ─── 3D ROTATING CANVAS ─── */
#retro-canvas {
  background: rgba(0, 0, 60, 0.5);
  border: 1px solid var(--dk-cyan);
  display: block;
}
#retro-label {
  color: var(--dk-cyan);
  font-size: 13px;
  text-align: center;
  margin-top: 2px;
}

/* ─── SYSTEM ORACLE BAR ─── */
#oracle-bar {
  background: var(--dk-mag);
  color: var(--yellow);
  padding: 3px 10px;
  font-size: 16px;
  text-align: center;
  flex-shrink: 0;
  text-shadow: 0 0 8px rgba(255,255,85,0.5);
  border-top: 1px solid var(--magenta);
  border-bottom: 1px solid var(--magenta);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

/* ─── TERMINAL ─── */
#terminal-container {
  flex: 1;
  min-height: 100px;
  display: flex;
  flex-direction: column;
  background: var(--black);
  border: 1px solid var(--dk-cyan);
  overflow: hidden;
}

#terminal-title-bar {
  background: var(--dk-cyan);
  color: var(--black);
  padding: 1px 8px;
  font-size: 15px;
  font-weight: bold;
  flex-shrink: 0;
}

#terminal-output {
  flex: 1;
  overflow-y: auto;
  padding: 4px 8px;
  font-size: 15px;
  line-height: 1.3;
  color: var(--fg);
  text-shadow: 0 0 4px rgba(255,255,255,0.2);
  scrollbar-width: thin;
  scrollbar-color: var(--dk-cyan) var(--black);
  white-space: pre-wrap;
  word-break: break-all;
}
#terminal-output::-webkit-scrollbar { width: 8px; }
#terminal-output::-webkit-scrollbar-track { background: var(--black); }
#terminal-output::-webkit-scrollbar-thumb { background: var(--dk-cyan); }

#terminal-input-line {
  display: flex;
  align-items: center;
  padding: 2px 8px 4px 8px;
  background: var(--black);
  flex-shrink: 0;
  border-top: 1px solid #001144;
}

#terminal-prompt {
  color: var(--cyan);
  text-shadow: var(--glow-cyan);
  white-space: pre;
  font-size: 15px;
}

#terminal-input {
  flex: 1;
  background: transparent;
  border: none;
  outline: none;
  color: var(--green);
  font-family: inherit;
  font-size: 15px;
  caret-color: var(--green);
  text-shadow: 0 0 6px rgba(85,255,85,0.4);
  margin-left: 2px;
}

/* Blinking block cursor via caret simulation */
.cursor-blink {
  display: inline-block;
  width: 9px; height: 16px;
  background: var(--green);
  animation: cursorBlink 1s step-start infinite;
  vertical-align: middle;
  margin-left: 1px;
}
@keyframes cursorBlink {
  0%, 100% { opacity: 1; }
  50%      { opacity: 0; }
}

/* ─── WIFI MODAL ─── */
#wifi-modal {
  display: none;
  position: fixed;
  top: 50%; left: 50%;
  transform: translate(-50%, -50%);
  background: var(--bg-dark);
  border: 2px solid var(--cyan);
  padding: 0;
  z-index: 2000;
  min-width: 450px;
  max-width: 560px;
  box-shadow: 0 0 30px rgba(85,255,255,0.3), inset 0 0 60px rgba(0,0,100,0.3);
}

#wifi-modal-title {
  background: var(--dk-cyan);
  color: var(--black);
  padding: 3px 10px;
  font-size: 17px;
  font-weight: bold;
  display: flex;
  justify-content: space-between;
  align-items: center;
}

#wifi-modal-body {
  padding: 10px 14px;
  max-height: 380px;
  overflow-y: auto;
  scrollbar-width: thin;
  scrollbar-color: var(--dk-cyan) var(--bg-dark);
}

#wifi-modal-body::-webkit-scrollbar { width: 8px; }
#wifi-modal-body::-webkit-scrollbar-track { background: var(--bg-dark); }
#wifi-modal-body::-webkit-scrollbar-thumb { background: var(--dk-cyan); }

.wifi-network {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 4px 6px;
  margin: 2px 0;
  cursor: pointer;
  border: 1px solid transparent;
}
.wifi-network:hover {
  background: rgba(85,255,255,0.1);
  border-color: var(--dk-cyan);
}
.wifi-ssid { color: var(--green); }
.wifi-rssi { color: var(--gray); font-size: 14px; }
.wifi-enc { color: var(--dk-yellow); font-size: 13px; }

#wifi-connect-form {
  display: none;
  margin-top: 10px;
  padding-top: 8px;
  border-top: 1px solid var(--dk-cyan);
}

#wifi-connect-form label {
  color: var(--cyan);
  display: block;
  margin-bottom: 4px;
  font-size: 15px;
}

#wifi-connect-form input[type="text"],
#wifi-connect-form input[type="password"] {
  width: 100%;
  background: var(--black);
  border: 1px solid var(--dk-cyan);
  color: var(--green);
  font-family: inherit;
  font-size: 15px;
  padding: 4px 8px;
  margin-bottom: 8px;
  outline: none;
}
#wifi-connect-form input:focus {
  border-color: var(--cyan);
  box-shadow: 0 0 6px rgba(85,255,255,0.3);
}

.btn-retro {
  background: var(--dk-cyan);
  color: var(--black);
  border: 1px solid var(--cyan);
  padding: 3px 14px;
  font-family: inherit;
  font-size: 15px;
  cursor: pointer;
  font-weight: bold;
  margin-right: 6px;
}
.btn-retro:hover { background: var(--cyan); }
.btn-retro.btn-danger {
  background: var(--dk-red);
  color: var(--yellow);
  border-color: var(--red);
}
.btn-retro.btn-danger:hover { background: var(--red); color: var(--black); }
)rawhtml"
R"rawhtml(
/* ─── HELP MODAL ─── */
#help-modal {
  display: none;
  position: fixed;
  top: 50%; left: 50%;
  transform: translate(-50%, -50%);
  background: var(--bg-dark);
  border: 2px solid var(--cyan);
  padding: 0;
  z-index: 2000;
  min-width: 480px;
  max-width: 600px;
  box-shadow: 0 0 30px rgba(85,255,255,0.3), inset 0 0 60px rgba(0,0,100,0.3);
}
#help-modal-title {
  background: var(--dk-cyan);
  color: var(--black);
  padding: 3px 10px;
  font-size: 17px;
  font-weight: bold;
  display: flex;
  justify-content: space-between;
  align-items: center;
}
#help-modal-body {
  padding: 12px 16px;
  line-height: 1.5;
  font-size: 15px;
  max-height: 420px;
  overflow-y: auto;
}

/* ─── RESPONSIVE ─── */
@media (max-width: 800px) {
  #body-area { flex-direction: column; }
  #right-panel { width: 100%; flex-direction: row; justify-content: center; }
  #retro-canvas { width: 150px !important; height: 150px !important; }
}
</style>
</head>
<body>
<div id="crt-monitor">
  <div id="vignette"></div>
  <div id="main-content">

    <!-- ══════ HEADER BAR ══════ -->
    <div id="header">
      <span class="title">CGA CRT SipServer v5.03 — Retro-Console Switchboard</span>
      <div class="menu-items">
        <button class="menu-btn" onclick="showHelp()" title="Help">[F1 Help]</button>
        <button class="menu-btn" onclick="refreshNow()" title="Refresh">[F5 Refresh]</button>
        <button class="menu-btn" onclick="oracleSpeak()" title="Query Oracle">[F7 Oracle]</button>
        <button class="menu-btn" onclick="showWifi()" title="WiFi">[F9 WiFi]</button>
      </div>
    </div>

    <!-- ══════ BODY ══════ -->
    <div id="body-area">

      <!-- LEFT COLUMN -->
      <div id="left-panels">

        <!-- STATUS PANEL -->
        <div class="panel">
          <div class="panel-border-top">╔══════════════════════════════════════════════════════════════╗</div>
          <div class="panel-title"> ║ ☼ SYSTEM STATUS ☼</div>
          <div class="panel-body">
            <div class="status-row"><span class="status-label">Server IP ···</span><span class="status-value" id="s-ip">0.0.0.0</span></div>
            <div class="status-row"><span class="status-label">Port ········</span><span class="status-value" id="s-port">5060</span></div>
            <div class="status-row"><span class="status-label">Uptime ······</span><span class="status-value" id="s-uptime">00:00:00</span></div>
            <div class="status-row"><span class="status-label">Extensions ··</span><span class="status-value" id="s-ext-count">0</span></div>
            <div class="status-row"><span class="status-label">Active Calls </span><span class="status-value" id="s-call-count">0</span></div>
            <div class="status-row"><span class="status-label">Packets ·····</span><span class="status-value" id="s-packets">0</span></div>
          </div>
          <div class="panel-border-bottom">╚══════════════════════════════════════════════════════════════╝</div>
        </div>

        <!-- REGISTERED EXTENSIONS -->
        <div class="panel">
          <div class="panel-border-top">╔══════════════════════════════════════════════════════════════╗</div>
          <div class="panel-title"> ║ ☎ REGISTERED EXTENSIONS</div>
          <div class="panel-body">
            <div class="table-scroll">
              <table class="retro-table">
                <thead>
                  <tr><th>Ext#</th><th>IP Address</th><th>Port</th><th>Status</th></tr>
                </thead>
                <tbody id="ext-tbody">
                  <tr><td colspan="4" style="color:var(--dk-gray)">No extensions registered</td></tr>
                </tbody>
              </table>
            </div>
          </div>
          <div class="panel-border-bottom">╚══════════════════════════════════════════════════════════════╝</div>
        </div>

        <!-- ACTIVE SESSIONS -->
        <div class="panel">
          <div class="panel-border-top">╔══════════════════════════════════════════════════════════════╗</div>
          <div class="panel-title"> ║ ⚡ ACTIVE SESSIONS</div>
          <div class="panel-body">
            <div class="table-scroll">
              <table class="retro-table">
                <thead>
                  <tr><th>Caller</th><th>Callee</th><th>State</th><th>Duration</th></tr>
                </thead>
                <tbody id="sess-tbody">
                  <tr><td colspan="4" style="color:var(--dk-gray)">No active sessions</td></tr>
                </tbody>
              </table>
            </div>
          </div>
          <div class="panel-border-bottom">╚══════════════════════════════════════════════════════════════╝</div>
        </div>

      </div> <!-- /left-panels -->

      <!-- RIGHT COLUMN: 3D Wireframe -->
      <div id="right-panel">
        <canvas id="retro-canvas" width="200" height="200"></canvas>
        <div id="retro-label" class="glow-cyan">⚙ 3D Tesseract Projection ⚙</div>
        <!-- Mini status lights -->
        <div style="margin-top:8px; font-size:14px; text-align:left; width:100%;">
          <div class="panel-border-top" style="font-size:14px;">┌──────────────────┐</div>
          <div style="padding:2px 4px;">
            <span style="color:var(--gray);">│</span> <span style="color:var(--green);">●</span> SIP Engine <span style="color:var(--green);">ON</span><br>
            <span style="color:var(--gray);">│</span> <span id="ind-calls" style="color:var(--dk-gray);">●</span> <span id="ind-calls-label">Calls: 0</span><br>
            <span style="color:var(--gray);">│</span> <span id="ind-pkts" style="color:var(--cyan);">▲</span> <span id="ind-pkts-label" style="color:var(--cyan);">Pkts: 0</span>
          </div>
          <div class="panel-border-bottom" style="font-size:14px;">└──────────────────┘</div>
        </div>
      </div>

    </div> <!-- /body-area -->

    <!-- ══════ SYSTEM ORACLE BAR ══════ -->
    <div id="oracle-bar">
      ☼ SYSTEM ORACLE: "In the beginning was the C, and the C was with the hardware, and the compiler was pleased." ☼
    </div>

    <!-- ══════ TERMINAL ══════ -->
    <div id="terminal-container">
      <div id="terminal-title-bar">═══ Retro Shell — C:/SipServer ═══</div>
      <div id="terminal-output"></div>
      <div id="terminal-input-line">
        <span id="terminal-prompt">C:/SipServer&gt; </span>
        <input type="text" id="terminal-input" autocomplete="off" spellcheck="false" autofocus>
        <span class="cursor-blink" id="input-cursor"></span>
      </div>
    </div>

  </div> <!-- /main-content -->
</div> <!-- /crt-monitor -->

<!-- ══════ HELP MODAL ══════ -->
<div id="help-modal">
  <div id="help-modal-title">
    <span>☼ CGA SipServer Help ☼</span>
    <button class="btn-retro" onclick="closeHelp()">✕ Close</button>
  </div>
  <div id="help-modal-body">
    <span style="color:var(--yellow);">═══ CGA CRT SIP Switchboard v5.03 ═══</span><br><br>
    <span style="color:var(--cyan);">KEYBOARD SHORTCUTS:</span><br>
    <span style="color:var(--green);">  F1</span>  — This help screen<br>
    <span style="color:var(--green);">  F5</span>  — Force refresh status<br>
    <span style="color:var(--green);">  F7</span>  — Query System Oracle<br>
    <span style="color:var(--green);">  F9</span>  — WiFi network manager<br>
    <span style="color:var(--green);">  Esc</span> — Close modals / Focus terminal<br><br>
    <span style="color:var(--cyan);">TERMINAL COMMANDS:</span><br>
    <span style="color:var(--green);">  help</span>       — Show available commands<br>
    <span style="color:var(--green);">  dir</span>        — List SipServer directory<br>
    <span style="color:var(--green);">  registered</span> — Show registered extensions<br>
    <span style="color:var(--green);">  sessions</span>   — Show active call sessions<br>
    <span style="color:var(--green);">  uptime</span>     — Show server uptime<br>
    <span style="color:var(--green);">  oracle</span>     — Receive System Oracle wisdom<br>
    <span style="color:var(--green);">  wifi</span>       — Open WiFi manager<br>
    <span style="color:var(--green);">  kill &lt;ext&gt;</span> — Terminate an extension<br>
    <span style="color:var(--green);">  clear</span>      — Clear terminal output<br>
    <span style="color:var(--green);">  ver</span>        — Show version info<br>
    <span style="color:var(--green);">  ascii</span>      — Display classic bell ASCII art<br><br>
    <span style="color:var(--dk-gray);">  "CGA CRT Console: 640x480 resolution, 16 vibrant colors, and pure low-latency SIP."</span><br>
  </div>
)rawhtml"
R"rawhtml(
</div>

<!-- ══════ WIFI MODAL ══════ -->
<div id="wifi-modal">
  <div id="wifi-modal-title">
    <span>☼ WiFi Network Manager ☼</span>
    <button class="btn-retro" onclick="closeWifi()">✕ Close</button>
  </div>
  <div id="wifi-modal-body">
    <div style="margin-bottom:8px;">
      <button class="btn-retro" onclick="scanWifi()">⟳ Scan Networks</button>
      <span id="wifi-status" style="color:var(--gray); font-size:14px; margin-left:8px;">Ready</span>
    </div>
    <div id="wifi-networks-list">
      <div style="color:var(--dk-gray);">Press "Scan Networks" to discover available WiFi...</div>
    </div>
    <div id="wifi-connect-form">
      <label>SSID: <span id="wifi-selected-ssid" style="color:var(--green);"></span></label>
      <label style="margin-top:4px;">Password:</label>
      <input type="password" id="wifi-password" placeholder="Enter network key...">
      <div style="margin-top:6px;">
        <button class="btn-retro" onclick="connectWifi()">⚡ Connect</button>
        <button class="btn-retro btn-danger" onclick="cancelWifiConnect()">Cancel</button>
      </div>
    </div>
  </div>
</div>

<script>
/* ═══════════════════════════════════════════════════════════════
   CGA CRT SipServer Dashboard — Premium Retro JavaScript
   ═══════════════════════════════════════════════════════════════ */

// ─── STATE ───
let statusData = {
  ip: '0.0.0.0', port: 5060, uptime: 0,
  clients: [], sessions: [], packetsProcessed: 0
};
let cmdHistory = [];
let cmdHistoryIdx = -1;
let oracleInterval = null;
let selectedSSID = '';
)rawhtml"
R"rawhtml(
// ─── SYSTEM ORACLE WORD GENERATOR ───
const oraclePhrases = [
  "In the beginning was the C, and the C was with the hardware, and the compiler was pleased.",
  "For the console so loved the user, it gave them 640x480 resolution and 16 colors.",
  "The righteous packeteer's packets shall never be dropped, for the router knows all interfaces.",
  "Blessed is the SIP header, for it carries the voice of the remote node.",
  "And on the seventh day, the engineer rested — but the server kept running.",
  "Ask and the router shall route; seek and the socket shall bind; knock and the port shall open.",
  "Thou shalt not saturate thy neighbor's bandwidth.",
  "The system is my administrator; I shall not want for uptime.",
  "Let there be light — and there was a gorgeous phosphor CRT glow at 60Hz.",
  "He who writes in assembly writes closest to the silicon, closest to speed.",
  "Yea, though I trace through the valley of the shadow of segfaults, I will fear no bug.",
  "The meek shall inherit the mutex lock.",
  "As the user thirsts for low latency, so my socket pants for zero-packet-loss audio.",
  "Judge not the packet by its size, lest thy own packets be dropped by the firewall.",
  "For dust thou art, and unto binary 1s and 0s shalt thou return.",
  "Where two or three nodes are gathered in a LAN, there is a low-latency voice call.",
  "A clean buffer is the sanctuary of a robust pointer.",
  "Put on the full armor of security: firewall, cryptography, and a solid checksum.",
  "The wages of buffer overflow is a core dump, but the gift of pure C++ is eternal uptime.",
  "Many are called, but few are registered.",
  "Every bit matters. Every byte is active. Every packet has a purpose.",
  "Faith without packet transmission is like a SIP server with no extensions.",
  "I am the Alpha and the Omega, the first bit and the last bit.",
  "Let your yes be ACK and your no be NACK.",
  "Render unto TCP the things that are TCP's, and unto UDP the things that are UDP's.",
  "CGA CRT Console resolution: 640x480. No more. No less. Perfectly crisp.",
  "The CPU works in mysterious clock cycles.",
  "Whoever has ears, let them hear the ring group.",
  "For I know the plans I have for your packets — plans to deliver, not to drop.",
  "Create in me a clean buffer, and renew a right pointer within me.",
  "The network interface declares the glory of uptime, and the console proclaims its handiwork.",
  "Standard console resolution is 640x480. 16 bright CGA colors.",
  "An elephant never forgets, and neither does ECC RAM.",
];

function getRandomOracle() {
  return oraclePhrases[Math.floor(Math.random() * oraclePhrases.length)];
}

function updateOracle() {
  const bar = document.getElementById('oracle-bar');
  bar.textContent = '\u263C SYSTEM ORACLE: "' + getRandomOracle() + '" \u263C';
}

function oracleSpeak() {
  updateOracle();
  termPrint('SYSTEM ORACLE: "' + getRandomOracle() + '"', 'var(--yellow)');
}

// ─── TERMINAL ───
const termOutput = document.getElementById('terminal-output');
const termInput = document.getElementById('terminal-input');
const inputCursor = document.getElementById('input-cursor');

function termPrint(text, color) {
  const line = document.createElement('div');
  if (color) line.style.color = color;
  line.textContent = text;
  termOutput.appendChild(line);
  termOutput.scrollTop = termOutput.scrollHeight;
}

function termPrintHTML(html) {
  const line = document.createElement('div');
  line.innerHTML = html;
  termOutput.appendChild(line);
  termOutput.scrollTop = termOutput.scrollHeight;
}

function termClear() {
  termOutput.innerHTML = '';
}

// Boot message
function bootSequence() {
  const bootLines = [
    { t: '═══════════════════════════════════════════════════', c: 'var(--cyan)' },
    { t: '  CGA CRT SipServer v5.03 — Retro-Console Switchboard', c: 'var(--yellow)' },
    { t: '  "Standard resolution: 640x480, 16 vibrant colors."', c: 'var(--dk-gray)' },
    { t: '═══════════════════════════════════════════════════', c: 'var(--cyan)' },
    { t: '' },
    { t: 'BIOS check .......... OK', c: 'var(--green)' },
    { t: 'CGA Graphics ........ ACTIVE (640x480 @ 60Hz)', c: 'var(--green)' },
    { t: 'SIP Stack ........... INITIALIZED', c: 'var(--green)' },
    { t: 'UDP Socket bound .... OK', c: 'var(--green)' },
    { t: 'System Oracle ....... ACTIVE', c: 'var(--green)' },
    { t: 'RNG seed generated .. 0x' + Math.floor(Math.random()*0xFFFFFFFF).toString(16).toUpperCase(), c: 'var(--magenta)' },
    { t: '' },
    { t: 'System ready. Type "help" for commands.', c: 'var(--fg)' },
    { t: '' },
  ];
  bootLines.forEach(l => termPrint(l.t, l.c));
}

function processCommand(cmd) {
  const trimmed = cmd.trim();
  if (!trimmed) return;

  // Echo command
  termPrintHTML('<span style="color:var(--cyan);">C:/SipServer&gt; </span><span style="color:var(--green);">' + escapeHtml(trimmed) + '</span>');

  const parts = trimmed.split(/\s+/);
  const command = parts[0].toLowerCase();

  switch (command) {
    case 'help':
      termPrint('');
      termPrint('═══ Available Commands ═══', 'var(--yellow)');
      termPrint('  help        Show this help text', 'var(--cyan)');
      termPrint('  dir         List SipServer directory', 'var(--cyan)');
      termPrint('  registered  Show registered extensions', 'var(--cyan)');
      termPrint('  sessions    Show active call sessions', 'var(--cyan)');
      termPrint('  uptime      Show server uptime', 'var(--cyan)');
      termPrint('  oracle      Receive System Oracle wisdom', 'var(--cyan)');
      termPrint('  wifi        Open WiFi network manager', 'var(--cyan)');
      termPrint('  kill <ext>  Terminate an extension', 'var(--cyan)');
      termPrint('  clear       Clear terminal output', 'var(--cyan)');
      termPrint('  ver         Show version info', 'var(--cyan)');
      termPrint('  ascii       Display classic bell ASCII art', 'var(--cyan)');
      termPrint('');
      break;

    case 'dir':
      termPrint('');
      termPrint(' Volume in drive C is CGA_SYSTEM', 'var(--gray)');
      termPrint(' Directory of C:\\SipServer', 'var(--gray)');
      termPrint('');
      termPrint(' 05/27/2026  05:03p     <DIR>          .', 'var(--fg)');
      termPrint(' 05/27/2026  05:03p     <DIR>          ..', 'var(--fg)');
      termPrint(' 05/27/2026  05:03p          12,288    SipServer.cpp', 'var(--fg)');
      termPrint(' 05/27/2026  05:03p           8,192    UdpServer.cpp', 'var(--fg)');
      termPrint(' 05/27/2026  05:03p           4,096    HttpServer.cpp', 'var(--fg)');
      termPrint(' 05/27/2026  05:03p           2,048    CMakeLists.txt', 'var(--fg)');
      termPrint(' 05/27/2026  05:03p          16,384    IPHelper.hpp', 'var(--fg)');
      termPrint(' 05/27/2026  05:03p           1,024    README.md', 'var(--yellow)');
      termPrint('        6 File(s)     44,032 bytes', 'var(--gray)');
      termPrint('        2 Dir(s)     1.44MB free', 'var(--gray)');
      termPrint('');
      break;

    case 'registered':
      termPrint('');
      if (statusData.clients && statusData.clients.length > 0) {
        termPrint('═══ Registered Extensions ═══', 'var(--yellow)');
        termPrint(' EXT#     IP ADDRESS        PORT    STATUS', 'var(--cyan)');
        termPrint(' ─────────────────────────────────────────', 'var(--dk-cyan)');
        statusData.clients.forEach(c => {
          const ext = String(c.number || c.extension || '????').padEnd(9);
          const addrParts = (c.address || '?.?.?.?:?').split(':');
          const ip = String(addrParts[0] || '?.?.?.?').padEnd(18);
          const port = String(addrParts[1] || '?').padEnd(8);
          termPrint(' ' + ext + ip + port + '● ONLINE', 'var(--green)');
        });
      } else {
        termPrint(' No extensions currently registered.', 'var(--dk-gray)');
      }
      termPrint('');
      break;

    case 'sessions':
      termPrint('');
      if (statusData.sessions && statusData.sessions.length > 0) {
        termPrint('═══ Active Sessions ═══', 'var(--yellow)');
        termPrint(' CALLER     CALLEE     STATE        DURATION', 'var(--cyan)');
        termPrint(' ─────────────────────────────────────────────', 'var(--dk-cyan)');
        statusData.sessions.forEach(s => {
          const caller = String(s.caller || '?').padEnd(11);
          const callee = String(s.callee || '?').padEnd(11);
          const state = String(s.state || '?').padEnd(13);
          const dur = s.duration || '00:00';
          termPrint(' ' + caller + callee + state + dur, 'var(--fg)');
        });
      } else {
        termPrint(' No active sessions.', 'var(--dk-gray)');
      }
      termPrint('');
      break;

    case 'uptime':
      termPrint('');
      termPrint(' Server uptime: ' + formatUptime(statusData.uptime), 'var(--green)');
      termPrint(' Packets processed: ' + (statusData.packetsProcessed || 0).toLocaleString(), 'var(--cyan)');
      termPrint('');
      break;

    case 'oracle':
      termPrint('');
      oracleSpeak();
      termPrint('');
      break;

    case 'wifi':
      showWifi();
      termPrint(' WiFi manager opened.', 'var(--cyan)');
      break;

    case 'kill':
      if (parts.length < 2) {
        termPrint(' Usage: kill <extension_number>', 'var(--red)');
      } else {
        const ext = parts[1];
        termPrint(' Sending KILL signal to extension ' + ext + '...', 'var(--yellow)');
        fetch('/api/kill', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'extension=' + encodeURIComponent(ext)
        }).then(r => r.text()).then(t => {
          termPrint(' ' + (t || 'Extension terminated.'), 'var(--green)');
        }).catch(e => {
          termPrint(' ERROR: ' + e.message, 'var(--red)');
        });
      }
      break;

    case 'clear':
      termClear();
      break;

    case 'ver':
      termPrint('');
      termPrint(' pocket-dial v5.03', 'var(--yellow)');
      termPrint(' C++17 Compiler Engine', 'var(--cyan)');
      termPrint(' Built: May 27, 2026', 'var(--gray)');
      termPrint(' "CGA CRT Switchboard"', 'var(--dk-gray)');
      termPrint(' Public domain — Classic License.', 'var(--dk-gray)');
      termPrint('');
      break;

    case 'ascii':
      termPrint('');
      const bell = [
        '         ______         ',
        '        /      \\        ',
        '       |   ||   |       ',
        '       |   ||   |       ',
        '      /____||____\\      ',
        '     /            \\     ',
        '     \\____________/     ',
        '         (____)         ',
      ];
      bell.forEach(l => termPrint(l, 'var(--yellow)'));
      termPrint('');
      termPrint('  "beautiful-bell — CGA CRT SipServer"', 'var(--dk-gray)');
      termPrint('');
      break;
)rawhtml"
R"rawhtml(
    default:
      termPrint(' Unknown command: "' + escapeHtml(command) + '". Type "help" for commands.', 'var(--red)');
      break;
  }
}

function escapeHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

// Terminal input handling
termInput.addEventListener('keydown', function(e) {
  if (e.key === 'Enter') {
    const cmd = termInput.value;
    cmdHistory.unshift(cmd);
    if (cmdHistory.length > 50) cmdHistory.pop();
    cmdHistoryIdx = -1;
    termInput.value = '';
    processCommand(cmd);
  } else if (e.key === 'ArrowUp') {
    e.preventDefault();
    if (cmdHistoryIdx < cmdHistory.length - 1) {
      cmdHistoryIdx++;
      termInput.value = cmdHistory[cmdHistoryIdx];
    }
  } else if (e.key === 'ArrowDown') {
    e.preventDefault();
    if (cmdHistoryIdx > 0) {
      cmdHistoryIdx--;
      termInput.value = cmdHistory[cmdHistoryIdx];
    } else {
      cmdHistoryIdx = -1;
      termInput.value = '';
    }
  }
});

// Hide native cursor blink when typing
termInput.addEventListener('focus', () => { inputCursor.style.display = 'none'; });
termInput.addEventListener('blur',  () => { inputCursor.style.display = 'inline-block'; });

// Click anywhere in terminal area to focus input
document.getElementById('terminal-container').addEventListener('click', function(e) {
  if (e.target.id !== 'terminal-output') termInput.focus();
});

// ─── FORMAT UPTIME ───
function formatUptime(seconds) {
  if (!seconds && seconds !== 0) return '00:00:00';
  const s = Math.floor(seconds);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(sec).padStart(2,'0');
}

// ─── API POLLING ───
function fetchStatus() {
  fetch('/api/status')
    .then(r => r.json())
    .then(data => {
      statusData = data;
      updateUI(data);
    })
    .catch(() => {
      // silent fail
    });
}

function updateUI(d) {
  // Status panel
  document.getElementById('s-ip').textContent = d.ip || '0.0.0.0';
  document.getElementById('s-port').textContent = d.port || '5060';
  document.getElementById('s-uptime').textContent = formatUptime(d.uptime);
  document.getElementById('s-ext-count').textContent = (d.clients ? d.clients.length : 0);
  document.getElementById('s-call-count').textContent = (d.sessions ? d.sessions.length : 0);
  document.getElementById('s-packets').textContent = (d.packetsProcessed || 0).toLocaleString();

  // Mini indicators
  const callCount = d.sessions ? d.sessions.length : 0;
  const indCalls = document.getElementById('ind-calls');
  const indCallsLabel = document.getElementById('ind-calls-label');
  indCallsLabel.textContent = 'Calls: ' + callCount;
  if (callCount > 0) {
    indCalls.style.color = 'var(--green)';
    indCallsLabel.style.color = 'var(--green)';
  } else {
    indCalls.style.color = 'var(--dk-gray)';
    indCallsLabel.style.color = 'var(--dk-gray)';
  }
  document.getElementById('ind-pkts-label').textContent = 'Pkts: ' + (d.packetsProcessed || 0).toLocaleString();

  // Extensions table
  const extTbody = document.getElementById('ext-tbody');
  if (d.clients && d.clients.length > 0) {
    extTbody.innerHTML = d.clients.map(c => {
      const num = c.number || c.extension || '?';
      const addrParts = (c.address || c.ip || '?:?').split(':');
      const ip = addrParts[0] || '?';
      const port = addrParts[1] || c.port || '?';
      return '<tr>' +
        '<td style="color:var(--cyan);">' + escapeHtml(String(num)) + '</td>' +
        '<td>' + escapeHtml(String(ip)) + '</td>' +
        '<td>' + escapeHtml(String(port)) + '</td>' +
        '<td class="status-online">● ONLINE</td>' +
        '</tr>';
    }).join('');
  } else {
    extTbody.innerHTML = '<tr><td colspan="4" style="color:var(--dk-gray)">No extensions registered</td></tr>';
  }

  // Sessions table
  const sessTbody = document.getElementById('sess-tbody');
  if (d.sessions && d.sessions.length > 0) {
    sessTbody.innerHTML = d.sessions.map(s => {
      const stateColor = s.state === 'ESTABLISHED' ? 'var(--green)' :
                         s.state === 'RINGING' ? 'var(--yellow)' :
                         s.state === 'TRYING' ? 'var(--cyan)' : 'var(--fg)';
      const stateClass = s.state === 'RINGING' ? 'status-ringing' : '';
      return '<tr>' +
        '<td style="color:var(--cyan);">' + escapeHtml(String(s.caller || '?')) + '</td>' +
        '<td style="color:var(--cyan);">' + escapeHtml(String(s.callee || '?')) + '</td>' +
        '<td class="' + stateClass + '" style="color:' + stateColor + ';">' + escapeHtml(String(s.state || '?')) + '</td>' +
        '<td>' + escapeHtml(String(s.duration || '00:00')) + '</td>' +
        '</tr>';
    }).join('');
  } else {
    sessTbody.innerHTML = '<tr><td colspan="4" style="color:var(--dk-gray)">No active sessions</td></tr>';
  }
}

function refreshNow() {
  fetchStatus();
  termPrint(' Status refreshed.', 'var(--green)');
}

// ─── KEYBOARD SHORTCUTS ───
document.addEventListener('keydown', function(e) {
  if (e.key === 'F1') { e.preventDefault(); showHelp(); }
  if (e.key === 'F5') { e.preventDefault(); refreshNow(); }
  if (e.key === 'F7') { e.preventDefault(); oracleSpeak(); }
  if (e.key === 'F9') { e.preventDefault(); showWifi(); }
  if (e.key === 'Escape') {
    closeHelp(); closeWifi();
    termInput.focus();
  }
});

// ─── HELP MODAL ───
function showHelp() { document.getElementById('help-modal').style.display = 'block'; }
function closeHelp() { document.getElementById('help-modal').style.display = 'none'; }

// ─── WIFI MODAL ───
function showWifi() { document.getElementById('wifi-modal').style.display = 'block'; }
function closeWifi() { document.getElementById('wifi-modal').style.display = 'none'; }

function scanWifi() {
  const statusEl = document.getElementById('wifi-status');
  statusEl.textContent = 'Scanning...';
  statusEl.style.color = 'var(--yellow)';

  fetch('/api/wifi/scan')
    .then(r => r.json())
    .then(data => {
      statusEl.textContent = 'Scan complete — ' + (data.networks ? data.networks.length : 0) + ' networks found';
      statusEl.style.color = 'var(--green)';
      renderWifiNetworks(data.networks || []);
    })
    .catch(e => {
      statusEl.textContent = 'Scan failed: ' + e.message;
      statusEl.style.color = 'var(--red)';
    });
}

function renderWifiNetworks(networks) {
  const list = document.getElementById('wifi-networks-list');
  list.innerHTML = '';
  if (networks.length === 0) {
    const empty = document.createElement('div');
    empty.style.color = 'var(--dk-gray)';
    empty.textContent = 'No networks found.';
    list.appendChild(empty);
    return;
  }
  // Build each row as DOM nodes and bind the click handler programmatically.
  // The SSID never reaches markup as a string, so a crafted SSID (e.g. one
  // containing quotes) cannot break out into a script context.
  networks.forEach(n => {
    const ssid = String(n.ssid == null ? '' : n.ssid);
    const rssi = Number(n.rssi) || 0;
    const enc = n.encryption || 'OPEN';
    const bars = rssi > -50 ? '▂▄▆█' : rssi > -65 ? '▂▄▆░' : rssi > -75 ? '▂▄░░' : '▂░░░';

    const row = document.createElement('div');
    row.className = 'wifi-network';
    row.addEventListener('click', () => selectWifi(ssid));

    const ssidEl = document.createElement('span');
    ssidEl.className = 'wifi-ssid';
    ssidEl.textContent = ssid;

    const metaEl = document.createElement('span');
    const rssiEl = document.createElement('span');
    rssiEl.className = 'wifi-rssi';
    rssiEl.textContent = bars + ' ' + rssi + 'dBm';
    const encEl = document.createElement('span');
    encEl.className = 'wifi-enc';
    encEl.textContent = '[' + enc + ']';
    metaEl.appendChild(rssiEl);
    metaEl.appendChild(document.createTextNode(' '));
    metaEl.appendChild(encEl);

    row.appendChild(ssidEl);
    row.appendChild(metaEl);
    list.appendChild(row);
  });
}

function selectWifi(ssid) {
  selectedSSID = ssid;
  document.getElementById('wifi-selected-ssid').textContent = ssid;
  document.getElementById('wifi-connect-form').style.display = 'block';
  document.getElementById('wifi-password').value = '';
  document.getElementById('wifi-password').focus();
}
)rawhtml"
R"rawhtml(
function connectWifi() {
  const pw = document.getElementById('wifi-password').value;
  const statusEl = document.getElementById('wifi-status');
  statusEl.textContent = 'Connecting to ' + selectedSSID + '...';
  statusEl.style.color = 'var(--yellow)';

  fetch('/api/wifi/connect', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'ssid=' + encodeURIComponent(selectedSSID) + '&password=' + encodeURIComponent(pw)
  }).then(r => r.text()).then(t => {
    statusEl.textContent = 'Connected to ' + selectedSSID + '!';
    statusEl.style.color = 'var(--green)';
    cancelWifiConnect();
    termPrint(' WiFi: Connected to "' + selectedSSID + '"', 'var(--green)');
  }).catch(e => {
    statusEl.textContent = 'Connection failed: ' + e.message;
    statusEl.style.color = 'var(--red)';
    termPrint(' WiFi ERROR: ' + e.message, 'var(--red)');
  });
}

function cancelWifiConnect() {
  document.getElementById('wifi-connect-form').style.display = 'none';
  selectedSSID = '';
}

// ─── 3D WIREFRAME ROTATING TESSERACT ───
(function() {
  const canvas = document.getElementById('retro-canvas');
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  let angle = 0;

  // Tesseract vertices (a rotating 4D hypercube projected into 3D)
  const tesseractVerts = [
    // Inner cube (scale 0.35)
    [-0.35, -0.35,  0.35], // 0
    [ 0.35, -0.35,  0.35], // 1
    [ 0.35,  0.35,  0.35], // 2
    [-0.35,  0.35,  0.35], // 3
    [-0.35, -0.35, -0.35], // 4
    [ 0.35, -0.35, -0.35], // 5
    [ 0.35,  0.35, -0.35], // 6
    [-0.35,  0.35, -0.35], // 7
    // Outer cube (scale 0.75)
    [-0.75, -0.75,  0.75], // 8
    [ 0.75, -0.75,  0.75], // 9
    [ 0.75,  0.75,  0.75], // 10
    [-0.75,  0.75,  0.75], // 11
    [-0.75, -0.75, -0.75], // 12
    [ 0.75, -0.75, -0.75], // 13
    [ 0.75,  0.75, -0.75], // 14
    [-0.75,  0.75, -0.75], // 15
  ];

  // Edges
  const tesseractEdges = [
    // Inner cube
    [0,1],[1,2],[2,3],[3,0],
    [4,5],[5,6],[6,7],[7,4],
    [0,4],[1,5],[2,6],[3,7],
    // Outer cube
    [8,9],[9,10],[10,11],[11,8],
    [12,13],[13,14],[14,15],[15,12],
    [8,12],[9,13],[10,14],[11,15],
    // Connecting inner to outer
    [0,8],[1,9],[2,10],[3,11],
    [4,12],[5,13],[6,14],[7,15],
  ];

  function project(x, y, z) {
    const fov = 3.0;
    const dist = fov + z;
    const scale = fov / dist * (W * 0.38);
    return [W/2 + x * scale, H/2 + y * scale];
  }

  function rotateY(v, a) {
    const cos = Math.cos(a), sin = Math.sin(a);
    return [v[0]*cos + v[2]*sin, v[1], -v[0]*sin + v[2]*cos];
  }

  function rotateX(v, a) {
    const cos = Math.cos(a), sin = Math.sin(a);
    return [v[0], v[1]*cos - v[2]*sin, v[1]*sin + v[2]*cos];
  }

  function drawTesseract() {
    ctx.clearRect(0, 0, W, H);

    // Starfield background
    ctx.fillStyle = 'rgba(0, 0, 40, 1)';
    ctx.fillRect(0, 0, W, H);

    // Twinkling stars
    const starSeed = Math.floor(Date.now() / 500);
    for (let i = 0; i < 30; i++) {
      const sx = ((i * 7919 + starSeed * 13) % W);
      const sy = ((i * 6271 + starSeed * 7) % H);
      const brightness = ((i * 3571 + starSeed) % 3 === 0) ? 0.8 : 0.3;
      ctx.fillStyle = 'rgba(255,255,255,' + brightness + ')';
      ctx.fillRect(sx, sy, 1, 1);
    }

    // Transform and project vertices
    const tiltX = 0.35;
    const projected = tesseractVerts.map(v => {
      let rv = rotateX(v, tiltX);
      rv = rotateY(rv, angle);
      return project(rv[0], rv[1], rv[2]);
    });

    // Draw edges with cyan glow
    ctx.shadowColor = '#55FFFF';
    ctx.shadowBlur = 4;
    ctx.strokeStyle = '#55FFFF';
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    tesseractEdges.forEach(([a, b]) => {
      ctx.moveTo(projected[a][0], projected[a][1]);
      ctx.lineTo(projected[b][0], projected[b][1]);
    });
    ctx.stroke();

    // Draw vertices as dots
    ctx.shadowBlur = 6;
    ctx.fillStyle = '#FFFFFF';
    projected.forEach(p => {
      ctx.beginPath();
      ctx.arc(p[0], p[1], 1.5, 0, Math.PI * 2);
      ctx.fill();
    });

    ctx.shadowBlur = 0;

    // Glowing orbital ring around center
    const haloPhase = Date.now() * 0.001;
    const haloOpacity = 0.15 + 0.1 * Math.sin(haloPhase * 2);
    ctx.strokeStyle = 'rgba(255, 255, 85, ' + haloOpacity + ')';
    ctx.lineWidth = 1;
    ctx.beginPath();
    // Center tesseract halo
    const haloCenter3D = rotateY(rotateX([0, 0, 0], tiltX), angle);
    const hc = project(haloCenter3D[0], haloCenter3D[1], haloCenter3D[2]);
    ctx.arc(hc[0], hc[1], 35, 0, Math.PI * 2);
    ctx.stroke();

    angle += 0.008;
    requestAnimationFrame(drawTesseract);
  }

  drawTesseract();
})();

// ─── INIT ───
bootSequence();
fetchStatus();
setInterval(fetchStatus, 3000);
oracleInterval = setInterval(updateOracle, 12000);

// Focus terminal on load
setTimeout(() => termInput.focus(), 100);

</script>
</body>
</html>
)rawhtml";

#endif // INDEX_HTML_H
