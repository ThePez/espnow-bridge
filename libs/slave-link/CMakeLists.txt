idf_component_register(SRCS "slave-link.c"
                    PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_wifi espnow-comm mac-store spi-proto
                    INCLUDE_DIRS ".")
