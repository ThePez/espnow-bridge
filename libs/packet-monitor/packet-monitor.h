/**
 ******************************************************************************
 * @file    packet-monitor.h
 * @author  Jack Cairns
 * @brief   Packet monitor for the ESP32 base station: a task that drains
 *          wifiQueue and prints each decoded packet, plus a summary of
 *          the totals.
 ******************************************************************************
 */

#ifndef PACKET_MONITOR_H
#define PACKET_MONITOR_H

/**
 * @brief Starts the packet monitor task, which drains wifiQueue and prints each decoded packet.
 *
 * Must be called after esp_now_module_init(), which creates the queue.
 */
void packet_monitor_start(void);

/**
 * @brief Prints the packet totals and the per-sender counts through the console.
 *
 * Safe to call from another task (the counters are only read).
 */
void packet_monitor_print_summary(void);

#endif
