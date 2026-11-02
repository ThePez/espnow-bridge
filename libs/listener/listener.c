#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "console.h"
#include "espnow_comm.h"
#include "listener.h"

/* ------------------------------------------------------------------------- */
/* Types and state                                                           */
/* ------------------------------------------------------------------------- */

// packet types
#define TYPE_JOIN 0x20
#define TYPE_XYZ  0x22

// limits for the XYZ values
#define X_MAX 200
#define Y_MAX 200
#define Z_MAX 99

#define MAX_SENDERS 10

typedef struct {
    uint8_t addr[4];
    uint32_t packets;
    uint32_t joins;
} Sender;

typedef struct {
    uint8_t type;
    uint8_t addr[4];
    uint8_t payload[11];
} Packet;

typedef enum {
    HAM_OK,
    HAM_CORRECTED,
    HAM_UNCORRECTABLE
} HamResult;

static Sender senders[MAX_SENDERS];
static int senderCount = 0;
static int lastSender = -1;

static uint32_t totalPackets = 0;
static uint32_t correctedPackets = 0;
static uint32_t rejectedHamming = 0;
static uint32_t rejectedFormat = 0;

/* ------------------------------------------------------------------------- */
/* Hamming decode                                                            */
/* ------------------------------------------------------------------------- */

// bit position that is wrong for each syndrome value (index 0 = no error)
static const uint8_t HAM_SYNDROME_TO_BIT[8] = {0, 1, 2, 6, 3, 5, 4, 7};

static HamResult ham_decode_nibble(uint8_t w, uint8_t *nibble)
{
    uint8_t data0, data1, data2, data3, ham0, ham1, ham2, syn0, syn1, syn2;
    uint8_t *bits[] = {&ham0, &ham1, &ham2, &data0, &data1, &data2, &data3};
    uint8_t parity = !!(w & 1);
    for (uint8_t i = 1; i < 8; i++) {
        *bits[i - 1] = (w >> i) & 1;
        parity ^= *bits[i - 1];
    }

    // Calculate the Syndrome
    syn0 = ham0 ^ data1 ^ data2 ^ data3;
    syn1 = ham1 ^ data0 ^ data2 ^ data3;
    syn2 = ham2 ^ data0 ^ data1 ^ data3;
    uint8_t syndrome = (syn2 << 2) | (syn1 << 1) | syn0;

    HamResult result = HAM_OK;
    if (!syndrome) {
        if (!parity) {
            result = HAM_OK;
        } else {
            result = HAM_CORRECTED;
        }
    } else {
        if (!parity) {
            result = HAM_UNCORRECTABLE;
        } else {
            // single error, flip it back
            w ^= (uint8_t)(1u << HAM_SYNDROME_TO_BIT[syndrome]);
            result = HAM_CORRECTED;
        }
    }

    *nibble = (w >> 4) & 0x0F;
    return result;
}

static HamResult ham_decode_byte(uint16_t enc, uint8_t *byte)
{
    uint8_t hi;
    HamResult hiRes = ham_decode_nibble(enc & 0xFF, &hi);
    if (hiRes == HAM_UNCORRECTABLE) {
        *byte = 0;
        return HAM_UNCORRECTABLE;
    }

    HamResult loRes = ham_decode_nibble((enc >> 8) & 0xFF, byte);
    if (loRes == HAM_UNCORRECTABLE) {
        *byte = 0;
        return HAM_UNCORRECTABLE;
    }

    *byte |= (hi << 4);
    return (hiRes == HAM_CORRECTED || loRes == HAM_CORRECTED) ? HAM_CORRECTED : HAM_OK;
}

static HamResult hamDecodePacket(const void *in, void *out, uint8_t *corrected)
{
    // Alias inputs
    uint8_t *pkt = (uint8_t *)out;
    const uint16_t *enc = (const uint16_t *)in;

    HamResult worst = HAM_OK;
    uint8_t fixes = 0;
    for (int i = 0; i < 16; i++) {
        HamResult res = ham_decode_byte(enc[i], &pkt[i]);
        if (res == HAM_CORRECTED) {
            fixes++;
        }

        if (res > worst) {
            worst = res;
        }
    }

    if (corrected != NULL) {
        *corrected = fixes;
    }

    return worst;
}

/* ------------------------------------------------------------------------- */
/* Parsing                                                                   */
/* ------------------------------------------------------------------------- */

static int findSender(const uint8_t addr[4])
{
    for (int i = 0; i < senderCount; i++) {
        if (memcmp(senders[i].addr, addr, 4) == 0) {
            return i;
        }
    }

    if (senderCount < MAX_SENDERS) {
        memcpy(senders[senderCount].addr, addr, 4);
        return senderCount++;
    }

    return -1;
}

static int parseJoin(const uint8_t *payload)
{
    return memcmp(payload, "JOIN\0\0\0\0\0\0\0", 11) != 0 ? 1 : 0;
}

static int parseXyz(const uint8_t *payload, int *x, int *y, int *z)
{
    if (memcmp(payload, "XYZ", 3) != 0) {
        return 1;
    }

    for (int i = 3; i < 11; i++) {
        if (payload[i] < '0' || payload[i] > '9') {
            return 1;
        }
    }

    const uint8_t *d = &payload[3];
    *x = (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
    *y = (d[3] - '0') * 100 + (d[4] - '0') * 10 + (d[5] - '0');
    *z = (d[6] - '0') * 10 + (d[7] - '0');
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Reporting                                                                 */
/* ------------------------------------------------------------------------- */

static void reportPacket(const uint16_t enc[16])
{
    Packet pkt;
    uint8_t fixes = 0;
    HamResult ecc = hamDecodePacket(enc, &pkt, &fixes);

    // drop packets with errors that can't be corrected, nothing in them can be trusted
    if (ecc == HAM_UNCORRECTABLE) {
        rejectedHamming++;
        return;
    } else if (ecc == HAM_CORRECTED) {
        correctedPackets++;
    }

    int x = 0;
    int y = 0;
    int z = 0;
    int formatOk;
    switch (pkt.type) {
    case TYPE_JOIN:
        formatOk = (parseJoin(pkt.payload) == 0);
        break;
    case TYPE_XYZ:
        formatOk = (parseXyz(pkt.payload, &x, &y, &z) == 0);
        break;
    default:
        formatOk = 0; // unknown type
        break;
    }

    // totals: everything that passed Hamming is counted, only well formed packets are
    // credited to their sender
    totalPackets++;
    if (!formatOk) {
        rejectedFormat++;
    }

    int sender = findSender(pkt.addr);
    if (sender >= 0 && formatOk) {
        senders[sender].packets++;
        if (pkt.type == TYPE_JOIN) {
            senders[sender].joins++;
        }
    }

    // Pre-format the variable parts, then print whole lines (console_printf needs each call
    // to end in a newline). snprintf's size includes the '\0', hence 4 for "XX " per byte.
    char encHex[3 * 32 + 1];
    for (uint8_t i = 0; i < 32; i++) {
        snprintf(&encHex[i * 3], 4, "%02X ", ((const uint8_t *)enc)[i]);
    }

    char decHex[3 * sizeof(Packet) + 1];
    for (uint8_t i = 0; i < sizeof(Packet); i++) {
        snprintf(&decHex[i * 3], 4, "%02X ", ((const uint8_t *)&pkt)[i]);
    }

    // the data is only printed (and trusted) when the format checked out
    if (!formatOk) {
        char text[12] = {0};
        for (int i = 0; i < 11; i++) {
            text[i] = (pkt.payload[i] >= 0x20 && pkt.payload[i] < 0x7F) ? pkt.payload[i] : '?';
        }

        console_printf("\r\n[%8lu ms] #%lu  from %02X%02X%02X%02X  BAD FORMAT  type 0x%02X  "
                       "payload \"%s\"\r\n",
                       (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()),
                       (unsigned long)totalPackets, pkt.addr[0], pkt.addr[1], pkt.addr[2],
                       pkt.addr[3], pkt.type, text);
    } else {
        char data[48];
        if (pkt.type == TYPE_JOIN) {
            snprintf(data, sizeof(data), "JOIN");
        } else {
            snprintf(data, sizeof(data), "XYZ x=%d y=%d z=%d%s", x, y, z,
                     (x > X_MAX || y > Y_MAX || z > Z_MAX) ? "  ** OUT OF RANGE **" : "");
        }

        console_printf("\r\n[%8lu ms] #%lu  from %02X%02X%02X%02X  %s\r\n",
                       (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()),
                       (unsigned long)totalPackets, pkt.addr[0], pkt.addr[1], pkt.addr[2],
                       pkt.addr[3], data);
    }

    console_printf("  encoded : %s\r\n", encHex);
    console_printf("  decoded : %s\r\n", decHex);

    if (ecc == HAM_CORRECTED) {
        console_printf("  hamming: ok after correcting %u single bit error(s)\r\n", fixes);
    }

    // flag the sender changing, e.g. another user's JOIN taking over a shared AOXB
    if (sender >= 0 && formatOk) {
        if (lastSender >= 0 && sender != lastSender) {
            const uint8_t *prev = senders[lastSender].addr;
            console_printf("  ** SENDER CHANGED: %02X%02X%02X%02X -> %02X%02X%02X%02X%s **\r\n",
                           prev[0], prev[1], prev[2], prev[3], pkt.addr[0], pkt.addr[1],
                           pkt.addr[2], pkt.addr[3],
                           pkt.type == TYPE_JOIN ? " (JOIN, this sender now controls the AOXB)"
                                                 : "");
        }
        lastSender = sender;
    }
}

void listener_print_summary(void)
{
    console_printf("... %lu packets (%lu needed correcting), %lu bad format, dropped: %lu failed "
                   "Hamming\r\n",
                   (unsigned long)totalPackets, (unsigned long)correctedPackets,
                   (unsigned long)rejectedFormat, (unsigned long)rejectedHamming);
    for (int i = 0; i < senderCount; i++) {
        console_printf("  | %02X%02X%02X%02X: %lu (%lu JOIN)\r\n", senders[i].addr[0],
                       senders[i].addr[1], senders[i].addr[2], senders[i].addr[3],
                       (unsigned long)senders[i].packets, (unsigned long)senders[i].joins);
    }
}

/* ------------------------------------------------------------------------- */
/* Task                                                                      */
/* ------------------------------------------------------------------------- */

static void listener_task(void *pvParams)
{
    (void)pvParams;

    struct wifi_packet_t packet = {0};

    for (;;) {
        // Block until a packet arrives, then drain everything that queued up while we were
        // busy printing
        if (xQueueReceive(wifiQueue, &packet, portMAX_DELAY) == pdTRUE) {
            do {
                reportPacket((const uint16_t *)packet.data.raw);
            } while (xQueueReceive(wifiQueue, &packet, 0) == pdTRUE);
        }
    }
}

void listener_start(void)
{
    xTaskCreate(listener_task, "Listener", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
}
