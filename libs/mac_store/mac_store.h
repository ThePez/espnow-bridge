#ifndef MAC_STORE_H
#define MAC_STORE_H

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

#define MAC_STORE_MAX 10

/**
 * @brief Loads the stored peer MACs from NVS (seeding an empty list on first boot).
 *
 * Calls nvs_init() itself, so it can run before or after esp_now_module_init().
 */
esp_err_t mac_store_init(void);

/** @brief Number of stored MACs. */
size_t mac_store_count(void);

/** @brief Copies the MAC at `index` (0 .. mac_store_count() - 1) into `mac`. */
esp_err_t mac_store_get(size_t index, uint8_t mac[6]);

/**
 * @brief Stores a MAC and saves the list. Storing one that is already there is a success.
 *
 * @return ESP_OK, ESP_ERR_NO_MEM if MAC_STORE_MAX are already stored, or a flash write error.
 */
esp_err_t mac_store_add(const uint8_t mac[6]);

/** @brief Removes a MAC and saves the list. ESP_ERR_NOT_FOUND if it wasn't stored. */
esp_err_t mac_store_remove(const uint8_t mac[6]);

/** @brief Removes every stored MAC and saves the empty list. */
esp_err_t mac_store_clear(void);

#endif
