#ifndef LINK_PROTO_H
#define LINK_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define ESP_LINK_PAYLOAD_LEN 32 // same size as the ESP-NOW packet data
#define ESP_LINK_FRAME_LEN   40 // every transaction, both directions
// MACs that fit in one GET_PEERS reply (after the count byte). A list can't be longer than this
#define ESP_LINK_PEERS_PER_REPLY ((ESP_LINK_PAYLOAD_LEN - 1) / 6)

/* Peer types, used by PEER_ADD and GET_PEERS */
enum {
    ESP_LINK_PEER_ENCRYPTED = 0,
    ESP_LINK_PEER_UNENCRYPTED = 1,
};

/* Magic bytes for the start of a frame */
enum {
    ESP_LINK_START_MASTER = 0xA5,
    ESP_LINK_START_SLAVE = 0x5A
};

/* Request codes, master -> slave (EspLinkFrame.code) */
enum {
    ESP_LINK_CMD_NOP = 0,         // no request, just clock the previous answer out
    ESP_LINK_CMD_SEND_PACKET = 1, // payload (32 bytes) is sent out over ESP-NOW
    ESP_LINK_CMD_PEER_ADD = 2,    // payload: MAC (6 bytes), then the type (ESP_LINK_PEER_*). Registers and stores it
    ESP_LINK_CMD_PEER_DEL = 3,    // payload: MAC (6 bytes). Removes it whatever its type
    ESP_LINK_CMD_PEER_CLEAR = 4,  // no payload
    ESP_LINK_CMD_GET_MAC = 5,     // answer: this board's MAC (6 bytes)
    ESP_LINK_CMD_GET_PEERS = 6,   // arg: the type wanted (ESP_LINK_PEER_*). Answer: count (1 byte), then
                                  // that many MACs (6 bytes each)
};

/* Status codes, slave -> master (EspLinkFrame.code) */
enum {
    ESP_LINK_STATUS_OK = 0,
    ESP_LINK_STATUS_BAD_FRAME = 1, // wrong start byte or length
    ESP_LINK_STATUS_BAD_CRC = 2,
    ESP_LINK_STATUS_UNKNOWN_CMD = 3,
    ESP_LINK_STATUS_FAILED = 4, // the ESP tried and the operation failed
};

typedef struct {
    uint8_t start; // ESP_LINK_START_MASTER or ESP_LINK_START_SLAVE
    uint8_t seq;   // master counts up per request, slave echoes it
    uint8_t code;  // master: ESP_LINK_CMD_*, slave: ESP_LINK_STATUS_*
    uint8_t len;   // bytes used in payload
    uint8_t payload[ESP_LINK_PAYLOAD_LEN];
    uint16_t crc16;  // over every byte before it (offsetof crc16)
    uint8_t _res[2]; // Reserved for padding
} EspLinkFrame;

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)
static inline uint16_t esp_link_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }

    return crc;
}

// Fills in the CRC. Call after everything else in the frame is set.
static inline void esp_link_frame_seal(EspLinkFrame *f)
{
    f->crc16 = esp_link_crc16((const uint8_t *)f, offsetof(EspLinkFrame, crc16));
}

// ESP_LINK_STATUS_OK if the frame starts with `start`, has a sane length and a good CRC.
static inline uint8_t esp_link_frame_check(const EspLinkFrame *f, uint8_t start)
{
    if (f->start != start || f->len > ESP_LINK_PAYLOAD_LEN) {
        return ESP_LINK_STATUS_BAD_FRAME;
    }

    if (f->crc16 != esp_link_crc16((const uint8_t *)f, offsetof(EspLinkFrame, crc16))) {
        return ESP_LINK_STATUS_BAD_CRC;
    }

    return ESP_LINK_STATUS_OK;
}

#endif
