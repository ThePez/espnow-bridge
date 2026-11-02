/**
 ******************************************************************************
 * @file    common-functions.h
 * @author  Jack Cairns
 * @brief   Small helper macros shared by the ESP32 libraries (header
 *          only): ARG_UNUSED and the CHECK_ERR / CHECK_ERR_NO_LOG early-
 *          return helpers for esp_err_t.
 ******************************************************************************
 */

#ifndef COMMON_H
#define COMMON_H

#include <esp_err.h>
#include <esp_log.h>

#define ARG_UNUSED(arg) ((void) arg)

// Returns the error from the calling function if `code` is not ESP_OK. CHECK_ERR logs `msg`
// first (printf style) using the TAG that the calling file defines.
#define CHECK_ERR(code, msg, ...)                                                                  \
    do {                                                                                           \
        esp_err_t _err = (code);                                                                   \
        if (_err != ESP_OK) {                                                                      \
            ESP_LOGE(TAG, msg, ##__VA_ARGS__);                                                     \
            return _err;                                                                           \
        }                                                                                          \
    } while (0)

#define CHECK_ERR_NO_LOG(code)                                                                     \
    do {                                                                                           \
        esp_err_t _err = (code);                                                                   \
        if (_err != ESP_OK) {                                                                      \
            return _err;                                                                           \
        }                                                                                          \
    } while (0)

#endif
