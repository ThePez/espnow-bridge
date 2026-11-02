/*
 ******************************************************************************
 * File: device_config.c
 * Author: Jack Cairns
 * Date: 15-07-2026
 ******************************************************************************
 */

#include "device_config.h"

#include "common_functions.h"

#include <nvs_flash.h>
#include <stdbool.h>
#include <string.h>

#define TAG "CONFIG"

// device config key
static const char* const key = "cfg"; 
static bool initialised = false;
static bool cfgLoaded = false;

SemaphoreHandle_t cfgMutex = NULL;

esp_err_t nvs_init(void) {

    if (initialised) {
        return ESP_OK;
    }

    // Initialize NVS flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        CHECK_ERR_NO_LOG(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    CHECK_ERR(ret, "Flash initialise failed");

    // NVS init success
    initialised = true;
    return ESP_OK;
}

// Reads a blob value from NVS, failing if the stored size doesn't match len.
static esp_err_t blob_read(const char* namespace, const char* key, void* data, size_t len) {
    if (!initialised) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }

    nvs_handle_t handle;
    CHECK_ERR(nvs_open(namespace, NVS_READONLY, &handle), "open(%s) failed", namespace);
    size_t actual = len;
    CHECK_ERR(nvs_get_blob(handle, key, data, &actual), "read(%s/%s) failed", namespace, key);

    // Ensure length is expected
    if (actual != len) {
        ESP_LOGW(TAG, "Size mismatch: expected %d, got %d", (int) len, (int) actual);
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    return ESP_OK;
}

// Writes and commits a blob value to NVS.
static esp_err_t blob_write(const char* namespace, const char* key, const void* data, size_t len) {
    if (!initialised) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }

    nvs_handle_t handle;
    CHECK_ERR(nvs_open(namespace, NVS_READWRITE, &handle), "open(%s) failed", namespace);

    esp_err_t err = nvs_set_blob(handle, key, data, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set failed (%s/%s)", namespace, key);
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "commit failed (%s/%s)", namespace, key);
    }

    nvs_close(handle);
    return err;
}

esp_err_t device_config_load(const char* namespace, void* cfg, size_t len, uint16_t version,
                             const void* defaults) {

    if (len < sizeof(uint16_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (cfgLoaded) {
        return ESP_OK;
    }

    // Here only on first call (ie during boot process).
    cfgLoaded = true;
    cfgMutex = xSemaphoreCreateMutex();
    esp_err_t err = blob_read(namespace, key, cfg, len);

    // Config data was either found but stale or no data was found -> Store defaults
    if ((err == ESP_OK && *(uint16_t*) cfg != version) || err != ESP_OK) {
        // Write in the defaults loaded from compile-time
        CHECK_ERR(blob_write(namespace, key, defaults, len), "flash config update failed");
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }

    // Valid config data found and loaded.
    return ESP_OK;
}

esp_err_t device_config_save(const char* namespace, const void* new_data, size_t len) {
    if (!cfgMutex || !cfgLoaded) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }

    return blob_write(namespace, key, new_data, len);
}
