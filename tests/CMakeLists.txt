cmake_minimum_required(VERSION 3.10)
project(pocket_dial_tests)

enable_testing()
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS} ../src)

add_executable(sip_parser_tests
    SipMessage_test.cpp
    SipMessage_header_test.cpp
)

target_link_libraries(sip_parser_tests PRIVATE ${GTEST_LIBRARIES} pthread)

add_test(NAME sip_parser_tests COMMAND sip_parser_tests)
