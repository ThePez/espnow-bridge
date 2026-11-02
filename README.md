# espnow-bridge

Replaces the nRF24L01+ radio on the CSSE3010 STM32 with an ESP32-S3 running ESP-NOW. The STM32
doesn't know about WiFi at all: it talks to the ESP over SPI through two functions, and the ESP
does the rest. A second ESP32-S3 acts as a base station that receives, Hamming-decodes and
prints what the radios send.

```
 STM32F429 (SPI master)        ESP32-S3 "spi-bridge"         ESP32-S3 "base-station"
┌──────────────────────┐ SPI ┌──────────────────────┐ ESP-NOW ┌──────────────────────┐
│ esp_link_send_packet │────>│ SPI slave -> ESP-NOW │────────>│ monitor: decode and  │
│ esp_link_request     │<────│ peer list in NVS     │<────────│ print each packet    │
└──────────────────────┘     └──────────────────────┘         └──────────────────────┘
     console over UART           console over UART                console over UART
```

All three boards have the same line-editing console (history, Tab completion, a command table).

## Layout

| Folder | What it is |
|---|---|
| `stm32-master/` | CubeMX project for the Nucleo-F429ZI. FreeRTOS, USART3 console, SPI2 to the ESP |
| `spi-bridge/` | ESP-IDF project: SPI slave that forwards the STM32's packets over ESP-NOW |
| `base-station/` | ESP-IDF project: receives ESP-NOW packets, Hamming-decodes and prints them |
| `libs/` | Everything shared (see below) |

### `libs/`

| Component | Used by | Purpose |
|---|---|---|
| `embedded-console` | all three | Line editor, history, Tab completion and the command table (`commands.c`). One source for both chips; `commands.c` picks its commands with `ESP_PLATFORM` or `USE_HAL_DRIVER` |
| `espnow-comm` | ESP | Brings up NVS, WiFi and encrypted ESP-NOW; packet CRC; the receive queue |
| `mac-store` | ESP | The peer MACs (5 encrypted and 5 unencrypted), saved in NVS so they survive a reboot |
| `device-config` | ESP | Versioned config blobs in NVS (used by `mac-store`) |
| `common-functions` | ESP | Header-only error-check macros |
| `packet-monitor` | base-station | Task that drains the receive queue and prints each decoded packet |
| `slave-link` | spi-bridge | The ESP side of the SPI link |
| `master-link` | stm32-master | The STM32 side of the SPI link  |
| `spi-proto` | both ends | `spi-proto.h`: the SPI frame format. The only thing the two sides share |

Each ESP project builds `libs/` as extra components (`EXTRA_COMPONENT_DIRS`). The STM32 project
includes `embedded-console.cmake` and `master-link.cmake` from `libs/` instead.

## Hardware

Two ESP32-S3 boards and a Nucleo-F429ZI. **Join the grounds of all boards.** Without a common
ground the SPI data is corrupted and the symptoms look like a protocol bug.

SPI link (defaults, all overridable by macro, see `master-link.h` and `slave-link.c`):

| Signal | STM32 | ESP32-S3 |
|---|---|---|
| SCK | PB10 (SPI2) | GPIO 12 |
| MOSI | PC3 (SPI2) | GPIO 11 |
| MISO | PC2 (SPI2) | GPIO 13 |
| CS | PB12 (GPIO output, idle high) | GPIO 10 |
| READY | PF14 (GPIO input, pull-down) | GPIO 9 (output) |

In CubeMX, SPI2 is full-duplex master, 8 bit, MSB first, CPOL low, CPHA 1 edge, software NSS.
The speed is your choice; 21 MHz (prescaler 2) is fast for jumper wires, so start slower.
CS and READY are plain GPIOs: `esp_link` doesn't configure them.

Consoles run at the default UART settings of each board (the ESP's UART0, the Nucleo's USART3
over the ST-Link virtual COM port).

## Building

ESP projects (ESP-IDF 5.4.1, target `esp32s3`):

```bash
cd base-station && idf.py build flash monitor
cd spi-bridge   && idf.py build flash monitor
```

STM32 (CubeMX project with CMake presets, built with the bundled arm-none-eabi toolchain):

```bash
cd stm32-master && cmake --preset Debug && cmake --build --preset Debug
```

Build each project once before using the editor. `.clangd` in the repo root points the language
server at the `compile_commands.json` of the project that builds each part of `libs/`.

## First-time setup

There are two kinds of peer:

- **Encrypted** (the default): for your own boards. Both sides must list each other, with the
  same keys. The base station needs the bridge as a peer to decrypt its packets, and the bridge
  needs the base station to send to it.
- **Unencrypted** (`-u`): for devices that can't be given our keys, such as the machines the
  students control. Only the sender lists the machine's MAC. The machine receives the frames
  without registering anyone, and nothing is checked about who sent them.

The base station is for testing, so use encrypted peers there. The STM32 add command takes the
same `-u` flag, which is how students point their bridge at a machine.

1. On the base station console: `mac` (note it).
2. From the STM32 console: `esp_mac` (the bridge's MAC), then `esp_add <base-station MAC>`.
3. On the base station console: `peer_add <bridge MAC>`.
4. From the STM32 console: `esp_send`. The base station should print a `JOIN` packet.

Peers are saved in the ESP's NVS (up to 5 encrypted and 5 unencrypted), so this only needs doing
once per board. Adding a MAC that is already stored in the other list moves it. Both ESPs
use WiFi channel 9 and the PMK/LMK keys in `espnow-comm.c`; change them in one place and both
boards need the same value.

## Console commands

Tab completes command names, Up/Down walk the history, `help` lists everything.

ESP boards (base-station and spi-bridge):

| Command | Does |
|---|---|
| `peer_add <mac> [-u]` | Register a device and save it. `-u`: unencrypted |
| `peer_del <mac>` | Remove a device and forget it |
| `peer_clear` | Remove every stored device |
| `peers` | Show the registered devices, split into encrypted and unencrypted |
| `mac` | Show this board's MAC address |
| `summary` | Packet totals and per-sender counts (base-station) |

STM32 (these forward to the ESP over SPI):

| Command | Does |
|---|---|
| `esp_mac` | The ESP's MAC address |
| `esp_peers` | The ESP's stored peers, split into encrypted and unencrypted |
| `esp_add <mac> [-u]` / `esp_del <mac>` / `esp_clear` | Same as the ESP's own peer commands |
| `esp_send` | Send an already-encoded test JOIN through the ESP |

All boards also have `hello`, `echo`, `heap`, `uptime`, `reboot` and `help`.

## The SPI link

The STM32's whole API is in `libs/master-link/master-link.h`:

```c
void    esp_link_init(void);
uint8_t esp_link_send_packet(uint8_t *tx_buf);   // 32 bytes, same payload as the nrf
uint8_t esp_link_request(uint8_t cmd, const uint8_t *arg, uint8_t arg_len,
                         uint8_t *resp, uint8_t *resp_len);
```

Every SPI transaction is one 40-byte `EspLinkFrame` each way: start byte, sequence number,
command/status code, length, 32 bytes of payload and a CRC-16. The slave has to load its reply
before the master starts clocking, so the reply to a transaction is the answer to the previous
one. A request is therefore two transactions: the request, then a NOP that clocks the answer out.
The ESP raises READY when it has a reply loaded and the master waits for it before each
transaction. The commands are listed in `libs/spi-proto/spi-proto.h` (send a packet, add, delete
or clear a peer, get the MAC, get the peers). The STM32 only knows the codes; the ESP decides
what they do.

The ESP slave runs without DMA, so a frame must stay at or under 64 bytes.

## Packets

The 32 byte payload is 16 bytes, each Hamming-encoded into two bytes (a Hamming(7,4) code with
an overall parity bit per nibble: one flipped bit is corrected, two are detected and the packet is
dropped). The 16 decoded bytes are `type` (1), sender address (4) and payload (11):

| Type | Payload | Meaning |
|---|---|---|
| `0x20` | `"JOIN"` and zeros | A sender joining |
| `0x22` | `"XYZ"` and 8 digits | x (3 digits), y (3), z (2). Flagged out of range above 200 / 200 / 99 |

The base station prints each valid packet with its timestamp, the encoded and decoded hex, any
corrections it made and a note when the sender address changes. Anything that fails Hamming is
dropped and counted; anything that decodes but doesn't match one of the formats is printed as
`BAD FORMAT`. `summary` shows the counts.

## Testing

- **Link only:** `esp_mac` from the STM32 should print the same address as `mac` on the bridge.
  If it fails, check the common ground, then the wiring, then slow the SPI clock.
- **Whole path:** `esp_send` on the STM32, then watch the base station.
- **Without the STM32:** `spi-bridge/main/packet_sender.c` holds a table of well formed packets,
  bad-format packets, packets with correctable and uncorrectable Hamming errors and one with a bad
  CRC. Uncomment `packet_sender_start()` in `spi-bridge.c` to send one every 2 seconds to every
  peer. The base station should count each kind as described in the comments of that table.

## Notes

- `libs/embedded-console/commands.c` includes ESP-only headers (`packet-monitor.h`, `mac-store.h`) and
  STM32-only ones (`master-link.h`) behind `#if`, so the same file builds for both.
- The bad-CRC test packet makes the base station log `Invalid wifi packet recieved`. That is the
  expected result.
