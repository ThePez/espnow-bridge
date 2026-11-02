idf_component_register(SRCS "spi_link.c"
                    PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_wifi espnow-comm mac_store spi-proto
                    INCLUDE_DIRS ".")
