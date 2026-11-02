#ifndef PACKET_SENDER_H
#define PACKET_SENDER_H

/**
 * @brief Starts the task that cycles through the test packets in packet_sender.c.
 *
 * One packet is sent every SEND_PERIOD_MS to every registered ESP-NOW peer (the MACs added
 * with "listen" or restored from flash). Needs esp_now_module_init() and console_start()
 * to have run first.
 */
void packet_sender_start(void);

#endif
