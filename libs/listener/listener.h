#ifndef LISTENER_H
#define LISTENER_H

/**
 * @brief Starts the listener task, which drains wifiQueue and prints each decoded packet.
 *
 * Must be called after esp_now_module_init(), which creates the queue.
 */
void listener_start(void);

/**
 * @brief Prints the packet totals and the per-sender counts through the console.
 *
 * Safe to call from another task (the counters are only read).
 */
void listener_print_summary(void);

#endif
