/*
 ******************************************************************************
 * File: espnow-comm.h
 * Author: Jack Cairns
 * Date: 17-01-2026
 ******************************************************************************
 */

#ifndef ESPNOW_COMM_H
#define ESPNOW_COMM_H

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <stdbool.h>
#include <stdint.h>

union packet_data {
    uint8_t raw[32];
    // Any further types...
};

struct wifi_packet_t {
    uint8_t packet_id;
    uint16_t crc16;
    union packet_data data;
};

// Packet ids, one per payload type in packet_data.
enum esp_now_packet_id {
    RAW,
    // any further types
};

/**
 * @brief Initializes NVS, WiFi, and ESP-NOW, and registers the given peers.
 *
 * This function:
 * - Initializes NVS flash, erasing and reinitializing it if no free pages
 *   are available or a new version is found.
 * - Initializes WiFi in station mode and sets the ESP-NOW channel.
 * - Initializes the ESP-NOW driver, sets the PMK, registers the send/receive
 *   callbacks, and adds each address in peer_addr as an encrypted peer.
 * - Creates the wifiQueue (if not already created) and wifiSendSemaphore
 *   used for queuing and gating outgoing packets.
 *
 * @param peer_addr Array of MAC addresses (6 bytes each) to register as ESP-NOW peers.
 * @param num_peers Number of MAC addresses in peer_addr.
 *
 * @note Must be called once during startup before sending/receiving ESP-NOW packets.
 *
 * @global wifiQueue          Created if it does not already exist.
 * @global wifiSendSemaphore  Created and given so the first send can proceed.
 *
 * @return ESP_OK on success, or error code on failure
 */
esp_err_t esp_now_module_init(const uint8_t *peer_addr[], uint8_t num_peers);

/**
 * @brief Computes and stores the packet's CRC16, then sends it via ESP-NOW.
 *
 * Wraps esp_now_send() so callers don't need to calculate the CRC
 * themselves; the value is written into packet->crc16 over the data
 * union before transmission.
 *
 * @param addr   MAC address (6 bytes) of the peer to send to, or NULL to
 *               send to all registered peers.
 * @param packet Packet to checksum and send. Modified in place (crc16 field).
 *
 * @return ESP_OK on success, or error code from esp_now_send() on failure.
 */
esp_err_t esp_now_send_packet(const uint8_t *addr, struct wifi_packet_t *packet);

/**
 * @brief Registers a MAC address as an ESP-NOW peer, encrypted or not.
 *
 * An encrypted peer uses the module's shared LMK, and the other board must have this one
 * registered as an encrypted peer too. An unencrypted peer needs nothing from the other board,
 * which also receives unencrypted frames from senders it has never registered.
 *
 * Registering an address that is already a peer updates it, so this can also change an existing
 * peer between encrypted and unencrypted. Requires esp_now_module_init() to have run.
 *
 * @param mac       MAC address (6 bytes) of the peer to add.
 * @param encrypted true to encrypt traffic with this peer, false to send it in the clear.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if mac is NULL, or the error
 *         code from esp_now_add_peer() / esp_now_mod_peer() on failure.
 */
esp_err_t esp_now_module_add_peer(const uint8_t mac[6], bool encrypted);

/**
 * @brief Changes the WiFi channel ESP-NOW communicates on.
 *
 * Every device that should talk to this one must be on the same channel.
 * Requires esp_now_module_init() to have run.
 *
 * @param channel WiFi channel, 1-13.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if channel is out of range,
 *         or the error code from esp_wifi_set_channel() on failure.
 */
esp_err_t esp_now_module_set_channel(uint8_t channel);

// ESPNOW queue
extern QueueHandle_t wifiQueue;
extern SemaphoreHandle_t wifiSendSemaphore;

#endif
