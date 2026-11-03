/**
 ******************************************************************************
 * @file    master-link.h
 * @author  Jack Cairns
 * @brief   SPI master side of the link to the ESP32-S3, for the STM32F4
 *          HAL. Software chip select and a READY input from the ESP. The
 *          SPI handle, pins and timeout can be overridden in platform-
 *          conf.h. Frame format is in spi-proto.h.
 ******************************************************************************
 */

#ifndef MASTER_LINK_H
#define MASTER_LINK_H

#ifdef HAVE_PLATFORMCONF_H
#include "platform-conf.h"
#endif

#include <stdint.h>

#include "spi-proto.h"
#include "stm32f4xx_hal.h"

/* The CubeMX SPI handle, if not specified by a platform-conf.h file */
#ifndef ESP_LINK_HSPI
#define ESP_LINK_HSPI hspi2
#endif

/* Chip select (software controlled) */
#ifndef ESP_LINK_CS_PORT
#define ESP_LINK_CS_PORT GPIOB
#endif

#ifndef ESP_LINK_CS_PIN
#define ESP_LINK_CS_PIN GPIO_PIN_12
#endif

/* READY input from the ESP */
#ifndef ESP_LINK_READY_PORT
#define ESP_LINK_READY_PORT GPIOF
#endif

#ifndef ESP_LINK_READY_PIN
#define ESP_LINK_READY_PIN GPIO_PIN_14
#endif

/* How long to wait for READY and for each SPI transfer */
#ifndef ESP_LINK_TIMEOUT_MS
#define ESP_LINK_TIMEOUT_MS 50
#endif

/* Return codes. HAL_OK, HAL_TIMEOUT and HAL_ERROR (transfer or frame problem) are returned as
 * they are. When the ESP answers with a non-OK status, the call returns ESP_LINK_RC_BASE plus that
 * ESP_LINK_STATUS_* code, which keeps it clear of the HAL values. */
enum {
    ESP_LINK_RC_BASE = 0x10,
    ESP_LINK_RC_FAILED = ESP_LINK_RC_BASE + ESP_LINK_STATUS_FAILED,
    ESP_LINK_RC_BUSY = ESP_LINK_RC_BASE + ESP_LINK_STATUS_BUSY,
    ESP_LINK_RC_NO_PEERS = ESP_LINK_RC_BASE + ESP_LINK_STATUS_NO_PEERS,
};

/* Set/Reset CS pin */
#define ESP_LINK_CS_HIGH() HAL_GPIO_WritePin(ESP_LINK_CS_PORT, ESP_LINK_CS_PIN, GPIO_PIN_SET)
#define ESP_LINK_CS_LOW()  HAL_GPIO_WritePin(ESP_LINK_CS_PORT, ESP_LINK_CS_PIN, GPIO_PIN_RESET)

/* READY is high when the ESP has a reply loaded */
#define ESP_LINK_READY() (HAL_GPIO_ReadPin(ESP_LINK_READY_PORT, ESP_LINK_READY_PIN) == GPIO_PIN_SET)

/**
 * @brief  Put the link in its idle state (CS high, sequence number zero). Call once, after
 *         the CubeMX peripherals are initialised.
 * @param  None
 * @retval None
 */
void esp_link_init(void);

/**
 * @brief  Send one packet. The ESP sends it out over ESP-NOW to every device in its peer list.
 *         A request, so two SPI transactions: the packet, then one that reads back the ESP's
 *         result for it. HAL_OK means the ESP accepted the packet and queued it for ESP-NOW. It
 *         does not mean the other end received it.
 * @param  tx_buf Payload to send, ESP_LINK_PAYLOAD_LEN (32) bytes.
 * @retval HAL_OK      The ESP queued the packet for ESP-NOW.
 * @retval HAL_TIMEOUT The ESP never raised READY, packet not sent.
 * @retval HAL_ERROR   The transfer or the reply frame was bad.
 * @retval ESP_LINK_RC_BUSY     The ESP-NOW queue is full, try again shortly.
 * @retval ESP_LINK_RC_NO_PEERS The ESP has no peers to send to, retrying will not help.
 * @retval ESP_LINK_RC_FAILED   The ESP tried and could not send it.
 */
uint8_t esp_link_send_packet(uint8_t *tx_buf);

/**
 * @brief  Ask the ESP to do something and read back its answer (two SPI transactions). The
 *         ESP_LINK_CMD_* codes in spi-proto.h say what can be asked, e.g. ADD_PEER with
 *         the MAC as the argument, or GET_MAC with no argument and 6 bytes back.
 * @param  cmd      An ESP_LINK_CMD_* code.
 * @param  arg      Argument bytes, or NULL if arg_len is 0.
 * @param  arg_len  0 to ESP_LINK_PAYLOAD_LEN.
 * @param  resp     Buffer for the answer, ESP_LINK_PAYLOAD_LEN bytes. May be NULL if the
 *                  command has no answer.
 * @param  resp_len Number of answer bytes written to resp. May be NULL.
 * @retval HAL_OK      The ESP did it.
 * @retval HAL_TIMEOUT The ESP never raised READY.
 * @retval HAL_ERROR   The transfer failed or the reply frame was bad.
 * @retval ESP_LINK_RC_BASE + n  The ESP reported ESP_LINK_STATUS_n (see ESP_LINK_RC_*).
 */
uint8_t esp_link_request(uint8_t cmd, const uint8_t *arg, uint8_t arg_len, uint8_t *resp,
                         uint8_t *resp_len);

#endif
