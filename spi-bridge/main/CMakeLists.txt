idf_component_register(SRCS "spi-bridge.c" "packet_sender.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_wifi esp_rom
                    )
