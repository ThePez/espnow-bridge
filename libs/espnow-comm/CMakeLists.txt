idf_component_register(SRCS "espnow-comm.c"
                    PRIV_REQUIRES esp_event esp_netif esp_wifi device-config common-functions
                    INCLUDE_DIRS ".")