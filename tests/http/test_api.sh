#!/usr/bin/env bash
# ==============================================================================
# pocket-dial HTTP API Verification Script
# ==============================================================================
# This script executes standard, boundary, safety, and security (CORS) test
# scenarios against a running pocket-dial target board.
#
# Usage:
#   ./test_api.sh [target_ip]
#   (Defaults to 192.168.4.1 if no argument is provided)
# ==============================================================================

# Terminal Colors for Premium output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0;1m' # No Color
RESET='\033[0m'

TARGET_IP="${1:-192.168.4.1}"
BASE_URL="http://${TARGET_IP}"

echo -e "${CYAN}======================================================================${RESET}"
echo -e "${CYAN}          POCKET-DIAL HTTP REST API AUTOMATED VERIFICATION           ${RESET}"
echo -e "${CYAN}======================================================================${RESET}"
echo -e "Target IP Address : ${NC}${TARGET_IP}${RESET}"
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

# Ensure clean temp files deletion on exit
cleanup() {
    rm -f temp_large_body.txt temp_resp_body.txt
}
trap cleanup EXIT

# ── TEST SUITE 1: HAPPY PATH ENPOINT VALIDATIONS ─────────────────────────────
print_suite "Happy Path & Content Type Delivery"

# TC-HP-01: Get Static Landing Page Dashboard
RESP_DATA=$(curl -s -w "\n%{http_code}" "${BASE_URL}/")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-HP-01: GET Landing Dashboard (/)" "200" "$HTTP_CODE"

# Check if dashboard actually serves the embedded HTML content
if [[ "$BODY_CONTENT" == *"<html"* || "$BODY_CONTENT" == *"<HTML"* ]]; then
    echo -e "  [${GREEN}PASS${RESET}] TC-HP-01: Landing Page served valid HTML structure."
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
  -H "Host: ${TARGET_IP}" \
  -H "Origin: http://${TARGET_IP}" \
  -d "extension=9999" "${BASE_URL}/api/kill")
HTTP_CODE=$(echo "$RESP_DATA" | tail -n1)
BODY_CONTENT=$(echo "$RESP_DATA" | sed '$d')
assert_status "TC-SEC-02: POST /api/kill (Same-Origin Header Validation)" "200" "$HTTP_CODE" "$BODY_CONTENT"

# TC-SEC-03: Cross-Origin Request (Malicious script on third-party tab) -> REJECT 403
RESP_DATA=$(curl -s -w "\n%{http_code}" -X POST \
  -H "Host: ${TARGET_IP}" \
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
