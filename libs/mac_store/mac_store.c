#include "mac_store.h"

#include <stdbool.h>
#include <string.h>

#include "device_config.h"

#define NAMESPACE   "mac_store"
#define CFG_VERSION 1

// Stored as a single blob. `version` must stay first (see device_config.h); bump CFG_VERSION on
// any layout change.
typedef struct {
    uint16_t version;
    uint8_t count;
    uint8_t macs[MAC_STORE_MAX][6];
} MacStoreConfig;

static const MacStoreConfig defaults = {.version = CFG_VERSION};
static MacStoreConfig cfg = {.version = CFG_VERSION};
static bool ready = false;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

// index of mac in the list, or -1. Caller holds cfgMutex.
static int find(const uint8_t mac[6])
{
    for (int i = 0; i < cfg.count; i++) {
        if (memcmp(cfg.macs[i], mac, 6) == 0) {
            return i;
        }
    }

    return -1;
}

// Writes the list to flash. If that fails, restores `backup` so RAM matches flash.
static esp_err_t save(const MacStoreConfig *backup)
{
    esp_err_t err = device_config_save(NAMESPACE, &cfg, sizeof(cfg));
    if (err != ESP_OK) {
        cfg = *backup;
    }

    return err;
}

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

esp_err_t mac_store_init(void)
{
    esp_err_t err = nvs_init();
    if (err != ESP_OK) {
        return err;
    }

    cfg = defaults;
    err = device_config_load(NAMESPACE, &cfg, sizeof(cfg), CFG_VERSION, &defaults);
    if (err != ESP_OK || cfg.count > MAC_STORE_MAX) {
        // First boot or a layout change (the library reports that as an error): it has
        // written the defaults to flash, but `cfg` may still hold a stale blob, so start
        // from the defaults here too. If the write itself failed the list still works in
        // RAM and mac_store_add() will report the failed save.
        cfg = defaults;
    }

    ready = true;
    return ESP_OK;
}

size_t mac_store_count(void)
{
    return ready ? cfg.count : 0;
}

esp_err_t mac_store_get(size_t index, uint8_t mac[6])
{
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (index >= cfg.count) {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(mac, cfg.macs[index], 6);
    return ESP_OK;
}

esp_err_t mac_store_add(const uint8_t mac[6])
{
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(cfgMutex, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    if (find(mac) < 0) {
        if (cfg.count >= MAC_STORE_MAX) {
            err = ESP_ERR_NO_MEM;
        } else {
            MacStoreConfig backup = cfg;
            memcpy(cfg.macs[cfg.count++], mac, 6);
            err = save(&backup);
        }
    }

    xSemaphoreGive(cfgMutex);
    return err;
}

esp_err_t mac_store_remove(const uint8_t mac[6])
{
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(cfgMutex, portMAX_DELAY);

    esp_err_t err = ESP_ERR_NOT_FOUND;
    int i = find(mac);
    if (i >= 0) {
        MacStoreConfig backup = cfg;
        // close the gap, then wipe the freed last slot
        memmove(cfg.macs[i], cfg.macs[i + 1], (size_t)(cfg.count - i - 1) * 6);
        memset(cfg.macs[--cfg.count], 0, 6);
        err = save(&backup);
    }

    xSemaphoreGive(cfgMutex);
    return err;
}

esp_err_t mac_store_clear(void)
{
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(cfgMutex, portMAX_DELAY);

    MacStoreConfig backup = cfg;
    cfg = defaults;
    esp_err_t err = save(&backup);

    xSemaphoreGive(cfgMutex);
    return err;
}
