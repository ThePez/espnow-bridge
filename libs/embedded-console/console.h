/**
 ******************************************************************************
 * @file    console.h
 * @author  Jack Cairns
 * @brief   Serial console shared by the ESP32 and STM32 builds. Line
 *          editing, history and Tab completion over a UART, with printf
 *          variants that keep the line being edited intact.
 ******************************************************************************
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdarg.h>

/**
 * @brief Starts the console: brings up the UART receive path and the console task.
 *
 * The task reads bytes from the UART and feeds them through the line editor
 * (arrow keys, Home/End, Delete, Backspace). Completed lines are run as commands.
 */
void console_start(void);

/**
 * @brief vprintf() that keeps the line being edited intact.
 *
 * Wipes the prompt, prints the message, then redraws the prompt and partial input.
 * Installed as the ESP-IDF log sink by console_start(); on other targets call it from
 * your own logging code. The message must end in a newline.
 */
int console_vprintf(const char* fmt, va_list ap);

/**
 * @brief printf() that keeps the line being edited intact (see console_vprintf).
 */
int console_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
