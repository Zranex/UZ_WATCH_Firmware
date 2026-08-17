set(SRCS
    "src/bsp_board_extra.c"
    "src/pcf85063a.c"
    "src/rtc_lib.c"
    "src/wifi_manager.c"
    "src/ble_manager.c"
)

set(INCLUDE_DIRS "include")

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    PRIV_REQUIRES esp_timer esp_psram driver esp_event esp_wifi nvs_flash esp_netif lwip bt
)
