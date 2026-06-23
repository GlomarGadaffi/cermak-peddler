cmake_minimum_required(VERSION 3.11)
project(pocket_dial_tests CXX)

# The codebase is C++17 (std::string_view / std::optional in SipMessage, the digest
# store, etc.). GCC/Clang default to C++17, so CI was implicitly fine, but MSVC
# defaults to C++14 and fails to parse those headers. Pin the standard explicitly so
# the host suite builds identically on every toolchain.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include_directories(${gtest_SOURCE_DIR}/include ../src/SIP ../src/Helpers)

add_executable(sip_parser_tests
    SipMessage_test.cpp
    SipMessage_header_test.cpp
    SipStatus_test.cpp
    SipDigest_test.cpp
    ArpLookup_test.cpp
    Pbx_test.cpp
    PageZone_test.cpp
    Rtp_test.cpp
    RtpReceiver_test.cpp
    PlayoutBuffer_test.cpp
    AnchorClient_test.cpp
    Tui_test.cpp
    # The ANSI sysop-terminal renderer is transport-agnostic and host-compilable
    # (no wolfSSH, no ESP — only an esp_timer guard, behind ESP_PLATFORM). It links
    # cleanly into the host test binary so the banner/hub/mono-fallback render is
    # asserted on the desktop.
    ../src/Helpers/Tui.cpp
    # SIP digest-auth primitives + per-extension HA1 secret store. Both are
    # host-compilable: SipDigest vendors a portable MD5 (no mbedTLS) and
    # SipSecretStore uses an in-memory map under !ESP_PLATFORM (no NVS).
    ../src/Helpers/SipDigest.cpp
    ../src/Helpers/SipSecretStore.cpp
    # ArpLookup compiles to a host stub (returns nullopt — no ARP table off-device)
    # under !ESP_PLATFORM, so it links cleanly into the host test binary and lets
    # RequestsHandler's Learn-mode path build on the desktop.
    ../src/Helpers/ArpLookup.cpp
    # AdminAuth is self-contained (vendored SHA-256, host-stubbed NVS/RNG under
    # !ESP_PLATFORM). RequestsHandler::onDtmfInfo references AdminAuth::verifyPin for
    # the DTMF *PIN# admin menu, so it must be linked into the host binary too.
    ../src/Helpers/AdminAuth.cpp
    ../src/SIP/SipMessage.cpp
    ../src/SIP/SipStatus.cpp
    # Media beachhead (RTP sender + 440 media extension). RtpSender.cpp compiles to
    # host stubs (no sockets/FreeRTOS) under !ESP_PLATFORM; RequestsHandler.cpp +
    # SipSdpMessage/SipClient/Session are pulled in so the 440 SDP-answer and the
    # single-stream cap are unit-testable on the desktop build.
    ../src/SIP/RtpSender.cpp
    # Inbound media path (RtpReceiver): mirror of RtpSender. Host-stubbed under
    # !ESP_PLATFORM so the µ-law decode + RTP-parse helpers and the single-stream
    # cap are unit-testable on the desktop build (see RtpReceiver_test.cpp).
    ../src/SIP/RtpReceiver.cpp
    # Adaptive jitter buffer used by AnchorClient implementations. Pure C++17;
    # no ESP/socket dependencies. Exercises write/read/overrun/underrun paths.
    ../src/SIP/PlayoutBuffer.cpp
    # AnchorClient reference implementation. Pure loopback (no network); exercises
    # the AnchorClient contract: init/start/makeCall/audio/drop/stop lifecycle.
    ../src/SIP/LoopbackAnchorClient.cpp
    ../src/SIP/RequestsHandler.cpp
    ../src/SIP/SipSdpMessage.cpp
    ../src/SIP/SipClient.cpp
    ../src/SIP/Session.cpp
)

target_link_libraries(sip_parser_tests PRIVATE gtest gtest_main)

if (WIN32)
    target_link_libraries(sip_parser_tests PRIVATE wsock32 ws2_32)
else()
    target_link_libraries(sip_parser_tests PRIVATE pthread)
endif()

add_test(NAME sip_parser_tests COMMAND sip_parser_tests)
