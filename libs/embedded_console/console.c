#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "commands.h"
#include "console.h"

#if defined(ESP_PLATFORM)

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#elif defined(USE_HAL_DRIVER)

#include "main.h" // CubeMX: pulls in the HAL and the UART_HandleTypeDef externs
#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <task.h>

#else

#error "console: unsupported platform (need ESP_PLATFORM or USE_HAL_DRIVER)"

#endif

#define PROMPT      ">> "
#define BUF_LEN     100 // num chars
#define SEQ_TIMEOUT 50  // ms
#define HISTORY     10  // num slots
#define MATCH_MAX   10  // num slots

typedef enum {
    NORMAL,
    ESCAPE,
    CSI
} State;

typedef struct {
    uint8_t pos, len;
    char buffer[BUF_LEN];
} Line;

typedef struct {
    int8_t idx;
    uint8_t count;
    char draft[BUF_LEN];
    char entries[HISTORY][BUF_LEN];
} History;

typedef struct {
    State state;
    int param;
    uint32_t started;
} Parser;

typedef struct {
    uint8_t editing;
    Line line;
    Parser parser;
    History history;
} Console;

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

// Internal Console state
static Console con = {.history.idx = -1};
static SemaphoreHandle_t lock = NULL;

static inline uint32_t millis(void)
{
    return (xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ------------------------------------------------------------------------- */
/* Line editor                                                               */
/* ------------------------------------------------------------------------- */

static void remove_at(Line *line, int idx)
{
    memmove(&line->buffer[idx], &line->buffer[idx + 1], line->len - idx - 1);
    line->len--;
}

static void store_line(History *hist, const char *input)
{
    // Find line in history
    int8_t found = -1;
    for (uint8_t i = 0; i < hist->count; i++) {
        if (!strcmp(hist->entries[i], input)) {
            found = i;
            break;
        }
    }

    // Remove the copy from the list if found
    if (found >= 0 && found < hist->count) {
        memmove(&hist->entries[found][0], &hist->entries[found + 1][0],
                BUF_LEN * (hist->count - 1 - found));
        hist->count--;
    }

    // Now store line at the head
    memmove(&hist->entries[1][0], &hist->entries[0][0], BUF_LEN * (HISTORY - 1));
    memcpy(&hist->entries[0][0], input, BUF_LEN);
    if (hist->count < HISTORY) {
        hist->count++;
    }
}

static void move_home(Line *line)
{
    if (line->pos != 0) {
        printf("\x1b[%dD", line->pos);
        fflush(stdout);
    }

    line->pos = 0;
}

static void move_end(Line *line)
{
    if (line->pos < line->len) {
        printf("\x1b[%dC", line->len - line->pos);
        fflush(stdout);
    }

    line->pos = line->len;
}

static void delete_forward(Line *line)
{
    if (line->pos < line->len) {
        remove_at(line, line->pos);
        printf("\x1b[P");
        fflush(stdout);
    }
}

static void redraw_line(Line *line)
{
    printf("\r\x1B[2K" PROMPT "%.*s", line->len, line->buffer);
    if (line->pos < line->len) {
        printf("\x1B[%dD", line->len - line->pos);
    }

    fflush(stdout);
}

static void load_line(Line *line, const char *data)
{

    memcpy(line->buffer, data, BUF_LEN);
    // Ensure its null terminated
    line->buffer[BUF_LEN - 1] = '\0';
    line->len = strlen(line->buffer);
    line->pos = line->len;
    redraw_line(line);
}

/* ------------------------------------------------------------------------- */
/* Tab completion                                                            */
/* ------------------------------------------------------------------------- */

static size_t common_prefix_len(const char **matches, size_t n)
{
    size_t common = strlen(matches[0]);
    for (size_t i = 1; i < n; i++) {
        size_t len = 0;
        while (len < common && matches[0][len] == matches[i][len]) {
            len++;
        }

        common = len;
    }

    return common;
}

static uint8_t line_insert(Line *line, const char *text, int n)
{
    if (line->len + n >= BUF_LEN) {
        return 1;
    }

    memmove(&line->buffer[line->pos + n], &line->buffer[line->pos], line->len - line->pos);
    memcpy(&line->buffer[line->pos], text, n);
    line->len += n;
    line->pos += n;
    return 0;
}

static void complete_line(Line *line)
{
    if (line->len > 0 && line->pos == 0) {
        return;
    }

    for (int i = 0; i < line->pos; i++) {
        if (line->buffer[i] == ' ') {
            // Found a space -> not in the first word
            return;
        }
    }

    // Temperarily make space for a '\0'.
    memmove(&line->buffer[line->pos + 1], &line->buffer[line->pos], line->len - line->pos);
    line->buffer[line->pos] = '\0';

    // Get list of matches
    const char *matches[MATCH_MAX];
    size_t n = commands_complete(line->buffer, matches, MATCH_MAX);

    // Now restore the buffers state
    memmove(&line->buffer[line->pos], &line->buffer[line->pos + 1], line->len - line->pos);
    size_t common;
    switch (n) {
    case 0:
        return;
    case 1:
        // One match, move anything after cursor and copy in the full match
        common = strlen(matches[0]);
        if (!line_insert(line, matches[0] + line->pos, common - line->pos)) {
            line_insert(line, " ", 1);
            redraw_line(line);
        }

        break;

    default:
        // Many matches, move to new line, print all options and then reprint line.
        common = common_prefix_len(matches, n);
        if (common > line->pos) {
            // Add missing chars
            line_insert(line, matches[0] + line->pos, common - line->pos);
        } else {
            // Print the list
            printf("\r\n");
            for (size_t i = 0; i < n; i++) {
                printf("  %s\r\n", matches[i]); // one per line, indented by two spaces
            }
        }

        redraw_line(line);
    }
}

/* ------------------------------------------------------------------------- */
/* Input parser                                                              */
/* ------------------------------------------------------------------------- */

static void csi_dispatch(Console *console, char end, int param)
{
    Line *line = &console->line;
    History *hist = &console->history;

    switch (end) {
    case 'A':
        // up
        if (hist->idx + 1 < hist->count) {
            if (hist->idx == -1) {
                // First Up press: remember what the user had typed
                line->buffer[line->len] = '\0';
                memcpy(hist->draft, line->buffer, BUF_LEN);
            }

            hist->idx++;
            load_line(line, hist->entries[hist->idx]);
        }

        break;
    case 'B':
        // down
        if (hist->idx >= 0) {
            hist->idx--;
            const char *data = hist->draft;
            if (hist->idx >= 0) {
                data = hist->entries[hist->idx];
            }

            load_line(line, data);
        }

        break;
    case 'D':
        // left
        if (line->pos > 0) {
            line->pos--;
            printf("\x1b[D");
            fflush(stdout);
        }

        break;
    case 'C':
        // right
        if (line->pos < line->len) {
            line->pos++;
            printf("\x1b[C");
            fflush(stdout);
        }

        break;
    case 'H':
        // home
        move_home(line);
        break;
    case 'F':
        // end
        move_end(line);
        break;
    case '~': // VT220 style: the number says which key
        switch (param) {
        case 1: // screen, linux console
        case 7: // rxvt
            move_home(line);
            break;
        case 4:
        case 8:
            move_end(line);
            break;
        case 3:
            delete_forward(line);
            break;
        default:
            // The rest, get ignored for now...
            break;
        }

        break;
    default:
        // the rest, get ignored for now...
        break;
    }
}

static void process(Console *console, char byte)
{
    Line *line = &console->line;
    History *hist = &console->history;
    Parser *parser = &console->parser;

    switch (parser->state) {
    case NORMAL:
        switch (byte) {
        case '\n': // NEWLINE
        case '\r':
            line->buffer[line->len] = '\0';
            printf("\r\n");
            fflush(stdout);

            if (line->len > 0) {
                store_line(hist, line->buffer);
            }

            console->editing = 0;
            commands_execute(line->buffer);
            console->editing = 1;
            printf("\r%s", PROMPT); // ensure start of line
            fflush(stdout);
            memset(line->buffer, 0, BUF_LEN);
            line->len = 0;
            line->pos = 0;
            hist->idx = -1;
            break;

        case 0x1B: // ESC
            parser->started = millis();
            parser->state = ESCAPE;
            break;

        case '\t': // TAB
            complete_line(line);
            break;

        case 0x0C: // CTRL + L -> CLEAR
            printf("\x1b[2J\x1b[H");
            redraw_line(line);
            break;

        case 0x01: // CTRL + A -> HOME
            move_home(line);
            break;

        case 0x05: // CTRL + E -> END
            move_end(line);
            break;

        case 0x02: // CTRL + B -> LEFT
            csi_dispatch(console, 'D', 0);
            break;

        case 0x06: // CTRL + F -> RIGHT
            csi_dispatch(console, 'C', 0);
            break;

        case 0x10: // CTRL + P -> UP
            csi_dispatch(console, 'A', 0);
            break;

        case 0x0E: // CTRL + N -> DOWN
            csi_dispatch(console, 'B', 0);
            break;

        case 0x04: // CTRL + D -> Delete
            delete_forward(line);
            break;

        case 0x0B: // CTRL + K -> Delete until end of line
            line->len = line->pos;
            line->buffer[line->len] = '\0';
            redraw_line(line);
            break;

        case 0x15: // CTRL + U -> Delete until start of line
            memmove(&line->buffer[0], &line->buffer[line->pos], line->len - line->pos);
            line->len -= line->pos;
            line->pos = 0;
            redraw_line(line);
            break;

        case 0x17: { // CTRL + W -> Delete previous word
            int new_pos = line->pos;
            // Loop over spaces
            while (new_pos > 0 && line->buffer[new_pos - 1] == ' ') {
                new_pos--;
            }

            // Loop over non spaces
            while (new_pos > 0 && line->buffer[new_pos - 1] != ' ') {
                new_pos--;
            }

            memmove(&line->buffer[new_pos], &line->buffer[line->pos], line->len - line->pos);
            line->len -= line->pos - new_pos;
            line->pos = new_pos;
            redraw_line(line);
            break;
        }

        case '\b': // BACKSPACE
        case 0x7F:
            if (line->pos > 0) {
                line->pos--;
                remove_at(line, line->pos);
                printf("\b\x1B[P");
                fflush(stdout);
            }

            break;

        default:
            if (byte >= 0x20 && byte < 0x7F) { // PRINTABLE
                if (line->len < BUF_LEN - 1) {
                    if (line->pos < line->len) {
                        // Shift right
                        printf("\x1B[@");
                        memmove(&line->buffer[line->pos + 1], &line->buffer[line->pos],
                                line->len - line->pos);
                    }

                    printf("%c", byte);
                    line->buffer[line->pos++] = byte;
                    line->len++;
                } else {
                    // Line is full
                    printf("\a");
                }

                fflush(stdout);
            }

            // All others dropped
        }

        break;

    case ESCAPE:
        if (millis() - parser->started > SEQ_TIMEOUT) {
            parser->state = NORMAL;
            process(console, byte);
            break;
        }

        if (byte == '[' || byte == 'O') {
            parser->started = millis();
            parser->param = 0;
            parser->state = CSI;
        } else {
            parser->state = NORMAL;
        }

        break;
    case CSI:
        if (millis() - parser->started > SEQ_TIMEOUT) {
            parser->state = NORMAL;
            process(console, byte);
            break;
        }

        if (byte >= '0' && byte <= '9') {
            parser->param = parser->param * 10 + (byte - '0');
            break;
        }

        if (byte == ';') {
            parser->param = 0;
            break;
        }

        parser->state = NORMAL;
        if (byte >= 0x40 && byte <= 0x7E) {
            csi_dispatch(console, byte, parser->param);
        }

        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Output                                                                    */
/* ------------------------------------------------------------------------- */

int console_vprintf(const char *fmt, va_list ap)
{
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);

    if (con.editing) {
        printf("\r\x1b[2K"); // wipe prompt + partial input
    }

    int n = vprintf(fmt, ap); // message must end in '\n'
    if (con.editing) {
        redraw_line(&con.line); // prompt + input come back
    }

    xSemaphoreGiveRecursive(lock);
    return n;
}

int console_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = console_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

/* ------------------------------------------------------------------------- */
/* UART + task                                                              */
/* ------------------------------------------------------------------------- */

#define RX_BUF_SIZE   256
#define TASK_PRIORITY 5

#if defined(ESP_PLATFORM)

#define CONSOLE_UART    UART_NUM_0
#define TASK_STACK_SIZE 4096 // bytes

#elif defined(USE_HAL_DRIVER)

// Which CubeMX UART carries the console (e.g. the ST-Link virtual COM port)
#define CONSOLE_HUART   huart3
#define TASK_STACK_SIZE 1024 // words (4 bytes each on Cortex-M)

extern UART_HandleTypeDef CONSOLE_HUART;

static QueueHandle_t rx_queue = NULL;
static uint8_t rx_byte; // the single byte the UART interrupt is currently filling

// Called from the UART ISR when rx_byte is full: hand it to the task and re-arm.
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart != &CONSOLE_HUART) {
        return;
    }

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(rx_queue, &rx_byte, &woken);
    HAL_UART_Receive_IT(&CONSOLE_HUART, &rx_byte, 1);
    portYIELD_FROM_ISR(woken);
}

// newlib's printf() lands here; overrides CubeMX's weak stub in syscalls.c
int _write(int fd, char *ptr, int len)
{
    (void)fd;
    HAL_UART_Transmit(&CONSOLE_HUART, (uint8_t *)ptr, len, HAL_MAX_DELAY);
    return len;
}

#endif

static void console_task(void *arg)
{
    (void)arg;
    uint8_t c;
    printf("3010 Console\r\n");
    printf(PROMPT);
    fflush(stdout);
    con.editing = 1; // the prompt is on screen now, so later output must wipe and redraw it

    for (;;) {
#if defined(ESP_PLATFORM)
        // ESP32 Style Code

        // Block until a byte arrives; the sequence timeouts are checked when the next byte lands.
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) == 1) {
            process(&con, (char)c);
        }
#elif defined(USE_HAL_DRIVER)
        // STM32 Style Code

        // Block until the UART ISR queues a byte
        if (xQueueReceive(rx_queue, &c, portMAX_DELAY) == pdTRUE) {
            process(&con, (char)c);
        }
#endif
    }
}

void console_start(void)
{
    lock = xSemaphoreCreateRecursiveMutex();

#if defined(ESP_PLATFORM)
    // ESP32 Style Code
    esp_log_set_vprintf(console_vprintf);
    // RX ring buffer only. TX still goes through printf's VFS path to the same UART.
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, RX_BUF_SIZE, 0, 0, NULL, 0));

#elif defined(USE_HAL_DRIVER)
    // STM32 Style Code

    rx_queue = xQueueCreate(RX_BUF_SIZE, sizeof(uint8_t));
    configASSERT(rx_queue != NULL);
    
    // No stdio buffering: the editor's escape sequences must go out immediately
    // setvbuf(stdout, NULL, _IONBF, 0);
    HAL_UART_Receive_IT(&CONSOLE_HUART, &rx_byte, 1);
#endif

    xTaskCreate(console_task, "console", TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL);
}
