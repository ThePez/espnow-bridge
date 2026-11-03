/**
 ******************************************************************************
 * @file    slave-link.c
 * @author  Jack Cairns
 * @brief   Implementation of the SPI slave link in slave-link.h. Receives
 *          one frame per transaction, checks it, handles the request
 *          (send a packet over ESP-NOW, peer add/delete/clear, get
 *          MAC/peers) and loads the reply for the next transaction.
 ******************************************************************************
 */

#include "esp_err.h"
#include "slave-link.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "spi-proto.h"
#include "espnow-comm.h"
#include "mac-store.h"

_Static_assert(MAC_STORE_MAX_ENCRYPTED <= ESP_LINK_PEERS_PER_REPLY &&
                   MAC_STORE_MAX_PLAIN <= ESP_LINK_PEERS_PER_REPLY,
               "a GET_PEERS reply must hold a whole list");

#define TAG "SPI_LINK"

typedef struct {
    uint8_t status;
    uint8_t len;
    uint8_t data[ESP_LINK_PAYLOAD_LEN];
} Result;

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

#ifndef SPI_LINK_HOST
#define SPI_LINK_HOST SPI2_HOST
#endif

// Defaults are the IO_MUX pins of SPI2 on the ESP32-S3, which allow the highest clock
#ifndef SPI_LINK_PIN_MOSI
#define SPI_LINK_PIN_MOSI 11
#endif
#ifndef SPI_LINK_PIN_MISO
#define SPI_LINK_PIN_MISO 13
#endif
#ifndef SPI_LINK_PIN_SCLK
#define SPI_LINK_PIN_SCLK 12
#endif
#ifndef SPI_LINK_PIN_CS
#define SPI_LINK_PIN_CS 10
#endif

// Output to the STM32: high while a reply is loaded and the master may start a transaction
#ifndef SPI_LINK_PIN_READY
#define SPI_LINK_PIN_READY 9
#endif

// Frame buffers
static EspLinkFrame rxFrame;
static EspLinkFrame txFrame;

/* ------------------------------------------------------------------------- */
/* Request handlers                                                          */
/* ------------------------------------------------------------------------- */

static inline uint8_t send_status_of(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return ESP_LINK_STATUS_OK;
    case ESP_ERR_ESPNOW_NO_MEM:
        return ESP_LINK_STATUS_BUSY;
    case ESP_ERR_ESPNOW_NOT_FOUND:
        return ESP_LINK_STATUS_NO_PEERS;
    default:
        return ESP_LINK_STATUS_FAILED;
    }
}

static inline uint8_t status_of(esp_err_t err)
{
    return (err == ESP_OK ? ESP_LINK_STATUS_OK : ESP_LINK_STATUS_FAILED);
}

static Result handle_request(const EspLinkFrame *frame)
{
    Result result = {.status = ESP_LINK_STATUS_OK};
    struct wifi_packet_t pkt = {.packet_id = RAW};
    esp_err_t err = ESP_OK;

    switch (frame->code) {
    case ESP_LINK_CMD_NOP:
        break;

    case ESP_LINK_CMD_SEND_PACKET:
        if (frame->len != ESP_LINK_PAYLOAD_LEN) {
            result.status = ESP_LINK_STATUS_BAD_FRAME;
            break;
        }

        memcpy(pkt.data.raw, frame->payload, ESP_LINK_PAYLOAD_LEN);
        result.status = send_status_of(esp_now_send_packet(NULL, &pkt));
        break;

    case ESP_LINK_CMD_PEER_ADD: {
        // payload: the MAC, then the peer type
        if (frame->len != 7 || frame->payload[6] > ESP_LINK_PEER_UNENCRYPTED) {
            result.status = ESP_LINK_STATUS_BAD_FRAME;
            break;
        }

        bool encrypted = frame->payload[6] == ESP_LINK_PEER_ENCRYPTED;
        err = esp_now_module_add_peer(frame->payload, encrypted);
        if (err == ESP_OK) {
            // remember it across reboots
            err = mac_store_add(frame->payload, encrypted);
        }

        result.status = status_of(err);
        break;
    }

    case ESP_LINK_CMD_PEER_DEL: {
        if (frame->len != 6) {
            result.status = ESP_LINK_STATUS_BAD_FRAME;
            break;
        }

        esp_err_t stored = mac_store_remove(frame->payload);
        esp_err_t live = esp_now_del_peer(frame->payload);
        // Not being there already is fine, the peer is gone either way
        bool ok = (stored == ESP_OK || stored == ESP_ERR_NOT_FOUND) &&
                  (live == ESP_OK || live == ESP_ERR_ESPNOW_NOT_FOUND);
        result.status = ok ? ESP_LINK_STATUS_OK : ESP_LINK_STATUS_FAILED;
        break;
    }

    case ESP_LINK_CMD_PEER_CLEAR: {
        for (int type = 0; type < 2; type++) {
            bool encrypted = type == 0;
            for (size_t i = 0; i < mac_store_count(encrypted); i++) {
                uint8_t mac[6];
                if (mac_store_get(encrypted, i, mac) == ESP_OK) {
                    esp_now_del_peer(mac);
                }
            }
        }

        result.status = status_of(mac_store_clear());
        break;
    }

    case ESP_LINK_CMD_GET_MAC:
        result.status = status_of(esp_wifi_get_mac(WIFI_IF_STA, result.data));
        result.len = 6;
        break;

    case ESP_LINK_CMD_GET_PEERS: {
        // The argument is which list is wanted. Each list fits in one reply.
        bool encrypted = !(frame->len >= 1 && frame->payload[0] == ESP_LINK_PEER_UNENCRYPTED);
        size_t n = mac_store_count(encrypted);
        result.data[0] = (uint8_t)n;
        for (size_t i = 0; i < n; i++) {
            mac_store_get(encrypted, i, &result.data[1 + 6 * i]);
        }

        result.len = (uint8_t)(1 + 6 * n);
        break;
    }

    default:
        result.status = ESP_LINK_STATUS_UNKNOWN_CMD;
        break;
    }

    return result;
}

/* ------------------------------------------------------------------------- */
/* SPI slave ISRs                                                            */
/* ------------------------------------------------------------------------- */

static void IRAM_ATTR spi_slave_setup_cb(spi_slave_transaction_t *t)
{
    (void)t;
    gpio_set_level(SPI_LINK_PIN_READY, 1);
}

static void IRAM_ATTR spi_slave_trans_cb(spi_slave_transaction_t *t)
{
    (void)t;
    gpio_set_level(SPI_LINK_PIN_READY, 0);
}

/* ------------------------------------------------------------------------- */
/* SPI slave                                                                 */
/* ------------------------------------------------------------------------- */

static void fill_reply(EspLinkFrame *tx, uint8_t seq, const Result *r)
{
    memset(tx, 0, sizeof(*tx));
    tx->start = ESP_LINK_START_SLAVE;
    tx->seq = seq;
    tx->code = r->status;
    tx->len = r->len;
    memcpy(tx->payload, r->data, r->len);
    esp_link_frame_seal(tx);
}

static void link_task(void *arg)
{
    (void)arg;

    EspLinkFrame *rx = &rxFrame;
    EspLinkFrame *tx = &txFrame;

    // The first transaction has nothing to answer yet
    Result none = {.status = ESP_LINK_STATUS_OK};
    fill_reply(tx, 0, &none);

    for (;;) {
        memset(rx, 0, sizeof(*rx));
        spi_slave_transaction_t t = {
            .length = ESP_LINK_FRAME_LEN * 8, // in bits
            .tx_buffer = tx,
            .rx_buffer = rx,
        };

        // Blocks until the master has clocked a whole frame
        esp_err_t err = spi_slave_transmit(SPI_LINK_HOST, &t, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "transaction failed (%s)", esp_err_to_name(err));
            continue;
        }

        // Handle the request
        uint8_t status = esp_link_frame_check(rx, ESP_LINK_START_MASTER);
        Result result = {.status = status};
        if (result.status == ESP_LINK_STATUS_OK) {
            result = handle_request(rx);
        }

        // Build reply
        fill_reply(tx, rx->seq, &result);
    }
}

esp_err_t spi_link_start(void)
{
    // READY: output, low until the first reply is loaded
    gpio_config_t ready = {
        .pin_bit_mask = 1ULL << SPI_LINK_PIN_READY,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&ready));
    gpio_set_level(SPI_LINK_PIN_READY, 0);

    spi_bus_config_t bus = {
        .mosi_io_num = SPI_LINK_PIN_MOSI,
        .miso_io_num = SPI_LINK_PIN_MISO,
        .sclk_io_num = SPI_LINK_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    spi_slave_interface_config_t slave = {
        .mode = 0, // CPOL 0, CPHA 0: set the same on the STM32
        .spics_io_num = SPI_LINK_PIN_CS,
        .queue_size = 1,
        .flags = 0,
        .post_setup_cb = spi_slave_setup_cb,
        .post_trans_cb = spi_slave_trans_cb,
    };

    gpio_set_pull_mode(SPI_LINK_PIN_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SPI_LINK_PIN_SCLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SPI_LINK_PIN_CS, GPIO_PULLUP_ONLY);

    // The FIFO holds 64 bytes, so a frame must not be longer than that
    _Static_assert(ESP_LINK_FRAME_LEN <= 64, "frame too long for SPI without DMA");

    esp_err_t err = spi_slave_initialize(SPI_LINK_HOST, &bus, &slave, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_initialize failed (%s)", esp_err_to_name(err));
        return err;
    }

    xTaskCreate(link_task, "spi_link", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
    return ESP_OK;
}
