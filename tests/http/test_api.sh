#!/usr/bin/env bash
# ==============================================================================
# pocket-dial HTTP API Verification Script  (single source of truth)
# ==============================================================================
# This script executes standard, boundary, safety, security (CORS), OTA, and
# admin-auth test scenarios against a running pocket-dial target.
#
# It is the ONE smoke suite used both:
#   * locally / in CI against the host build  (SipServer on 127.0.0.1:8080), and
#   * on real hardware against the device captive-portal AP (192.168.4.1).
#
# Usage:
#   ./test_api.sh [target]
#       target may be a bare IP/host ("192.168.4.1") OR host:port
#       ("127.0.0.1:8080"). Defaults to 192.168.4.1 (device AP) if omitted.
#
# Optional environment:
#   SERVER_PID   If set (host/CI), the OTA-reboot test asserts that this PID is
#                STILL ALIVE afterwards (the desktop reboot endpoint must be a
#                no-op and must NOT exit the process). Ignored on real hardware.
#
# Test ORDERING is deliberate and load-bearing:
#   1. Happy path            (works while unprovisioned/open)
#   2. CSRF / same-origin
#   3. Input validation      (16 KB body cap -> 413 on the buffered endpoints)
#   4. Routing / 404
#   5. OTA                   (run while STILL UNPROVISIONED, so the upload gate
#                             is open and we can prove the streaming path returns
#                             501 — NOT 413 — for a multi-KB body)
#   6. Admin auth            (LAST: this SETS A PIN, which flips every mutating
#                             endpoint — /api/kill, /api/ota/upload, ... — to 401
#                             without a session cookie, so it must not run before
#                             the suites above)
# ==============================================================================

# Terminal Colors for Premium output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0;1m' # No Color
RESET='\033[0m'

TARGET_INPUT="${1:-192.168.4.1}"
# Accept either "host" or "host:port". Build BASE_URL accordingly and derive a
# bare host (TARGET_IP) for the Host/Origin headers the same-origin check reads.
if [[ "$TARGET_INPUT" == *:* ]]; then
    TARGET_IP="${TARGET_INPUT%%:*}"
    BASE_URL="http://${TARGET_INPUT}"
    ORIGIN_HDR="http://${TARGET_INPUT}"
    HOST_HDR="${TARGET_INPUT}"
else
    TARGET_IP="${TARGET_INPUT}"
    BASE_URL="http://${TARGET_INPUT}"
    ORIGIN_HDR="http://${TARGET_INPUT}"
    HOST_HDR="${TARGET_INPUT}"
fi

echo -e "${CYAN}======================================================================${RESET}"
echo -e "${CYAN}          POCKET-DIAL HTTP REST API AUTOMATED VERIFICATION           ${RESET}"
echo -e "${CYAN}======================================================================${RESET}"
echo -e "Target            : ${NC}${TARGET_INPUT}${RESET}"
echo -e "Base Connection   : ${NC}${BASE_URL}/${RESET}"
echo -e "${CYAN}======================================================================${RESET}\n"

# Statistics trackers
PASSED_TESTS=0
FAILED_TESTS=0

# Helper function to print test header
print_suite() {
    echo -e "\n${BLUE}● Suite: $1${RESET}"
}

# Helper function to assert HTTP Status Codes
assert_status() {
    local test_name="$1"
    local expected_code="$2"
    local actual_code="$3"
    local body="$4"

    if [ "$actual_code" -eq "$expected_code" ]; then
        echo -e "  [${GREEN}PASS${RESET}] ${test_name} (Got ${actual_code}, Expected ${expected_code})"
        ((PASSED_TESTS++))
    else
        echo -e "  [${RED}FAIL${RESET}] ${test_name} (Got ${actual_code}, Expected ${expected_code})"
        if [ -n "$body" ]; then
            echo -e "         Response Body: ${YELLOW}${body}${RESET}"
        fi
        ((FAILED_TESTS++))
    fi
}

# Generic boolean assertion (for content / liveness checks).
assert_true() {
    local test_name="$1"
    local condition="$2"   # "0" == true/pass (shell convention via [ ] exit code passed as string)
    if [ "$condition" = "0" ]; then
        echo -e "  [${GREEN}PASS${RESET}] ${test_name}"
        ((PASSED_TESTS++))
    else
        echo -e "  [${RED}FAIL${RESET}] ${test_name}"
        ((FAILED_TESTS++))
    fi
}

# Ensure clean temp files deletion on exit
cleanup() {
    rm -f temp_large_body.txt temp_ota_body.txt temp_resp_body.txt
}
trap cleanup EXIT

# ── TEST SUITE 1: HAPPY PATH ENDPOINT VALIDATIONS ────────────────────────────
print_suite "Happy Path & Content Type Delivery"

# TC-HP-01: Get Static Landing Page Dashboard
RESP_DATA=$(curl -s -w "\n%{http_code}" "${BASE_URL}/")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-HP-01: GET Landing Dashboard (/)" "200" "$HTTP_CODE"

# Check if dashboard actually serves the embedded HTML content
if [[ "$BODY_CONTENT" == *"<html"* || "$BODY_CONTENT" == *"<HTML"* ]]; then
    echo -e "  [${GREEN}PASS${RESET}] TC-HP-01: Landing Page served valid HTML structure."
    ((PASSED_TESTS++))
else
    echo -e "  [${RED}FAIL${RESET}] TC-HP-01: Landing Page response did not contain expected HTML signature."
    ((FAILED_TESTS++))
fi

# TC-HP-02: Get Active System Status JSON Snapshot
RESP_DATA=$(curl -s -w "\n%{http_code}" "${BASE_URL}/api/status")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-HP-02: GET Status API Snapshot (/api/status)" "200" "$HTTP_CODE" "$BODY_CONTENT"

# Check JSON signature fields to prove snapshot works correctly
if [[ "$BODY_CONTENT" == *"uptime"* && "$BODY_CONTENT" == *"packetsProcessed"* && "$BODY_CONTENT" == *"clients"* ]]; then
    echo -e "  [${GREEN}PASS${RESET}] TC-HP-02: Status snapshot has complete metrics schema."
    ((PASSED_TESTS++))
else
    echo -e "  [${RED}FAIL${RESET}] TC-HP-02: Status snapshot has incomplete JSON schema."
    ((FAILED_TESTS++))
fi


# ── TEST SUITE 2: CSRF & SAME-ORIGIN SECURITY CHECK ──────────────────────────
print_suite "Same-Origin (CSRF) Security Verification"

# TC-SEC-01: Direct Request (No Origin Header, e.g. manual curl/script) -> ALLOW
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST -d "extension=9999" "${BASE_URL}/api/kill")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-SEC-01: POST /api/kill (Direct Action - No Origin Header)" "200" "$HTTP_CODE" "$BODY_CONTENT"

# TC-SEC-02: Same-Origin Request (Matching Origin and Host) -> ALLOW
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: ${ORIGIN_HDR}" \
  -d "extension=9999" "${BASE_URL}/api/kill")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-SEC-02: POST /api/kill (Same-Origin Header Validation)" "200" "$HTTP_CODE" "$BODY_CONTENT"

# TC-SEC-03: Cross-Origin Request (Malicious script on third-party tab) -> REJECT 403
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: http://malicious-attacker-domain.com" \
  -d "extension=9999" "${BASE_URL}/api/kill")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-SEC-03: POST /api/kill (Cross-Origin Request Protection)" "403" "$HTTP_CODE" "$BODY_CONTENT"


# ── TEST SUITE 3: INPUT VALIDATION & LIMIT BOUNDS ────────────────────────────
print_suite "Input Validation, Schema Bounds & Limits"

# TC-ED-01: Payload size restriction (Capped at 16 KB) -> REJECT 413
echo -e "${YELLOW}  * Generating 17 KB oversized mock body...${RESET}"
# Generate exactly 17,408 bytes of 'A' (17 KB) to exceed 16 KB limits
dd if=/dev/zero bs=1024 count=17 2>/dev/null | tr '\0' 'A' > temp_large_body.txt

RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data-binary @temp_large_body.txt \
  "${BASE_URL}/api/wifi/connect")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-ED-01: POST /api/wifi/connect (Oversized payload > 16 KB check)" "413" "$HTTP_CODE" "$BODY_CONTENT"

# TC-ED-02: Missing Kill Extension Parameter -> REJECT 400
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST "${BASE_URL}/api/kill")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-ED-02: POST /api/kill (Empty body / missing parameter check)" "400" "$HTTP_CODE" "$BODY_CONTENT"

# TC-ED-03: Missing Connect SSID Parameter -> REJECT 400
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST -d "password=testpass" "${BASE_URL}/api/wifi/connect")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-ED-03: POST /api/wifi/connect (Missing SSID parameter check)" "400" "$HTTP_CODE" "$BODY_CONTENT"


# ── TEST SUITE 4: NON-IMPLEMENTED ENDPOINTS (DESKTOP MODE BEHAVIOR) ──────────
print_suite "Platform Environment Routing & Mock Capabilities"

# TC-ED-04: Non-implemented routes check
RESP_DATA=$(curl -s -w "\n%{http_code}" "${BASE_URL}/api/invalid-route-name-xyz")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-ED-04: GET Invalid Path (Returns 404)" "404" "$HTTP_CODE" "$BODY_CONTENT"


# ── TEST SUITE 5: OTA UPDATE ENDPOINTS (RUN WHILE UNPROVISIONED) ─────────────
# These MUST execute before any PIN is set so the upload/reboot auth gate is
# still open. The headline assertion is the streaming-cap-bypass regression
# guard: a multi-KB OTA body must reach the streaming handler (501 on host),
# NOT trip the 16 KB buffered-body cap (413).
print_suite "OTA Firmware-Update Surface (pre-provisioning)"

# TC-OTA-01: GET /api/ota/status -> 200, ungated, reports otaSupported flag.
RESP_DATA=$(curl -s -w "\n%{http_code}" "${BASE_URL}/api/ota/status")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-OTA-01: GET /api/ota/status (ungated introspection)" "200" "$HTTP_CODE" "$BODY_CONTENT"
# On the host build the stub reports otaSupported:false; on device it is true.
# We only assert the field is present and well-formed here.
if [[ "$BODY_CONTENT" == *'"otaSupported"'* && "$BODY_CONTENT" == *'"running"'* ]]; then
    echo -e "  [${GREEN}PASS${RESET}] TC-OTA-01: ota/status has otaSupported + partition fields."
    ((PASSED_TESTS++))
else
    echo -e "  [${RED}FAIL${RESET}] TC-OTA-01: ota/status missing expected schema fields."
    ((FAILED_TESTS++))
fi

# TC-OTA-02: Cross-Origin OTA upload -> REJECT 403 (same gate as other mutations).
echo -e "${YELLOW}  * Generating 32 KB mock firmware body...${RESET}"
dd if=/dev/zero bs=1024 count=32 2>/dev/null | tr '\0' 'B' > temp_ota_body.txt

RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: http://malicious-attacker-domain.com" \
  --data-binary @temp_ota_body.txt \
  "${BASE_URL}/api/ota/upload")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-OTA-02: POST /api/ota/upload (Cross-Origin rejected)" "403" "$HTTP_CODE" "$BODY_CONTENT"

# TC-OTA-03: Same-origin OTA upload of a 32 KB body.
#   REGRESSION GUARD: the streaming interception bypasses the 16 KB buffered cap,
#   so this must NOT be 413. On host the stub drains the body and returns 501;
#   on device it would proceed to flash. We accept the device-or-host outcome
#   but explicitly FAIL on 413 (the cap-bypass regression).
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: ${ORIGIN_HDR}" \
  --data-binary @temp_ota_body.txt \
  "${BASE_URL}/api/ota/upload")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
if [ "$HTTP_CODE" = "413" ]; then
    echo -e "  [${RED}FAIL${RESET}] TC-OTA-03: 32 KB OTA upload hit the 16 KB buffered cap (got 413 — streaming bypass REGRESSED)."
    echo -e "         Response Body: ${YELLOW}${BODY_CONTENT}${RESET}"
    ((FAILED_TESTS++))
else
    # Host stub -> 501 (Not Implemented). This is the expected CI outcome.
    assert_status "TC-OTA-03: POST /api/ota/upload (32 KB streams past 16 KB cap; host stub 501)" "501" "$HTTP_CODE" "$BODY_CONTENT"
fi

# TC-OTA-04: Empty-body OTA upload (Content-Length: 0) -> REJECT 411.
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: ${ORIGIN_HDR}" \
  -H "Content-Length: 0" \
  "${BASE_URL}/api/ota/upload")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-OTA-04: POST /api/ota/upload (Content-Length: 0 -> 411)" "411" "$HTTP_CODE" "$BODY_CONTENT"

# TC-OTA-05: Cross-Origin reboot -> REJECT 403.
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: http://malicious-attacker-domain.com" \
  "${BASE_URL}/api/ota/reboot")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-OTA-05: POST /api/ota/reboot (Cross-Origin rejected)" "403" "$HTTP_CODE" "$BODY_CONTENT"

# TC-OTA-06: Same-origin reboot -> 200 (host stub is a no-op and must NOT exit).
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: ${ORIGIN_HDR}" \
  "${BASE_URL}/api/ota/reboot")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
# On a real device with no staged image this is 409; on host it is a simulated
# 200. Accept either of those, but treat a 5xx/crash as failure.
if [ "$HTTP_CODE" = "200" ] || [ "$HTTP_CODE" = "409" ]; then
    echo -e "  [${GREEN}PASS${RESET}] TC-OTA-06: POST /api/ota/reboot same-origin (Got ${HTTP_CODE})"
    ((PASSED_TESTS++))
else
    echo -e "  [${RED}FAIL${RESET}] TC-OTA-06: POST /api/ota/reboot same-origin (Got ${HTTP_CODE}, expected 200 or 409)"
    echo -e "         Response Body: ${YELLOW}${BODY_CONTENT}${RESET}"
    ((FAILED_TESTS++))
fi

# TC-OTA-07: If SERVER_PID is provided (host/CI), the reboot stub must have left
# the process running (a real esp_restart() would be fatal off-device).
if [ -n "${SERVER_PID:-}" ]; then
    if kill -0 "${SERVER_PID}" 2>/dev/null; then
        echo -e "  [${GREEN}PASS${RESET}] TC-OTA-07: Server PID ${SERVER_PID} still alive after reboot (host stub is a no-op)."
        ((PASSED_TESTS++))
    else
        echo -e "  [${RED}FAIL${RESET}] TC-OTA-07: Server PID ${SERVER_PID} died after /api/ota/reboot — desktop reboot must NOT exit."
        ((FAILED_TESTS++))
    fi
fi


# ── TEST SUITE 6: ADMIN AUTH  (RUN LAST — IT PROVISIONS A PIN) ───────────────
# Setting a PIN turns the mutating endpoints (incl. /api/kill and
# /api/ota/upload) into 401-without-cookie, so every preceding suite assumed the
# OPEN/unprovisioned state and therefore had to run first.
print_suite "Admin Authentication & Session Gating (provisioning)"

# TC-AUTH-01: status reports unprovisioned before we set a PIN.
RESP_DATA=$(curl -s -w "\n%{http_code}" "${BASE_URL}/api/admin/status")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-AUTH-01: GET /api/admin/status (reachable)" "200" "$HTTP_CODE" "$BODY_CONTENT"
if [[ "$BODY_CONTENT" == *'"provisioned":false'* ]]; then
    echo -e "  [${GREEN}PASS${RESET}] TC-AUTH-01: device reports provisioned:false initially."
    ((PASSED_TESTS++))
else
    echo -e "  [${RED}FAIL${RESET}] TC-AUTH-01: expected provisioned:false, got: ${YELLOW}${BODY_CONTENT}${RESET}"
    ((FAILED_TESTS++))
fi

# TC-AUTH-02: cross-origin set-pin is rejected (403) before anything else.
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: http://malicious-attacker-domain.com" \
  -d "pin=1234" "${BASE_URL}/api/admin/set-pin")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-AUTH-02: POST /api/admin/set-pin (Cross-Origin rejected)" "403" "$HTTP_CODE" "$BODY_CONTENT"

# TC-AUTH-03: same-origin set-pin while unprovisioned -> 200 (first-run onboarding).
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: ${ORIGIN_HDR}" \
  -d "pin=1234" "${BASE_URL}/api/admin/set-pin")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-AUTH-03: POST /api/admin/set-pin (Same-Origin first-run -> 200)" "200" "$HTTP_CODE" "$BODY_CONTENT"

# TC-AUTH-04: status now reports provisioned:true.
RESP_DATA=$(curl -s -w "\n%{http_code}" "${BASE_URL}/api/admin/status")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
if [[ "$BODY_CONTENT" == *'"provisioned":true'* ]]; then
    echo -e "  [${GREEN}PASS${RESET}] TC-AUTH-04: device reports provisioned:true after set-pin."
    ((PASSED_TESTS++))
else
    echo -e "  [${RED}FAIL${RESET}] TC-AUTH-04: expected provisioned:true, got: ${YELLOW}${BODY_CONTENT}${RESET}"
    ((FAILED_TESTS++))
fi

# TC-AUTH-05: a mutating endpoint with NO session cookie is now 401.
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: ${ORIGIN_HDR}" \
  -d "extension=123" "${BASE_URL}/api/kill")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-AUTH-05: POST /api/kill (provisioned, no cookie -> 401)" "401" "$HTTP_CODE" "$BODY_CONTENT"

# TC-AUTH-06: login with the correct PIN -> 200, a pd_session cookie, AND the
# per-session CSRF token in the response body. The token is captured here because
# every mutating request below needs it: on a provisioned device the session
# cookie alone is deliberately not sufficient (see TC-AUTH-07a).
# -i (headers + body together on stdout) rather than -D to a temp file: nothing
# to clean up, and it keeps working under a curl whose filesystem view differs
# from the shell's (e.g. a Windows curl.exe invoked from WSL, which cannot write
# to a /tmp path the shell just created).
LOGIN_RAW=$(curl -s -i -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: ${ORIGIN_HDR}" \
  -d "pin=1234" "${BASE_URL}/api/admin/login")
# Split on the first blank line: headers before it, body after.
LOGIN_HEADERS=$(printf '%s' "$LOGIN_RAW" | sed -n '1,/^\r*$/p')
LOGIN_BODY=$(printf '%s' "$LOGIN_RAW" | sed '1,/^\r*$/d')
LOGIN_CODE=$(printf '%s' "$LOGIN_HEADERS" | grep -i "^HTTP/" | tail -n1 | awk '{print $2}')
assert_status "TC-AUTH-06: POST /api/admin/login (correct PIN -> 200)" "200" "${LOGIN_CODE:-0}" "$LOGIN_HEADERS"

# Extract the pd_session token from the Set-Cookie header.
SESSION=$(printf '%s' "$LOGIN_HEADERS" \
    | grep -i "^set-cookie:" \
    | sed -n 's/.*pd_session=\([0-9a-fA-F]*\).*/\1/p' \
    | head -n1)
if [ -n "$SESSION" ]; then
    echo -e "  [${GREEN}PASS${RESET}] TC-AUTH-06: login issued a pd_session cookie (len ${#SESSION})."
    ((PASSED_TESTS++))
else
    echo -e "  [${RED}FAIL${RESET}] TC-AUTH-06: login did not return a pd_session cookie."
    ((FAILED_TESTS++))
fi

# Extract the CSRF token from the login response body.
CSRF=$(printf '%s' "$LOGIN_BODY" \
    | sed -n 's/.*"csrf":"\([0-9a-fA-F]*\)".*/\1/p' \
    | head -n1)
if [ -n "$CSRF" ]; then
    echo -e "  [${GREEN}PASS${RESET}] TC-AUTH-06: login returned a CSRF token (len ${#CSRF})."
    ((PASSED_TESTS++))
else
    echo -e "  [${RED}FAIL${RESET}] TC-AUTH-06: login did not return a CSRF token. Body: ${YELLOW}${LOGIN_BODY}${RESET}"
    ((FAILED_TESTS++))
fi

# TC-AUTH-07a: the cookie ALONE is no longer enough. The same-origin check
# deliberately admits a request with no Origin header (curl and scripts send
# none), so the per-session CSRF token is what actually stops a same-site page
# from riding the victim's cookie. Cookie without token must be refused.
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: ${ORIGIN_HDR}" \
  -H "Cookie: pd_session=${SESSION}" \
  -d "extension=123" "${BASE_URL}/api/kill")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-AUTH-07a: POST /api/kill (cookie but NO CSRF token -> 403)" "403" "$HTTP_CODE" "$BODY_CONTENT"

# TC-AUTH-07b: cookie + CSRF token succeeds (200).
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: ${ORIGIN_HDR}" \
  -H "Cookie: pd_session=${SESSION}" \
  -H "X-CSRF: ${CSRF}" \
  -d "extension=123" "${BASE_URL}/api/kill")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-AUTH-07b: POST /api/kill (cookie + CSRF token -> 200)" "200" "$HTTP_CODE" "$BODY_CONTENT"

# TC-AUTH-08: brute-force lockout — 5 consecutive wrong PINs trip a 429.
#   verifyPin engages the lockout on the 5th failure, so by the 5th attempt the
#   login endpoint must answer 429 (Too Many Requests). We log out first so the
#   valid session above does not interfere (logout needs the cookie+origin).
curl -s -o /dev/null -X POST \
  -H "Host: ${HOST_HDR}" \
  -H "Origin: ${ORIGIN_HDR}" \
  -H "Cookie: pd_session=${SESSION}" \
  "${BASE_URL}/api/admin/logout"

LOCKED_OUT=1
for attempt in 1 2 3 4 5; do
    WRONG_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST \
      -H "Host: ${HOST_HDR}" \
      -H "Origin: ${ORIGIN_HDR}" \
      -d "pin=0000" "${BASE_URL}/api/admin/login")
    echo -e "         attempt ${attempt}: /api/admin/login (wrong PIN) -> ${WRONG_CODE}"
    if [ "$WRONG_CODE" = "429" ]; then
        LOCKED_OUT=0
    fi
done
assert_true "TC-AUTH-08: 5x wrong PIN engages 429 lockout" "$LOCKED_OUT"


# ── FINAL VERIFICATION SUMMARY REPORT ─────────────────────────────────────────
echo -e "\n${CYAN}======================================================================${RESET}"
echo -e "${CYAN}                     API TEST EXECUTION SUMMARY                      ${RESET}"
echo -e "${CYAN}======================================================================${RESET}"
echo -e "  Total Tests Executed : $((PASSED_TESTS + FAILED_TESTS))"
echo -e "  Tests Passed         : ${GREEN}${PASSED_TESTS}${RESET}"
if [ "$FAILED_TESTS" -eq 0 ]; then
    echo -e "  Tests Failed         : ${GREEN}0 (SUCCESS)${RESET}"
    echo -e "${CYAN}======================================================================${RESET}"
    echo -e "${GREEN}>>> SUCCESS: The pocket-dial firmware matches all REST specifications!${RESET}"
    exit 0
else
    echo -e "  Tests Failed         : ${RED}${FAILED_TESTS}${RESET}"
    echo -e "${CYAN}======================================================================${RESET}"
    echo -e "${RED}>>> FAILURE: Some test cases did not meet REST specifications!${RESET}"
    exit 1
fi
