/*
 * Switcher local-protocol packet builder.
 *
 * Pure C99, no ESP-IDF dependencies — compiled both into the firmware and
 * into the host-side test binary (tools/test_switcher_proto.py), which
 * verifies every builder byte-for-byte against the aioswitcher Python
 * reference implementation.
 *
 * Protocol knowledge derived from https://github.com/TomerFi/aioswitcher
 * (Apache-2.0).
 *
 * All builders write a NUL-terminated lowercase hex string to `out` and
 * return its length in characters, or -1 if `cap` is too small. The result
 * is a complete wire packet: framed ("fef0" + length), then CRC-signed.
 * Send it with sw_hex_to_bin().
 */
#ifndef SWITCHER_PROTO_H
#define SWITCHER_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SW_TCP_PORT_TYPE1 9957   /* heater / plug ("type 1") */
#define SW_TCP_PORT_TYPE2 10000  /* breeze / runner ("type 2") */
#define SW_UDP_PORT_TYPE1 20002  /* discovery broadcasts, type 1 */
#define SW_UDP_PORT_TYPE2 20003  /* discovery broadcasts, type 2 */

/* Device id is 6 hex chars, device key 2, session id 8. */
#define SW_DEVICE_ID_LEN 6
#define SW_DEVICE_KEY_LEN 2
#define SW_SESSION_LEN 8

/* Initial session id used before login. */
#define SW_NO_SESSION "00000000"

/* Big enough for every fixed-size packet (largest is control1 at ~250
 * chars + framing + signature). Breeze IR packets are unbounded — their
 * builder takes its own cap. */
#define SW_PACKET_HEX_MAX 512

/* --- low-level helpers (exposed for tests) --------------------------- */

/* CRC-16/CCITT (poly 0x1021, MSB first), equivalent to binascii.crc_hqx. */
uint16_t sw_crc16(const uint8_t *data, size_t len, uint16_t crc);

/* Rewrite the length field of `hex_packet` (chars [4:8]) from its current
 * length + 4 signature bytes, then append the 8-hex-char double-CRC
 * signature. Needs room for 8 more chars + NUL. Returns new length or -1. */
int sw_frame_and_sign(char *hex_packet, size_t cap);

/* Little-endian hex of a 32-bit value (8 chars + NUL), e.g. timestamps
 * and timer seconds. */
void sw_hex32_le(uint32_t v, char out[9]);

/* Hex string -> bytes. Returns byte count or -1 on bad input/short buf. */
int sw_hex_to_bin(const char *hex, uint8_t *out, size_t cap);

/* --- packet builders ------------------------------------------------- */

/* Type-1 (heater/plug) login. `device_key` is the 2-hex-char key from the
 * discovery broadcast (offset 40 of the datagram). */
int sw_packet_login1(char *out, size_t cap, uint32_t timestamp,
                     const char *device_key);

/* Type-2 (breeze) login. */
int sw_packet_login2(char *out, size_t cap, uint32_t timestamp,
                     const char *device_id);

/* Get-state, type 1 / type 2. `session` is the 8-hex-char session id
 * parsed from the login response (sw_parse_session). */
int sw_packet_get_state1(char *out, size_t cap, const char *session,
                         uint32_t timestamp, const char *device_id);
int sw_packet_get_state2(char *out, size_t cap, const char *session,
                         uint32_t timestamp, const char *device_id);

/* On/off (+ optional off-timer in minutes, 0 = none), type 1. */
int sw_packet_control1(char *out, size_t cap, const char *session,
                       uint32_t timestamp, const char *device_id,
                       bool on, uint32_t timer_minutes);

/* Breeze IR command. `command_hex` is "00000000" + hex-encoded
 * "<Para>|<HexCode>" string from the remote's IRSet (see breeze_remote.c);
 * the length field is derived from it. */
int sw_packet_breeze_command(char *out, size_t cap, const char *session,
                             uint32_t timestamp, const char *device_id,
                             const char *command_hex);

/* --- response parsers ------------------------------------------------ */

/* Session id from a login response. Returns 0 on success. */
int sw_parse_session(const uint8_t *resp, size_t len,
                     char session[SW_SESSION_LEN + 1]);

/* Type-1 state response: on/off + current power draw in watts. */
int sw_parse_state1(const uint8_t *resp, size_t len, bool *on,
                    uint32_t *power_w);

/* Breeze state response. */
typedef struct {
    bool on;
    uint8_t mode;        /* 1 auto, 2 dry, 3 fan, 4 cool, 5 heat */
    float temperature;   /* measured, °C */
    uint8_t target_temp; /* °C */
    char fan_level;      /* '0' auto, '1' low, '2' med, '3' high */
    char swing;          /* '0' off, '1' on */
    char remote_id[9];   /* e.g. "ELEC7001" */
} sw_breeze_state_t;

int sw_parse_breeze_state(const uint8_t *resp, size_t len,
                          sw_breeze_state_t *state);

/* --- discovery datagram parser --------------------------------------- */

typedef struct {
    char device_id[SW_DEVICE_ID_LEN + 1];
    char device_key[SW_DEVICE_KEY_LEN + 1];
    char name[33];
    char type_hex[5];    /* model code, e.g. "0317" = V4, "01a8" = plug */
    char ip[16];         /* dotted quad */
    bool is_type2;       /* true: breeze/runner (TCP :10002) */
    bool on;             /* type-1 only */
    uint32_t power_w;    /* type-1 only */
} sw_discovered_t;

/* Parse a UDP broadcast datagram (165 bytes = type 1, 168 = type 2).
 * Returns 0 on success, -1 if the datagram is not a Switcher broadcast. */
int sw_parse_broadcast(const uint8_t *msg, size_t len, sw_discovered_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* SWITCHER_PROTO_H */
