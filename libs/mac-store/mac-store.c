#include "mac-store.h"

#include <stdbool.h>
#include <string.h>

#include "device-config.h"

#define NAMESPACE   "mac_store"
#define CFG_VERSION 2

// Slots in each list. Index 0 is the encrypted list, index 1 the unencrypted one.
#define LIST_ENCRYPTED 0
#define LIST_PLAIN     1
#define LIST_SLOTS \
    (MAC_STORE_MAX_ENCRYPTED > MAC_STORE_MAX_PLAIN ? MAC_STORE_MAX_ENCRYPTED : MAC_STORE_MAX_PLAIN)

static const uint8_t LIST_MAX[2] = {MAC_STORE_MAX_ENCRYPTED, MAC_STORE_MAX_PLAIN};

// Stored as a single blob. `version` must stay first (see device-config.h); bump CFG_VERSION on
// any layout change.
typedef struct {
    uint16_t version;
    uint8_t count[2];
    uint8_t macs[2][LIST_SLOTS][6];
} MacStoreConfig;

static const MacStoreConfig defaults = {.version = CFG_VERSION};
static MacStoreConfig cfg = {.version = CFG_VERSION};
static bool ready = false;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static inline int list_of(bool encrypted)
{
    return encrypted ? LIST_ENCRYPTED : LIST_PLAIN;
}

// Looks for mac in both lists. Returns true and sets *list and *index if found.
// Caller holds cfgMutex.
static bool find(const uint8_t mac[6], int *list, int *index)
{
    for (int l = 0; l < 2; l++) {
        for (int i = 0; i < cfg.count[l]; i++) {
            if (memcmp(cfg.macs[l][i], mac, 6) == 0) {
                *list = l;
                *index = i;
                return true;
            }
        }
    }

    return false;
}

// close the gap left by an entry, then wipe the freed last slot
static void remove_at(int list, int index)
{
    memmove(cfg.macs[list][index], cfg.macs[list][index + 1],
            (size_t)(cfg.count[list] - index - 1) * 6);
    memset(cfg.macs[list][--cfg.count[list]], 0, 6);
}

// Writes the lists to flash. If that fails, restores `backup` so RAM matches flash.
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
    if (err != ESP_OK || cfg.count[LIST_ENCRYPTED] > LIST_MAX[LIST_ENCRYPTED] ||
        cfg.count[LIST_PLAIN] > LIST_MAX[LIST_PLAIN]) {
        // First boot or a layout change (the library reports that as an error): it has
        // written the defaults to flash, but `cfg` may still hold a stale blob, so start
        // from the defaults here too. If the write itself failed the lists still work in
        // RAM and mac_store_add() will report the failed save.
        cfg = defaults;
    }

    ready = true;
    return ESP_OK;
}

size_t mac_store_count(bool encrypted)
{
    return ready ? cfg.count[list_of(encrypted)] : 0;
}

esp_err_t mac_store_get(bool encrypted, size_t index, uint8_t mac[6])
{
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    int list = list_of(encrypted);
    if (index >= cfg.count[list]) {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(mac, cfg.macs[list][index], 6);
    return ESP_OK;
}

esp_err_t mac_store_add(const uint8_t mac[6], bool encrypted)
{
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(cfgMutex, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    int want = list_of(encrypted);
    int list, index;
    bool found = find(mac, &list, &index);

    if (!(found && list == want)) {
        if (cfg.count[want] >= LIST_MAX[want]) {
            err = ESP_ERR_NO_MEM;
        } else {
            MacStoreConfig backup = cfg;
            if (found) {
                remove_at(list, index); // it is changing from one list to the other
            }

            memcpy(cfg.macs[want][cfg.count[want]++], mac, 6);
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
    int list, index;
    if (find(mac, &list, &index)) {
        MacStoreConfig backup = cfg;
        remove_at(list, index);
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
