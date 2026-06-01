# Pocket-Dial Firmware: PR & Coding Standards Policy

This document establishes the mandatory code quality, memory safety, and performance standards for all firmware contributions to **pocket-dial**. All Pull Requests (PRs) must strictly comply with these rules to pass the automated gating pipelines and peer reviews.

---

## 1. Quality Control & PR Lifecycle Rules

### A. Maximum Diff Size Restrictions
To maintain thorough peer reviews, keep your code changes small and focused:
* **The Rule**: A single Pull Request must not contain more than **500 lines of modified code** (excluding auto-generated files, assets, or markdown documentation).
* **Rationale**: Large PRs hide bugs, increase lock contention on developer review cycles, and complicate rollback strategies.
* **Exceptions**: Major upstream refactoring campaigns may exceed this limit but require pre-approval from the lead architect.

### B. Required Review Checklist
Before any PR can be merged into `main`, it must receive at least **two approvals** from senior firmware maintainers verifying the following checklist:

- [ ] **No Dynamic Allocation**: Code executed within the `RequestsHandler` path or any network packet loop contains zero dynamic allocations.
- [ ] **Bounds-Checked Strings**: All string copy or formatting tasks utilize `strlcpy` or `snprintf` with explicit size boundaries.
- [ ] **Lock Hold Duration**: Mutex acquisitions inside signaling paths are kept short. Slow disk or socket I/O are never executed inside a locked scope.
- [ ] **Checked Returns**: All NVS flash, driver registrations, and socket syscall return codes are explicitly checked and handled.
- [ ] **No Unchecked Pointers**: Any pointer dereferencing has been pre-verified against `nullptr` (particularly in fallback/onboarding modes).
- [ ] **Core Affinity Alignment**: Pinned tasks match the dual-core topology and do not unbalance Core 0/1 workloads.

---

## 2. Prohibited Patterns & Technical Antipatterns

The following code patterns are strictly prohibited. The CI static analysis pipeline will flag and reject any commits containing these blocks.

### 🔴 Prohibited Pattern 1: Dynamic Allocation in Real-Time Path
Do not allocate memory on the heap within high-frequency loops or signaling pathways.

```cpp
/* ─────────────────────────── BAD: PROHIBITED ─────────────────────────── */
void RequestsHandler::onInvite(std::shared_ptr<SipMessage> data) {
    // VIOLATION: Heap allocation inside the UDP packet handling loop!
    auto newSession = std::make_shared<Session>(data->getCallID(), srcClient);
    _sessions[data->getCallID()] = newSession;
}

/* ─────────────────────────── GOOD: MANDATORY ─────────────────────────── */
void RequestsHandler::onInvite(std::shared_ptr<SipMessage> data) {
    // CORRECT: Recycle pre-allocated memory from the static session pool
    auto newSession = allocateSession(data->getCallID(), srcClient);
    if (!newSession) {
        sendResponse(503, "Service Unavailable");
        return;
    }
    _sessions.emplace(data->getCallID(), newSession);
}
```

---

### 🔴 Prohibited Pattern 2: Unbounded String Copy (strcpy / sprintf)
Using unbounded string copy commands introduces buffer overflow vulnerabilities.

```cpp
/* ─────────────────────────── BAD: PROHIBITED ─────────────────────────── */
void saveCredentials(const char* ssid, const char* pass) {
    wifi_config_t wifi_config;
    // VIOLATION: Stack buffer overflow if inputs exceed 32 or 64 bytes!
    strcpy((char*)wifi_config.ap.ssid, ssid);
    strcpy((char*)wifi_config.ap.password, pass);
}

/* ─────────────────────────── GOOD: MANDATORY ─────────────────────────── */
void saveCredentials(const char* ssid, const char* pass) {
    wifi_config_t wifi_config = {};
    // CORRECT: Copy with strict bounds limits
    strlcpy((char*)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
    strlcpy((char*)wifi_config.ap.password, pass, sizeof(wifi_config.ap.password));
}
```

---

### 🔴 Prohibited Pattern 3: Blocking Socket Calls Inside Mutex Locks
Never execute blocking network, file system, or flash operations while holding the primary registrar lock.

```cpp
/* ─────────────────────────── BAD: PROHIBITED ─────────────────────────── */
void RequestsHandler::handle(std::shared_ptr<SipMessage> request) {
    std::lock_guard<std::mutex> lock(_mutex);
    
    // VIOLATION: Holding registrar mutex while calling a blocking network syscall!
    // This can stall the entire signaling thread for milliseconds.
    sendto(_socket, response.c_str(), response.size(), 0, &dest, sizeof(dest));
}

/* ─────────────────────────── GOOD: MANDATORY ─────────────────────────── */
void RequestsHandler::handle(std::shared_ptr<SipMessage> request) {
    std::vector<std::pair<sockaddr_in, std::shared_ptr<SipMessage>>> localOutbox;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // CORRECT: Buffer generated responses into a local outbox inside the lock
        _outbox.emplace_back(dest, std::move(response));
        localOutbox = std::move(_outbox);
    }
    
    // CORRECT: Fire socket syscalls outside the lock context
    for (auto& event : localOutbox) {
        _onHandled(event.first, std::move(event.second));
    }
}
```

---

### 🔴 Prohibited Pattern 4: Unchecked System Return Codes
Ignoring return codes of critical systems (such as NVS flash, drivers, or network operations) will lead to hard-to-debug device states.

```cpp
/* ─────────────────────────── BAD: PROHIBITED ─────────────────────────── */
void setupNetworkMode() {
    nvs_handle_t nvs_handle;
    nvs_open("storage", NVS_READONLY, &nvs_handle);
    // VIOLATION: wifi_mode retains uninitialized stack garbage if key is missing!
    uint8_t wifi_mode;
    nvs_get_u8(nvs_handle, "wifi_mode", &wifi_mode);
    nvs_close(nvs_handle);
}

/* ─────────────────────────── GOOD: MANDATORY ─────────────────────────── */
void setupNetworkMode() {
    nvs_handle_t nvs_handle;
    uint8_t wifi_mode = 0; // CORRECT: Safe default initializer
    
    if (nvs_open("storage", NVS_READONLY, &nvs_handle) == ESP_OK) {
        // CORRECT: Check each return code and apply safe fallback on error
        if (nvs_get_u8(nvs_handle, "wifi_mode", &wifi_mode) != ESP_OK) {
            wifi_mode = 0; // AP onboarding fallback
        }
        nvs_close(nvs_handle);
    } else {
        wifi_mode = 0; // AP onboarding fallback
    }
}
```

---

## 3. Concurrency & Task Affinity Directives

1. **Keep lvgl_task Isolated**: Under no circumstances should non-UI networking or file I/O operations be dispatched onto Core 1 on display-enabled hardware configurations.
2. **Utilize Double-Buffered Getters**: Any state data required by the HTTP server or display tasks from the registrar must be queried via snapshotted APIs (`getActiveClients()`, `getActiveSessions()`). Do not introduce raw mutex sharing across Core 0 and Core 1.
3. **Interrupt Service Routines (ISRs)**: ISR handlers must strictly avoid blocking calls, standard RTOS queue inserts, or any console print operations. Only `FromISR` suffix functions (e.g. `xQueueSendFromISR`) are permitted inside hardware interrupts.
