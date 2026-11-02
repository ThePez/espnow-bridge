/**
 ******************************************************************************
 * @file    mac-store.h
 * @author  Jack Cairns
 * @brief   Two lists of peer MAC addresses (encrypted and unencrypted)
 *          persisted in NVS so they survive a reboot. A MAC is in at most
 *          one list.
 ******************************************************************************
 */

#ifndef MAC_STORE_H
#define MAC_STORE_H

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Two lists of peer MACs, kept in NVS so they survive a reboot:
 *   encrypted   - boards that share the keys (the base station, the other bridge)
 *   unencrypted - boards that can't be set up with ours, e.g. the machines the students control
 * A MAC is in at most one list.
 */
#define MAC_STORE_MAX_ENCRYPTED 5
#define MAC_STORE_MAX_PLAIN     5

/**
 * @brief Loads the stored peer MACs from NVS (seeding empty lists on first boot).
 *
 * Calls nvs_init() itself, so it can run before or after esp_now_module_init().
 */
esp_err_t mac_store_init(void);

/** @brief Number of MACs in the encrypted (true) or unencrypted (false) list. */
size_t mac_store_count(bool encrypted);

/** @brief Copies the MAC at `index` (0 .. mac_store_count() - 1) of a list into `mac`. */
esp_err_t mac_store_get(bool encrypted, size_t index, uint8_t mac[6]);

/**
 * @brief Stores a MAC in the encrypted or unencrypted list and saves.
 *
 * Adding one that is already in that list is a success. One that is in the other list is moved.
 *
 * @return ESP_OK, ESP_ERR_NO_MEM if that list is already full, or a flash write error.
 */
esp_err_t mac_store_add(const uint8_t mac[6], bool encrypted);

/** @brief Removes a MAC from whichever list it is in and saves. ESP_ERR_NOT_FOUND if neither. */
esp_err_t mac_store_remove(const uint8_t mac[6]);

/** @brief Empties both lists and saves. */
esp_err_t mac_store_clear(void);

#endif
