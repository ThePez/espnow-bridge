/**
 ******************************************************************************
 * @file    device-config.h
 * @author  Jack Cairns
 * @brief   NVS flash init and versioned config blobs. A config struct is
 *          stored as one blob and reseeded from compiled defaults if the
 *          stored version does not match. cfgMutex protects the config
 *          data.
 ******************************************************************************
 */

#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

/**
 * @brief Device configuration blobs persisted in NVS.
 *
 * A config struct is written and read as a single blob under one
 * namespace/key (see device_config_load) rather than as many individual
 * keys, so a save is one commit instead of several. Each struct must start
 * with a uint16_t `version` field. Bump it on any layout change -
 * device_config_load's version check will then fail against a blob written
 * by the old layout, letting it reseed from compiled defaults instead of
 * reading a stale struct.
 */

// Protects the config data during operations
extern SemaphoreHandle_t cfgMutex;

/**
 * @brief Initializes the NVS flash partition.
 *
 * Erases and reinitializes the partition if no free pages are available or
 * a newer NVS version is found. Safe to call more than once - subsequent
 * calls are a no-op once initialization has succeeded.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t nvs_init(void);

/**
 * @brief Loads a versioned config blob from NVS into `cfg`, or seeds it
 * from `defaults` (and writes that back) if nothing valid is there yet.
 *
 * Falls back to defaults if the read fails for any reason (not found, size
 * mismatch from a layout change) or if the stored blob's version field
 * doesn't match `version`. Creates cfgMutex on this call. Only the first
 * call actually loads anything - subsequent calls are a no-op that just
 * return ESP_OK.
 *
 * @param namespace NVS namespace to open.
 * @param cfg       Destination buffer; must start with a uint16_t version
 *                  field and be at least `len` bytes.
 * @param len       Size of the config blob in bytes.
 * @param version   Expected value of cfg's leading version field.
 * @param defaults  Compiled-default blob to fall back to, `len` bytes.
 *
 * @return ESP_OK once `cfg` holds a valid config, either way.
 */
esp_err_t device_config_load(const char* namespace, void* cfg, size_t len, uint16_t version,
                             const void* defaults);

/**
 * @brief Updates the given config blob into NVS
 *
 * @param namespace NVS namespace to write to - the same one passed to
 *                  device_config_load() for this blob.
 * @param new_data  Complete replacement blob, `len` bytes.
 * @param len       Size of the config blob in bytes.
 *
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_INITIALIZED if
 *         device_config_load() hasn't been called yet, or an error code
 *         from the flash write.
 */
esp_err_t device_config_save(const char* namespace, const void* new_data, size_t len);

#endif
