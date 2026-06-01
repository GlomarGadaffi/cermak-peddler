# Select transport: "wifi" for SoftAP mode, "eth" for W5500 Ethernet (PoE), "display" for Guition AXS15231B Display
# Override at build time:  idf.py -D SIP_TRANSPORT=display build
if(NOT DEFINED SIP_TRANSPORT)
    set(SIP_TRANSPORT "eth")      # default to Ethernet for Waveshare board
endif()

set(ADDITIONAL_SRCS "")

if(SIP_TRANSPORT STREQUAL "eth")
    set(MAIN_SRC "esp_main_eth.cpp")
    set(EXTRA_REQUIRES esp_eth driver)
    message(STATUS "SipServer transport: W5500 Ethernet (PoE)")
elseif(SIP_TRANSPORT STREQUAL "display")
    set(MAIN_SRC "esp_main_display.cpp")
    set(EXTRA_REQUIRES esp_wifi esp_lcd lvgl__lvgl)
    set(ADDITIONAL_SRCS "drivers/esp_lcd_axs15231b.c" "ui/ui.cpp" "wifi/DnsServer.cpp")
    message(STATUS "SipServer transport: Guition AXS15231B Touch Display (LVGL)")
else()
    set(MAIN_SRC "esp_main.cpp")
    set(EXTRA_REQUIRES esp_wifi)
    message(STATUS "SipServer transport: WiFi SoftAP")
endif()

idf_component_register(SRCS ${MAIN_SRC}
                            ${ADDITIONAL_SRCS}
                            "../src/Helpers/UdpServer.cpp"
                            "../src/Helpers/HttpServer.cpp"
                            "../src/SIP/RequestsHandler.cpp"
                            "../src/SIP/Session.cpp"
                            "../src/SIP/SipClient.cpp"
                            "../src/SIP/SipMessage.cpp"
                            "../src/SIP/SipMessageFactory.cpp"
                            "../src/SIP/SipSdpMessage.cpp"
                            "../src/SIP/SipServer.cpp"
                    INCLUDE_DIRS "../src/Helpers" "../src/SIP" "drivers" "ui" "wifi"
                    REQUIRES nvs_flash lwip esp_netif esp_event freertos espressif__mdns esp_driver_gpio esp_wifi esp_lcd lvgl__lvgl esp_eth driver)
