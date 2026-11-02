/**
 ******************************************************************************
 * @file    slave-link.h
 * @author  Jack Cairns
 * @brief   SPI slave side of the link to the STM32. Starts the SPI slave
 *          and READY pin and a task that answers the STM32's requests
 *          (see spi-proto.h).
 ******************************************************************************
 */

#ifndef SLAVE_LINK_H
#define SLAVE_LINK_H

#include <esp_err.h>

/**
 * @brief Starts the SPI slave that the STM32 talks to (see spi-proto.h).
 *
 * Sets up the SPI slave and READY pin, then runs a task that answers the STM32's requests:
 * packets are forwarded over ESP-NOW and the peer commands use the same peer list as the
 * console commands. Needs esp_now_module_init() and mac_store_init() to have run first.
 *
 * Pins and the SPI host can be overridden by defining SPI_LINK_HOST, SPI_LINK_PIN_MOSI,
 * SPI_LINK_PIN_MISO, SPI_LINK_PIN_SCLK, SPI_LINK_PIN_CS and SPI_LINK_PIN_READY.
 */
esp_err_t spi_link_start(void);

#endif
