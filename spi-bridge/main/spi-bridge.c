#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "console.h"
#include "espnow-comm.h"
#include "packet_sender.h"
#include "slave-link.h"
#include "mac_store.h"

void app_main(void)
{
    // Brings up NVS, WiFi and ESP-NOW (no peers yet - use the "peer_add" command)
    ESP_ERROR_CHECK(esp_now_module_init(NULL, 0));

    // Re-add the peers saved by earlier "peer_add" commands
    ESP_ERROR_CHECK(mac_store_init());
    for (size_t i = 0; i < mac_store_count(); i++) {
        uint8_t mac[6];
        if (mac_store_get(i, mac) != ESP_OK) {
            continue;
        }

        esp_err_t err = esp_now_module_add_peer(mac);
        if (err != ESP_OK) {
            ESP_LOGW("main", "Could not re-add stored peer %02x:%02x:%02x:%02x:%02x:%02x (%s)",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], esp_err_to_name(err));
        }
    }

    // The console must be up before the sender, because the sender prints through it
    console_start();

    // The STM32 talks to this board over SPI (see libs/spi_link)
    ESP_ERROR_CHECK(spi_link_start());

    // Test packets, remove this once the STM32 is the one sending
    // packet_sender_start();

    // End the main task here.
    vTaskDelete(NULL);
}
