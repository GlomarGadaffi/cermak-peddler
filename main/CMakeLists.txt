# Select transport: "wifi" for SoftAP mode, "eth" for W5500 Ethernet (PoE)
# Override at build time:  idf.py -D SIP_TRANSPORT=eth build
if(NOT DEFINED SIP_TRANSPORT)
    set(SIP_TRANSPORT "eth")      # default to Ethernet for Waveshare board
endif()

if(SIP_TRANSPORT STREQUAL "eth")
    set(MAIN_SRC "esp_main_eth.cpp")
    set(EXTRA_REQUIRES esp_eth driver)
    message(STATUS "SipServer transport: W5500 Ethernet (PoE)")
else()
    set(MAIN_SRC "esp_main.cpp")
    set(EXTRA_REQUIRES esp_wifi)
    message(STATUS "SipServer transport: WiFi SoftAP")
endif()

idf_component_register(SRCS ${MAIN_SRC}
                            "../src/Helpers/UdpServer.cpp"
                            "../src/Helpers/HttpServer.cpp"
                            "../src/SIP/RequestsHandler.cpp"
                            "../src/SIP/Session.cpp"
                            "../src/SIP/SipClient.cpp"
                            "../src/SIP/SipMessage.cpp"
                            "../src/SIP/SipMessageFactory.cpp"
                            "../src/SIP/SipSdpMessage.cpp"
                            "../src/SIP/SipServer.cpp"
                    INCLUDE_DIRS "../src/Helpers" "../src/SIP"
                    REQUIRES nvs_flash lwip esp_netif esp_event freertos ${EXTRA_REQUIRES})
