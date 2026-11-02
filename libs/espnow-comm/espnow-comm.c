/**
 ******************************************************************************
 * @file    espnow-comm.c
 * @author  Jack Cairns
 * @brief   Implementation of the ESP-NOW wrapper in espnow-comm.h:
 *          WiFi/ESP-NOW init, the send and receive callbacks, peer add
 *          and channel set.
 ******************************************************************************
 */

#include "espnow-comm.h"

#include "common-functions.h"
#include "device-config.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_now.h>
#include <esp_rom_crc.h>
#include <esp_wifi.h>
#include <string.h>

#define TAG "ESPNOW"

// Encryption keys
// PMK (Primary Master Key) - must be same on both devices
const uint8_t pmk_key[16] = {0x9F, 0x3C, 0x8A, 0x2D, 0x7B, 0x14,
                             0xE6, 0xF0, 0xA5, 0xC9, 0xD2, 0xB1};
// LMK (Local Master Key) - must be same for this peer relationship
const uint8_t lmk_key[16] = {0x4A, 0x7E, 0x1D, 0x9C, 0x0F, 0x6B,
                             0x2A, 0x8E, 0x5D, 0x3C, 0x91, 0xB4};

// Channel all peers communicate on (1-13). Peers are registered with channel 0 = "current channel".
static uint8_t espnow_channel = 9;

// Queue for recieving packets
QueueHandle_t wifiQueue = NULL;
SemaphoreHandle_t wifiSendSemaphore = NULL;

// ESP-NOW send-complete callback.
static void espnow_send_cb(const uint8_t* mac_addr, esp_now_send_status_t status) {
    ARG_UNUSED(mac_addr);
    xSemaphoreGive(wifiSendSemaphore);
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "MESSAGE NOT DELIVERED");
    }
}

// ESP-NOW receive callback.
static void espnow_recv_cb(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len) {

    ARG_UNUSED(recv_info);
    ARG_UNUSED(len);

    struct wifi_packet_t* packet = (struct wifi_packet_t*) data;

    // Check CRC16 value
    if (packet->crc16 != esp_rom_crc16_le(0, (uint8_t*) &packet->data, sizeof(union packet_data))) {
        // Invalid Packet
        ESP_LOGE(TAG, "Invalid wifi packet recieved");
        return;
    }

    // No waiting
    if (xQueueSendToBack(wifiQueue, packet, 0) != pdTRUE) {
        ESP_LOGI(TAG, "Packet recieved but not posted");
    }
}

// Brings up WiFi in station mode on a fixed channel with power-save disabled.
static esp_err_t wifi_init(void) {
    CHECK_ERR(esp_netif_init(), "netif init failed");
    CHECK_ERR(esp_event_loop_create_default(), "event loop failed");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    CHECK_ERR(esp_wifi_init(&cfg), "wifi init failed");
    CHECK_ERR(esp_wifi_set_storage(WIFI_STORAGE_RAM), "storage init failed");
    CHECK_ERR(esp_wifi_set_mode(WIFI_MODE_STA), "mode set failed");

    CHECK_ERR(esp_wifi_start(), "wifi not started");

    // Disable modem sleep - power save delays/drops ESP-NOW RX between DTIM wakeups
    CHECK_ERR(esp_wifi_set_ps(WIFI_PS_NONE), "Wifi power save disable failed");

    CHECK_ERR(esp_wifi_set_channel(espnow_channel, WIFI_SECOND_CHAN_NONE), "Wifi channel not set");

    ESP_LOGI(TAG, "Wifi initialised");

    return ESP_OK;
}

// Initializes the ESP-NOW driver.
static esp_err_t espnow_init(const uint8_t* peer_addr[], uint8_t num_peers) {
    // Initialize ESP-NOW
    CHECK_ERR(esp_now_init(), "init failed");

    // Set PMK after initialization
    CHECK_ERR(esp_now_set_pmk(pmk_key), "PMK failed");

    // Register the callback functions
    CHECK_ERR(esp_now_register_send_cb(espnow_send_cb), "send cb register failed");
    CHECK_ERR(esp_now_register_recv_cb(espnow_recv_cb), "recieve cb register failed");

    // Add all peers
    for (uint8_t i = 0; i < num_peers; i++) {
        CHECK_ERR(esp_now_module_add_peer(peer_addr[i], true), "peer add failed");
    }

    ESP_LOGI(TAG, "ESP-NOW initialized with encryption, %u peer(s) added", num_peers);
    return ESP_OK;
}

esp_err_t esp_now_module_init(const uint8_t* peer_addr[], uint8_t num_peers) {

    // Ensure NVS is initialised
    CHECK_ERR_NO_LOG(nvs_init());

    // Initialise WiFi
    CHECK_ERR_NO_LOG(wifi_init());

    // Initialise ESP-NOW
    CHECK_ERR_NO_LOG(espnow_init(peer_addr, num_peers));

    // Now initialise the messaging primatives
    do {
        // Try until it works...
        wifiQueue = xQueueCreate(10, sizeof(struct wifi_packet_t));
    } while (wifiQueue == NULL);

    do {
        // Try until it works...
        wifiSendSemaphore = xSemaphoreCreateBinary();
    } while (wifiSendSemaphore == NULL);

    xSemaphoreGive(wifiSendSemaphore);

    return ESP_OK;
}

esp_err_t esp_now_send_packet(const uint8_t* addr, struct wifi_packet_t* packet) {
    // Add the CRC before sending
    packet->crc16 = esp_rom_crc16_le(0, (uint8_t*) &packet->data, sizeof(union packet_data));
    return esp_now_send(addr, (uint8_t*) packet, sizeof(struct wifi_packet_t));
}


esp_err_t esp_now_module_add_peer(const uint8_t mac[6], bool encrypted) {
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_now_peer_info_t peer_info = {
        .channel = 0,         // 0 = use the current WiFi channel
        .ifidx = WIFI_IF_STA, // WIFI interface
        .encrypt = encrypted,
    };

    // Copy in the mac address, and the LMK if this peer is encrypted
    memcpy(peer_info.peer_addr, mac, ESP_NOW_ETH_ALEN);
    if (encrypted) {
        memcpy(peer_info.lmk, lmk_key, sizeof(lmk_key));
    }

    esp_err_t err = esp_now_add_peer(&peer_info);
    if (err == ESP_ERR_ESPNOW_EXIST) {
        // Already registered: update it, in case its encryption setting changed
        err = esp_now_mod_peer(&peer_info);
    }
    return err;
}

esp_err_t esp_now_module_set_channel(uint8_t channel) {
    if (channel < 1 || channel > 13) {
        return ESP_ERR_INVALID_ARG;
    }

    CHECK_ERR(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE), "Wifi channel not set");
    espnow_channel = channel;

    ESP_LOGI(TAG, "Channel set to %u", channel);
    return ESP_OK;
}
