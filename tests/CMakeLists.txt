cmake_minimum_required(VERSION 3.11)
project(pocket_dial_tests)

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
    ../src/SIP/SipMessage.cpp
)

target_link_libraries(sip_parser_tests PRIVATE gtest gtest_main)

if (WIN32)
    target_link_libraries(sip_parser_tests PRIVATE wsock32 ws2_32)
else()
    target_link_libraries(sip_parser_tests PRIVATE pthread)
endif()

add_test(NAME sip_parser_tests COMMAND sip_parser_tests)
