/**
 ******************************************************************************
 * @file    commands.c
 * @author  Jack Cairns
 * @brief   Implementation of the command registry in commands.h. One
 *          table drives dispatch, help and completion. ESP32 builds get
 *          the peer and summary commands, STM32 builds get the esp_*
 *          commands that go over the SPI link.
 ******************************************************************************
 */

#include "commands.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(ESP_PLATFORM)

#include "esp_wifi_types_generic.h"

#include "esp_err.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espnow-comm.h"
#include "packet-monitor.h"
#include "mac-store.h"

#elif defined(USE_HAL_DRIVER)

#include "FreeRTOS.h"
#include "stm32f4xx_hal.h"
#include "task.h"

#include "master-link.h"

#else

#error "commands: unsupported platform (need ESP_PLATFORM or USE_HAL_DRIVER)"

#endif

#define MAX_ARGS 50

typedef struct {
    const char *name;
    const char *args; // e.g. "<mac>", "" if none. Shown after the name in help
    const char *desc;
    void (*fn)(int argc, char **argv);
} Command;

#if defined(ESP_PLATFORM)
static void cmd_peer_add(int argc, char **argv);
static void cmd_peer_del(int argc, char **argv);
static void cmd_peer_clear(int argc, char **argv);
static void cmd_peers(int argc, char **argv);
static void cmd_display_mac(int argc, char **argv);
static void cmd_summary(int argc, char **argv);
#endif
#if defined(USE_HAL_DRIVER)
static void cmd_esp_mac(int argc, char **argv);
static void cmd_esp_peers(int argc, char **argv);
static void cmd_esp_add(int argc, char **argv);
static void cmd_esp_del(int argc, char **argv);
static void cmd_esp_clear(int argc, char **argv);
static void cmd_esp_send(int argc, char **argv);
#endif
static void cmd_hello(int argc, char **argv);
static void cmd_echo(int argc, char **argv);
static void cmd_heap(int argc, char **argv);
static void cmd_uptime(int argc, char **argv);
static void cmd_reboot(int argc, char **argv);
static void cmd_help(int argc, char **argv);

// The command registry: dispatch, help and Tab completion all read from this table.
static const Command commands[] = {
#if defined(ESP_PLATFORM)
    {"peer_add", "<mac> [-u]", "Add a device and remember it. -u: unencrypted", cmd_peer_add},
    {"peer_del", "<mac>", "Remove a device and forget it", cmd_peer_del},
    {"peer_clear", "", "Remove every stored device", cmd_peer_clear},
    {"peers", "", "Show the registered devices (encrypted and not)", cmd_peers},
    {"mac", "", "Show this device's MAC address", cmd_display_mac},
    {"summary", "", "Show packet counts and per-sender totals", cmd_summary},
#endif
#if defined(USE_HAL_DRIVER)
    {"esp_mac", "", "Ask the ESP for its MAC address", cmd_esp_mac},
    {"esp_peers", "", "Ask the ESP for its stored peers", cmd_esp_peers},
    {"esp_add", "<mac> [-u]", "Tell the ESP to add a peer. -u: unencrypted", cmd_esp_add},
    {"esp_del", "<mac>", "Tell the ESP to remove a peer", cmd_esp_del},
    {"esp_clear", "", "Tell the ESP to remove every peer", cmd_esp_clear},
    {"esp_send", "", "Send a test JOIN packet through the ESP", cmd_esp_send},
#endif
    {"hello", "", "Print a greeting", cmd_hello},
    {"echo", "<text>", "Print the arguments back", cmd_echo},
    {"heap", "", "Show free heap memory", cmd_heap},
    {"uptime", "", "Show time since boot", cmd_uptime},
    {"reboot", "", "Restart the device", cmd_reboot},
    {"help", "", "Show this message", cmd_help},
};
static const size_t NUM_COMMANDS = sizeof(commands) / sizeof(commands[0]);

/* ------------------------------------------------------------------------- */
/* Platform glue                                                             */
/* ------------------------------------------------------------------------- */

static uint32_t platform_free_heap(void)
{
#if defined(ESP_PLATFORM)
    return esp_get_free_heap_size();
#else
    return (uint32_t)xPortGetFreeHeapSize();
#endif
}

static void platform_reboot(void)
{
#if defined(ESP_PLATFORM)
    esp_restart();
#else
    HAL_NVIC_SystemReset();
#endif
}

/* ------------------------------------------------------------------------- */
/* Registry                                                                  */
/* ------------------------------------------------------------------------- */

size_t commands_complete(const char *prefix, const char **matches, size_t max)
{
    size_t n = 0;
    size_t len = strlen(prefix);
    for (size_t i = 0; i < NUM_COMMANDS && n < max; i++) {
        if (strncmp(commands[i].name, prefix, len) == 0) {
            matches[n++] = commands[i].name;
        }
    }

    return n;
}

void commands_execute(char *line)
{
    char *argv[MAX_ARGS];
    int argc = 0;

    for (char *tok = strtok(line, " "); tok != NULL && argc < MAX_ARGS; tok = strtok(NULL, " ")) {
        argv[argc++] = tok;
    }

    if (argc == 0) {
        return;
    }

    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].fn(argc, argv);
            return;
        }
    }

    printf("Unknown command: %s\r\n", argv[0]);
}

/* ------------------------------------------------------------------------- */
/* Commands                                                                  */
/* ------------------------------------------------------------------------- */

// Parses "aa:bb:cc:dd:ee:ff" into 6 bytes. Returns 0 on success, 1 on malformed input.
static uint8_t parse_mac(const char *str, uint8_t mac[6])
{
    unsigned int b[6];
    int end = 0;
    if (sscanf(str, "%x:%x:%x:%x:%x:%x%n", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &end) != 6 ||
        str[end] != '\0') {
        return 1;
    }

    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xFF) {
            return 1;
        }

        mac[i] = (uint8_t)b[i];
    }

    return 0;
}

// Parses the arguments of peer_add / esp_add: "<mac>" or "<mac> -u". Returns 0 on success,
// 1 if the arguments are the wrong shape, 2 if the MAC is malformed.
static uint8_t parse_peer_args(int argc, char **argv, uint8_t mac[6], bool *encrypted)
{
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "-u") != 0)) {
        return 1;
    }

    if (parse_mac(argv[1], mac)) {
        return 2;
    }

    *encrypted = (argc == 2);
    return 0;
}

static void print_peer_usage(const char *name)
{
    printf("Usage: %s <mac> [-u]   e.g. %s aa:bb:cc:dd:ee:ff\r\n", name, name);
    printf("  -u  unencrypted peer (for devices that can't be given our keys)\r\n");
}

#if defined(ESP_PLATFORM)

static void cmd_peer_add(int argc, char **argv)
{
    uint8_t mac[6];
    bool encrypted;
    uint8_t rc = parse_peer_args(argc, argv, mac, &encrypted);
    if (rc == 1) {
        print_peer_usage("peer_add");
        return;
    } else if (rc == 2) {
        printf("Invalid MAC address, expected aa:bb:cc:dd:ee:ff\r\n");
        return;
    }

    esp_err_t err = esp_now_module_add_peer(mac, encrypted);
    if (err != ESP_OK) {
        printf("Failed to add peer (%s)\r\n", esp_err_to_name(err));
        return;
    }

    printf("Added %s peer %02x:%02x:%02x:%02x:%02x:%02x", encrypted ? "encrypted" : "unencrypted",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Remember it so it is added again on the next boot
    err = mac_store_add(mac, encrypted);
    if (err == ESP_OK) {
        printf(" (saved)\r\n");
    } else if (err == ESP_ERR_NO_MEM) {
        printf(" (not saved, %d %s peers already stored)\r\n",
               encrypted ? MAC_STORE_MAX_ENCRYPTED : MAC_STORE_MAX_PLAIN,
               encrypted ? "encrypted" : "unencrypted");
    } else {
        printf(" (not saved: %s)\r\n", esp_err_to_name(err));
    }
}

static void cmd_peer_del(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: peer_del <mac>   e.g. peer_del aa:bb:cc:dd:ee:ff\r\n");
        return;
    }

    uint8_t mac[6];
    if (parse_mac(argv[1], mac)) {
        printf("Invalid MAC address, expected aa:bb:cc:dd:ee:ff\r\n");
        return;
    }

    // Forget it for next boot and stop receiving from it now
    esp_err_t stored = mac_store_remove(mac);
    esp_err_t live = esp_now_del_peer(mac);

    if (stored == ESP_ERR_NOT_FOUND && live == ESP_ERR_ESPNOW_NOT_FOUND) {
        printf("No such peer\r\n");
        return;
    }

    if (stored != ESP_OK && stored != ESP_ERR_NOT_FOUND) {
        printf("Failed to update the stored list (%s)\r\n", esp_err_to_name(stored));
        return;
    }

    if (live != ESP_OK && live != ESP_ERR_ESPNOW_NOT_FOUND) {
        printf("Failed to remove peer (%s)\r\n", esp_err_to_name(live));
        return;
    }

    printf("Removed peer %02x:%02x:%02x:%02x:%02x:%02x\r\n", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

static void cmd_peer_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    size_t count = mac_store_count(true) + mac_store_count(false);
    for (int type = 0; type < 2; type++) {
        bool encrypted = type == 0;
        for (size_t i = 0; i < mac_store_count(encrypted); i++) {
            uint8_t mac[6];
            if (mac_store_get(encrypted, i, mac) == ESP_OK) {
                esp_now_del_peer(mac);
            }
        }
    }

    esp_err_t err = mac_store_clear();
    if (err != ESP_OK) {
        printf("Failed to clear the stored list (%s)\r\n", esp_err_to_name(err));
        return;
    }

    printf("Cleared %u stored peer(s)\r\n", (unsigned)count);
}

static void cmd_peers(int argc, char **argv)
{
    esp_now_peer_num_t num = {0};
    esp_err_t err = esp_now_get_peer_num(&num);
    if (err != ESP_OK) {
        printf("ESP-wifi not initialised\r\n");
        return;
    }

    // One list per type, each found by walking all the peers
    for (int pass = 0; pass < 2; pass++) {
        bool encrypted = pass == 0;
        printf("%s:\r\n", encrypted ? "Encrypted" : "Unencrypted");

        int shown = 0;
        for (int i = 0; i < num.total_num; i++) {
            esp_now_peer_info_t peer;
            // !i only true when i == 0
            err = esp_now_fetch_peer(!i, &peer);
            if (err != ESP_OK) {
                break;
            }

            if (peer.encrypt != encrypted) {
                continue;
            }

            uint8_t *mac = peer.peer_addr;
            printf("  %02x:%02x:%02x:%02x:%02x:%02x\r\n", mac[0], mac[1], mac[2], mac[3], mac[4],
                   mac[5]);
            shown++;
        }

        if (shown == 0) {
            printf("  none\r\n");
        }
    }
}

static void cmd_display_mac(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        printf("ESP-wifi not initialised\r\n");
        return;
    }

    printf("Mac address:\r\n%02X:%02X:%02X:%02X:%02X:%02X\r\n", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
}

static void cmd_summary(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    packet_monitor_print_summary();
}

#endif

#if defined(USE_HAL_DRIVER)

// Prints the result of an esp_link call
static void esp_link_report(const char *what, uint8_t rc)
{
    if (rc == HAL_OK) {
        printf("%s: ok\r\n", what);
    } else if (rc == HAL_TIMEOUT) {
        printf("%s: timed out waiting for the ESP (READY)\r\n", what);
    } else if (rc == ESP_LINK_RC_BUSY) {
        printf("%s: ESP busy (ESP-NOW queue full), try again\r\n", what);
    } else if (rc == ESP_LINK_RC_NO_PEERS) {
        printf("%s: the ESP has no peers, add one first\r\n", what);
    } else if (rc >= ESP_LINK_RC_BASE) {
        printf("%s: the ESP reported an error (status %u)\r\n", what, rc - ESP_LINK_RC_BASE);
    } else {
        printf("%s: failed (bad transfer or reply)\r\n", what);
    }
}

static void print_mac(const uint8_t *mac)
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x\r\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void cmd_esp_mac(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint8_t resp[ESP_LINK_PAYLOAD_LEN];
    uint8_t len = 0;
    uint8_t rc = esp_link_request(ESP_LINK_CMD_GET_MAC, NULL, 0, resp, &len);
    if (rc != HAL_OK || len != 6) {
        esp_link_report("GET_MAC", rc == HAL_OK ? HAL_ERROR : rc);
        return;
    }

    printf("ESP MAC: ");
    print_mac(resp);
}

static void cmd_esp_peers(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // The ESP keeps one list per type and each fits in a reply, so this is one request each
    for (uint8_t type = ESP_LINK_PEER_ENCRYPTED; type <= ESP_LINK_PEER_UNENCRYPTED; type++) {
        uint8_t resp[ESP_LINK_PAYLOAD_LEN];
        uint8_t len = 0;
        uint8_t rc = esp_link_request(ESP_LINK_CMD_GET_PEERS, &type, 1, resp, &len);
        if (rc != HAL_OK || len < 1) {
            esp_link_report("GET_PEERS", rc == HAL_OK ? HAL_ERROR : rc);
            return;
        }

        printf("%s:\r\n", type == ESP_LINK_PEER_ENCRYPTED ? "Encrypted" : "Unencrypted");

        uint8_t shown = (uint8_t)((len - 1) / 6);
        for (uint8_t i = 0; i < shown; i++) {
            printf("  ");
            print_mac(&resp[1 + 6 * i]);
        }

        if (shown == 0) {
            printf("  none\r\n");
        }
    }
}

// esp_add and esp_del: one MAC argument, sent as the request argument
static void esp_mac_request(const char *name, uint8_t cmd, int argc, char **argv)
{
    uint8_t mac[6];
    if (argc != 2 || parse_mac(argv[1], mac)) {
        printf("Usage: %s <mac>   e.g. %s aa:bb:cc:dd:ee:ff\r\n", name, name);
        return;
    }

    esp_link_report(name, esp_link_request(cmd, mac, 6, NULL, NULL));
}

static void cmd_esp_add(int argc, char **argv)
{
    uint8_t arg[7]; // the MAC, then the peer type
    bool encrypted;
    uint8_t rc = parse_peer_args(argc, argv, arg, &encrypted);
    if (rc == 1) {
        print_peer_usage("esp_add");
        return;
    } else if (rc == 2) {
        printf("Invalid MAC address, expected aa:bb:cc:dd:ee:ff\r\n");
        return;
    }

    arg[6] = encrypted ? ESP_LINK_PEER_ENCRYPTED : ESP_LINK_PEER_UNENCRYPTED;
    esp_link_report("esp_add", esp_link_request(ESP_LINK_CMD_PEER_ADD, arg, sizeof(arg), NULL, NULL));
}

static void cmd_esp_del(int argc, char **argv)
{
    esp_mac_request("esp_del", ESP_LINK_CMD_PEER_DEL, argc, argv);
}

static void cmd_esp_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_link_report("esp_clear", esp_link_request(ESP_LINK_CMD_PEER_CLEAR, NULL, 0, NULL, NULL));
}

static void cmd_esp_send(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // An already Hamming encoded "JOIN" from sender A0B1C2D3, the rest of the 32 bytes are 0
    uint8_t join[ESP_LINK_PAYLOAD_LEN] = {0x2B, 0x00, 0xA5, 0x00, 0xB8, 0x1D, 0xC9, 0x2B, 0xD4,
                                          0x36, 0x47, 0xA5, 0x47, 0xFF, 0x47, 0x93, 0x47, 0xE2};
    esp_link_report("esp_send", esp_link_send_packet(join));
}

#endif

static void cmd_hello(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Hello from the base station\r\n");
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        printf("%s%s", argv[i], (i + 1) < argc ? " " : "");
    }

    printf("\r\n");
}

static void cmd_heap(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Free heap: %lu bytes\r\n", (unsigned long)platform_free_heap());
}

static void cmd_uptime(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint32_t secs = (xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000;
    printf("Uptime: %luh %lum %lus\r\n", (unsigned long)(secs / 3600),
           (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60));
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Rebooting...\r\n");
    fflush(stdout);
    platform_reboot();
}

static void cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // Width of the widest "name <args>", so the descriptions line up whatever the commands are
    int width = 0;
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        int w = (int)strlen(commands[i].name);
        if (commands[i].args[0] != '\0') {
            w += 1 + (int)strlen(commands[i].args);
        }

        if (w > width) {
            width = w;
        }
    }

    printf("Commands:\r\n");
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        char usage[32];
        snprintf(usage, sizeof(usage), "%s%s%s", commands[i].name,
                 commands[i].args[0] != '\0' ? " " : "", commands[i].args);
        printf("  %-*s  %s\r\n", width, usage, commands[i].desc);
    }
}
