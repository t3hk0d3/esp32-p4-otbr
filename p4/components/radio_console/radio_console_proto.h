#pragma once

// Pure parts of the `radio-fw` console command (docs/reference/radio-console.md): argument parsing, the frame decoder,
// the offset rule and the reply lines. No UART, no FreeRTOS: host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_CONSOLE_PROTO_MAX_CHUNK       1024
#define RADIO_CONSOLE_PROTO_MAX_IMAGE       0x180000 /* the C6 RCP slot (common/c6_board/partitions_c6.csv ota_0) */
#define RADIO_CONSOLE_PROTO_BYTE_TIMEOUT_MS 500      /* a frame that stalls this long between two bytes is dropped */
#define RADIO_CONSOLE_PROTO_HEADER_LEN      8        /* "RF", offset u32, length u16 */
#define RADIO_CONSOLE_PROTO_CRC_LEN         4
#define RADIO_CONSOLE_PROTO_LINE_MAX        192      /* one reply line, "\n" and NUL included */
#define RADIO_CONSOLE_PROTO_PREFIX          "@radio-fw "

/** CRC-32 (IEEE 802.3, as zlib's crc32): start with 0, feed the previous result to continue. */
uint32_t radio_console_proto_crc32(uint32_t crc, const uint8_t *data, size_t len);

/**
 * `radio-fw load <size> <crc32 hex>` (argv[0] "radio-fw", argv[1] "load"). ESP_ERR_INVALID_ARG for a wrong count or an
 * argument that is not a number; ESP_ERR_INVALID_SIZE for size 0 or above RADIO_CONSOLE_PROTO_MAX_IMAGE.
 */
esp_err_t radio_console_proto_parse_load(int argc, char **argv, size_t *size, uint32_t *crc);

typedef enum {
    RADIO_CONSOLE_PROTO_NONE,        // nothing complete yet
    RADIO_CONSOLE_PROTO_FRAME,       // a whole frame with a good CRC: offset, len, payload
    RADIO_CONSOLE_PROTO_ABORT,       // a frame of length 0 with a good CRC
    RADIO_CONSOLE_PROTO_NAK_CRC,     // a whole frame whose CRC does not match
    RADIO_CONSOLE_PROTO_NAK_LENGTH,  // a header with a length above RADIO_CONSOLE_PROTO_MAX_CHUNK
    RADIO_CONSOLE_PROTO_NAK_TIMEOUT, // a partial frame stalled (radio_console_proto_poll)
} radio_console_proto_event_t;

typedef struct {
    uint8_t buf[RADIO_CONSOLE_PROTO_HEADER_LEN + RADIO_CONSOLE_PROTO_MAX_CHUNK + RADIO_CONSOLE_PROTO_CRC_LEN];
    size_t have;      // bytes of the current frame, magic included
    uint32_t last_ms; // time of the last byte of the current frame
    /* valid after RADIO_CONSOLE_PROTO_FRAME */
    uint32_t offset;
    uint16_t len;
    const uint8_t *payload;
} radio_console_proto_parser_t;

void radio_console_proto_parser_init(radio_console_proto_parser_t *p);

/** One received byte. Bytes before the magic "RF" are skipped. */
radio_console_proto_event_t radio_console_proto_feed(radio_console_proto_parser_t *p, uint8_t byte, uint32_t now_ms);

/** Called whenever a read returns (with or without bytes), before its bytes are fed: reports a stalled partial frame. */
radio_console_proto_event_t radio_console_proto_poll(radio_console_proto_parser_t *p, uint32_t now_ms);

typedef enum {
    RADIO_CONSOLE_PROTO_WRITE,      // the next bytes: write them, *next = offset + len
    RADIO_CONSOLE_PROTO_REPEAT,     // a frame already taken (its ack was lost): *next = expected, nothing written
    RADIO_CONSOLE_PROTO_NAK_OFFSET, // ahead of expected, or past the image: *next = expected
} radio_console_proto_accept_t;

radio_console_proto_accept_t radio_console_proto_accept(size_t expected, size_t size, uint32_t offset, uint16_t len,
                                                        size_t *next);

/** What `radio-fw status` reports; computed from the last probe and the radio mux. */
typedef struct {
    bool maintenance;       // radio_mux_maintenance_running (a push, a recovery, the boot steps)
    bool updater_reachable; // last probe
    bool esp_hosted;        // last probe
    bool failed;            // last probe ended with an error (last_error set, or a ROM recovery failed)
} radio_console_proto_state_input_t;

/** "busy", "updater", "esp_hosted", "fault" or "probing" (no probe result yet). */
const char *radio_console_proto_state(const radio_console_proto_state_input_t *in);

/**
 * One reply line: "@radio-fw <kind>[ <fields>]\n". Newlines and carriage returns inside fields become spaces; fields
 * too long for cap are cut, the "\n" always stays. Returns the length written (without NUL), 0 if cap cannot hold even
 * the kind.
 */
size_t radio_console_proto_line(char *buf, size_t cap, const char *kind, const char *fields);

/** "@radio-fw status state=<s> installed=<v|-> base=<v|-> jumper=<0|1>\n"; an empty or NULL version is "-". */
size_t radio_console_proto_status_line(char *buf, size_t cap, const char *state, const char *installed,
                                       const char *base, bool jumper);

/**
 * The end of a push: "@radio-fw done installed=<v|->\n" for ESP_OK, else "@radio-fw error <code> <detail>\n" (code from
 * the HTTP API's mapping, detail falling back to esp_err_to_name(err) when empty).
 */
size_t radio_console_proto_result_line(char *buf, size_t cap, esp_err_t err, const char *code, const char *detail,
                                       const char *installed);

#ifdef __cplusplus
}
#endif
