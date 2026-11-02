#include "packet_sender.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_now.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "console.h"
#include "espnow_comm.h"

/* ------------------------------------------------------------------------- */
/* Test packets                                                              */
/* ------------------------------------------------------------------------- */

#define SEND_PERIOD_MS 2000

#define TYPE_JOIN 0x20
#define TYPE_XYZ  0x22

// The sender addresses used in the packets (not the MAC address of this board)
#define ADDR_A {0xA0, 0xB1, 0xC2, 0xD3}
#define ADDR_B {0x11, 0x22, 0x33, 0x44}

// Flips `mask` in byte `byte` of the 32 byte encoded packet. Each decoded byte is two encoded
// bytes: the encoded high nibble first, then the encoded low nibble. Flipping one bit in a
// byte is a correctable error. Flipping two bits in the same byte is not.
typedef struct {
    uint8_t byte;
    uint8_t mask;
} Flip;

typedef struct {
    const char *name;
    uint8_t type;
    uint8_t addr[4];
    uint8_t payload[11]; // not terminated, exactly as it goes on the wire
    Flip flips[3];       // unused slots have mask 0
    bool badCrc;         // corrupt the CRC16 so the receiving ESP drops it
} TestPacket;

static const TestPacket tests[] = {
    /* well formed */
    {.name = "JOIN from A", .type = TYPE_JOIN, .addr = ADDR_A, .payload = "JOIN"},
    {.name = "XYZ in range", .type = TYPE_XYZ, .addr = ADDR_A, .payload = "XYZ10005025"},
    {.name = "XYZ at the limits", .type = TYPE_XYZ, .addr = ADDR_A, .payload = "XYZ20020099"},
    {.name = "XYZ out of range", .type = TYPE_XYZ, .addr = ADDR_A, .payload = "XYZ25012050"},
    {.name = "XYZ from B (sender changes)", .type = TYPE_XYZ, .addr = ADDR_B,
     .payload = "XYZ00100002"},
    {.name = "JOIN from B (takes over)", .type = TYPE_JOIN, .addr = ADDR_B, .payload = "JOIN"},

    /* decodes fine, bad format */
    {.name = "unknown type 0x30", .type = 0x30, .addr = ADDR_A, .payload = "HELLO"},
    {.name = "XYZ with a letter", .type = TYPE_XYZ, .addr = ADDR_A, .payload = "XYZ12A04506"},
    {.name = "XYZ with wrong tag", .type = TYPE_XYZ, .addr = ADDR_A, .payload = "ABC10005025"},
    {.name = "JOIN with trailing junk", .type = TYPE_JOIN, .addr = ADDR_A, .payload = "JOINxx"},
    {.name = "XYZ payload on a JOIN type", .type = TYPE_JOIN, .addr = ADDR_A,
     .payload = "XYZ10005025"},

    /* hamming errors the receiver should correct */
    {.name = "1 bit error (data bit)", .type = TYPE_XYZ, .addr = ADDR_A,
     .payload = "XYZ10005025", .flips = {{3, 0x10}}},
    {.name = "1 bit error (parity bit)", .type = TYPE_XYZ, .addr = ADDR_A,
     .payload = "XYZ10005025", .flips = {{8, 0x01}}},
    {.name = "3 bytes with 1 bit error each", .type = TYPE_JOIN, .addr = ADDR_A,
     .payload = "JOIN", .flips = {{0, 0x04}, {11, 0x80}, {20, 0x20}}},

    /* hamming errors the receiver should drop */
    {.name = "2 bit error in one byte", .type = TYPE_XYZ, .addr = ADDR_A,
     .payload = "XYZ10005025", .flips = {{5, 0x06}}},
    {.name = "2 bit error in the type", .type = TYPE_JOIN, .addr = ADDR_A, .payload = "JOIN",
     .flips = {{1, 0x30}}},

    /* corrected, but then bad format */
    {.name = "1 bit error + bad format", .type = 0x30, .addr = ADDR_A, .payload = "HELLO",
     .flips = {{14, 0x08}}},

    /* dropped by the espnow layer, never reaches the listener */
    {.name = "bad CRC16", .type = TYPE_XYZ, .addr = ADDR_A, .payload = "XYZ10005025",
     .badCrc = true},
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

/* ------------------------------------------------------------------------- */
/* Hamming(7,4) + overall parity encode, the inverse of the decoder in listener.c */
/* ------------------------------------------------------------------------- */

static uint8_t ham_encode_nibble(uint8_t d)
{
    uint8_t d0 = d & 1, d1 = (d >> 1) & 1, d2 = (d >> 2) & 1, d3 = (d >> 3) & 1;
    uint8_t h0 = d1 ^ d2 ^ d3;
    uint8_t h1 = d0 ^ d2 ^ d3;
    uint8_t h2 = d0 ^ d1 ^ d3;

    // bit 0 is the parity, bits 1-3 the hamming bits, bits 4-7 the data
    uint8_t w = (uint8_t)((h0 << 1) | (h1 << 2) | (h2 << 3) | (d << 4));
    uint8_t parity = 0;
    for (int i = 1; i < 8; i++) {
        parity ^= (w >> i) & 1;
    }

    return w | parity;
}

// low byte of the result encodes the high nibble, high byte the low nibble
static uint16_t ham_encode_byte(uint8_t b)
{
    return (uint16_t)((ham_encode_nibble(b & 0x0F) << 8) | ham_encode_nibble(b >> 4));
}

/* ------------------------------------------------------------------------- */
/* Sending                                                                   */
/* ------------------------------------------------------------------------- */

static void send_test(size_t index)
{
    const TestPacket *t = &tests[index];

    // 16 decoded bytes: type, 4 byte address, 11 byte payload
    uint8_t raw[16];
    raw[0] = t->type;
    memcpy(&raw[1], t->addr, 4);
    memcpy(&raw[5], t->payload, 11);

    uint16_t enc[16];
    for (int i = 0; i < 16; i++) {
        enc[i] = ham_encode_byte(raw[i]);
    }

    struct wifi_packet_t packet = {.packet_id = RAW};
    memcpy(packet.data.raw, enc, sizeof(enc));

    for (size_t i = 0; i < sizeof(t->flips) / sizeof(t->flips[0]); i++) {
        packet.data.raw[t->flips[i].byte] ^= t->flips[i].mask;
    }

    esp_err_t err;
    if (t->badCrc) {
        // esp_now_send_packet() would fix the CRC, so compute it and then break it
        packet.crc16 = esp_rom_crc16_le(0, (uint8_t *)&packet.data, sizeof(union packet_data)) ^ 0xFFFF;
        err = esp_now_send(NULL, (uint8_t *)&packet, sizeof(packet));
    } else {
        err = esp_now_send_packet(NULL, &packet); // NULL = every registered peer
    }

    console_printf("[%u/%u] %s: %s\r\n", (unsigned)(index + 1), (unsigned)NUM_TESTS, t->name,
                   err == ESP_OK ? "sent" : esp_err_to_name(err));
}

static void sender_task(void *arg)
{
    (void)arg;

    for (;;) {
        for (size_t i = 0; i < NUM_TESTS; i++) {
            vTaskDelay(pdMS_TO_TICKS(SEND_PERIOD_MS));
            send_test(i);
        }

        console_printf("--- end of list, starting again ---\r\n");
    }
}

void packet_sender_start(void)
{
    xTaskCreate(sender_task, "Sender", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
}
