/**
 ******************************************************************************
 * @file    master-link.c
 * @author  Jack Cairns
 * @brief   Implementation of the SPI master link in master-link.h. Waits
 *          for READY, clocks one frame, checks the reply, and builds the
 *          packet and request calls on that. A request is two
 *          transactions: the request, then one that reads the reply.
 ******************************************************************************
 */

#include "master-link.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "spi-proto.h"
#include "stm32f4xx_hal_def.h"
#include "task.h"

extern SPI_HandleTypeDef ESP_LINK_HSPI;

static uint8_t seq = 0; // sequence number of the last request sent

/* ------------------------------------------------------------------------- */
/* One SPI transaction                                                       */
/* ------------------------------------------------------------------------- */

static uint8_t transfer(const EspLinkFrame *tx, EspLinkFrame *rx)
{
    uint32_t start = HAL_GetTick();
    while (!ESP_LINK_READY()) {
        if (HAL_GetTick() - start >= ESP_LINK_TIMEOUT_MS) {
            return HAL_TIMEOUT;
        }

        vTaskDelay(1);
    }

    ESP_LINK_CS_LOW();
    HAL_StatusTypeDef rc = HAL_SPI_TransmitReceive(&ESP_LINK_HSPI, (uint8_t *)tx, (uint8_t *)rx,
                                                   ESP_LINK_FRAME_LEN, ESP_LINK_TIMEOUT_MS);
    ESP_LINK_CS_HIGH();

    start = HAL_GetTick();
    while (ESP_LINK_READY()) {
        if (HAL_GetTick() - start >= ESP_LINK_TIMEOUT_MS) {
            return rc == HAL_OK ? HAL_TIMEOUT : (uint8_t)rc;
        }
    }

    return (uint8_t)rc;
}

/* ------------------------------------------------------------------------- */
/* Frames                                                                    */
/* ------------------------------------------------------------------------- */

static uint8_t exchange(uint8_t cmd, const uint8_t *data, uint8_t len, EspLinkFrame *reply)
{
    EspLinkFrame request = {
        .start = ESP_LINK_START_MASTER, // Master
        .seq = ++seq,                   // Sequence number
        .code = cmd,                    // command
        .len = len                      // payload length
    };

    if (len > 0) {
        memcpy(request.payload, data, len);
    }

    // Fill the CRC16 value
    esp_link_frame_seal(&request);

    // SPI transmit
    uint8_t rc = transfer(&request, reply);
    if (rc != HAL_OK) {
        return rc;
    }

    // Convert Error codes
    return esp_link_frame_check(reply, ESP_LINK_START_SLAVE) == ESP_LINK_STATUS_OK ? HAL_OK
                                                                                   : HAL_ERROR;
}

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

void esp_link_init(void)
{
    ESP_LINK_CS_HIGH();
    seq = 0;
}

uint8_t esp_link_send_packet(uint8_t *tx_buf)
{
    return esp_link_request(ESP_LINK_CMD_SEND_PACKET, tx_buf, ESP_LINK_PAYLOAD_LEN, NULL, NULL);
}

uint8_t esp_link_request(uint8_t cmd, const uint8_t *arg, uint8_t arg_len, uint8_t *resp,
                         uint8_t *resp_len)
{
    if (arg_len > ESP_LINK_PAYLOAD_LEN) {
        return HAL_ERROR;
    }

    EspLinkFrame reply;

    // Transaction 1: the request. The reply to it is only for the request before it.
    uint8_t rc = exchange(cmd, arg, arg_len, &reply);
    if (rc != HAL_OK) {
        return rc;
    }
    uint8_t requestSeq = seq;

    // Transaction 2: nothing new to ask, this one just clocks the answer out.
    rc = exchange(ESP_LINK_CMD_NOP, NULL, 0, &reply);
    if (rc != HAL_OK) {
        return rc;
    }

    if (reply.seq != requestSeq) {
        return HAL_ERROR;
    }

    // The ESP understood the frame but the request failed, pass its reason up
    if (reply.code != ESP_LINK_STATUS_OK) {
        return (uint8_t)(ESP_LINK_RC_BASE + reply.code);
    }

    if (resp != NULL) {
        memcpy(resp, reply.payload, reply.len);
    }

    if (resp_len != NULL) {
        *resp_len = reply.len;
    }

    return HAL_OK;
}
