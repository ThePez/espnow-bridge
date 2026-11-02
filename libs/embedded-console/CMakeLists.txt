idf_component_register(SRCS "console.c" "commands.c"
                    PRIV_REQUIRES esp_driver_uart esp_wifi espnow-comm mac-store packet-monitor
                    INCLUDE_DIRS ".")
