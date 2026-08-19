/*
 * Switcher local-protocol packet builder — see switcher_proto.h.
 *
 * The hex templates below mirror aioswitcher's packets.py exactly; the
 * host test in tools/test_switcher_proto.py diffs every builder against
 * that reference, so any edit here that changes bytes will fail the test.
 */
#include "switcher_proto.h"

#include <stdio.h>
#include <string.h>

/* Shared template fragments. {s} = session, {t} = timestamp (8 hex,
 * little-endian), then a fixed suffix. */
#define REQ_MID1 "340001000000000000000000"  /* type-1 requests */
#define REQ_MID2 "390001000000000000000000"  /* type-2 get-state */
#define REQ_MIDB "000001000000000000000000"  /* breeze commands  */
#define REQ_TAIL "00000000000000000000f0fe"
#define PAD_72_ZEROS                                                    \
    "000000000000000000000000000000000000000000000000000000000000000000" \
    "000000"

uint16_t sw_crc16(const uint8_t *data, size_t len, uint16_t crc)
{
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int sw_hex_to_bin(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex);
    if (n % 2 != 0 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n; i += 2) {
        int hi = hex_nibble(hex[i]), lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}

void sw_hex32_le(uint32_t v, char out[9])
{
    snprintf(out, 9, "%02x%02x%02x%02x", (unsigned)(v & 0xff),
             (unsigned)((v >> 8) & 0xff), (unsigned)((v >> 16) & 0xff),
             (unsigned)((v >> 24) & 0xff));
}

/* "%x" of the byte length, left-justified and padded with '0' to 4 chars
 * (the wire format really is left-justified — a 0x52-byte packet gets
 * "5200"). Mirrors aioswitcher's set_message_length()/command length. */
static void hex_len_ljust4(size_t nbytes, char out[5])
{
    snprintf(out, 5, "%x", (unsigned)nbytes);
    for (size_t i = strlen(out); i < 4; i++) out[i] = '0';
    out[4] = '\0';
}

int sw_frame_and_sign(char *p, size_t cap)
{
    size_t n = strlen(p);
    if (n < 8 || n % 2 != 0 || n + 8 + 1 > cap) return -1;

    /* Length field counts the packet plus the 4 signature bytes. */
    char lenfield[5];
    hex_len_ljust4(n / 2 + 4, lenfield);
    memcpy(p + 4, lenfield, 4);

    /* First CRC pass: over the packet itself. Streamed nibble-by-nibble to
     * keep the stack small (breeze IR packets can be ~1 KB of hex). */
    uint16_t crc = 0x1021;
    for (size_t i = 0; i < n; i += 2) {
        int hi = hex_nibble(p[i]), lo = hex_nibble(p[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        uint8_t byte = (uint8_t)((hi << 4) | lo);
        crc = sw_crc16(&byte, 1, crc);
    }

    /* Second pass: CRC (little-endian) + 32 bytes of ASCII '0'. */
    uint8_t key[34];
    key[0] = (uint8_t)(crc & 0xff);
    key[1] = (uint8_t)(crc >> 8);
    memset(key + 2, 0x30, 32);
    uint16_t crc2 = sw_crc16(key, sizeof(key), 0x1021);

    snprintf(p + n, 9, "%02x%02x%02x%02x", crc & 0xff, crc >> 8,
             crc2 & 0xff, crc2 >> 8);
    return (int)(n + 8);
}

/* Assemble from parts, then frame + sign. Returns final length or -1. */
static int build(char *out, size_t cap, const char *const parts[], size_t nparts)
{
    size_t pos = 0;
    for (size_t i = 0; i < nparts; i++) {
        size_t l = strlen(parts[i]);
        if (pos + l + 1 > cap) return -1;
        memcpy(out + pos, parts[i], l + 1);
        pos += l;
    }
    return sw_frame_and_sign(out, cap);
}

int sw_packet_login1(char *out, size_t cap, uint32_t timestamp,
                     const char *device_key)
{
    char ts[9];
    sw_hex32_le(timestamp, ts);
    const char *parts[] = { "fef052000232a100", SW_NO_SESSION, REQ_MID1, ts,
                            REQ_TAIL, device_key, PAD_72_ZEROS, "00" };
    return build(out, cap, parts, 8);
}

int sw_packet_login2(char *out, size_t cap, uint32_t timestamp,
                     const char *device_id)
{
    char ts[9];
    sw_hex32_le(timestamp, ts);
    const char *parts[] = { "fef030000305a600", SW_NO_SESSION,
                            "ff0301000000", "0000", "00000000", ts, REQ_TAIL,
                            device_id, "00" };
    return build(out, cap, parts, 9);
}

int sw_packet_get_state1(char *out, size_t cap, const char *session,
                         uint32_t timestamp, const char *device_id)
{
    char ts[9];
    sw_hex32_le(timestamp, ts);
    const char *parts[] = { "fef0300002320103", session, REQ_MID1, ts,
                            REQ_TAIL, device_id, "00" };
    return build(out, cap, parts, 7);
}

int sw_packet_get_state2(char *out, size_t cap, const char *session,
                         uint32_t timestamp, const char *device_id)
{
    char ts[9];
    sw_hex32_le(timestamp, ts);
    const char *parts[] = { "fef0300003050103", session, REQ_MID2, ts,
                            REQ_TAIL, device_id, "00" };
    return build(out, cap, parts, 7);
}

int sw_packet_control1(char *out, size_t cap, const char *session,
                       uint32_t timestamp, const char *device_id,
                       bool on, uint32_t timer_minutes)
{
    char ts[9], timer[9];
    sw_hex32_le(timestamp, ts);
    if (timer_minutes > 0)
        sw_hex32_le(timer_minutes * 60, timer);
    else
        strcpy(timer, "00000000");

    const char *parts[] = { "fef05d0002320102", session, REQ_MID1, ts,
                            REQ_TAIL, device_id, PAD_72_ZEROS,
                            "000106000", on ? "1" : "0", "00", timer };
    return build(out, cap, parts, 11);
}

int sw_packet_breeze_command(char *out, size_t cap, const char *session,
                             uint32_t timestamp, const char *device_id,
                             const char *command_hex)
{
    char ts[9], cmdlen[5];
    sw_hex32_le(timestamp, ts);
    hex_len_ljust4(strlen(command_hex) / 2, cmdlen);

    const char *parts[] = { "fef0000003050102", session, REQ_MIDB, ts,
                            REQ_TAIL, device_id, PAD_72_ZEROS, "3701",
                            cmdlen, command_hex };

    /* Breeze packets can exceed SW_PACKET_HEX_MAX; assemble by hand with
     * the caller's cap, then reuse the common frame+sign path. */
    size_t pos = 0;
    for (size_t i = 0; i < 10; i++) {
        size_t l = strlen(parts[i]);
        if (pos + l + 1 > cap) return -1;
        memcpy(out + pos, parts[i], l + 1);
        pos += l;
    }
    return sw_frame_and_sign(out, cap);
}

/* --- response parsing ------------------------------------------------ */

int sw_parse_session(const uint8_t *resp, size_t len,
                     char session[SW_SESSION_LEN + 1])
{
    if (len < 12) return -1;
    snprintf(session, SW_SESSION_LEN + 1, "%02x%02x%02x%02x", resp[8],
             resp[9], resp[10], resp[11]);
    return 0;
}

int sw_parse_state1(const uint8_t *resp, size_t len, bool *on,
                    uint32_t *power_w)
{
    if (len < 81) return -1;
    if (on) *on = resp[75] == 0x01;
    if (power_w) *power_w = ((uint32_t)resp[78] << 8) | resp[77];
    return 0;
}

int sw_parse_breeze_state(const uint8_t *resp, size_t len,
                          sw_breeze_state_t *st)
{
    if (len < 92) return -1;
    memset(st, 0, sizeof(*st));
    st->on = resp[78] != 0x00;
    st->mode = resp[79];
    st->temperature = (float)(((uint32_t)resp[77] << 8) | resp[76]) / 10.0f;
    st->target_temp = resp[80];
    st->fan_level = (char)('0' + (resp[81] >> 4));
    st->swing = (char)('0' + (resp[81] & 0x0f));
    memcpy(st->remote_id, &resp[84], 8);
    st->remote_id[8] = '\0'; /* NUL-padded on the wire */
    return 0;
}

int sw_parse_broadcast(const uint8_t *msg, size_t len, sw_discovered_t *dev)
{
    if (len != 165 && len != 168) return -1;
    if (msg[0] != 0xfe || msg[1] != 0xf0) return -1;

    memset(dev, 0, sizeof(*dev));
    dev->is_type2 = (len == 168);

    snprintf(dev->device_id, sizeof(dev->device_id), "%02x%02x%02x", msg[18],
             msg[19], msg[20]);
    memcpy(dev->name, &msg[42], 32);
    dev->name[32] = '\0';
    snprintf(dev->type_hex, sizeof(dev->type_hex), "%02x%02x", msg[74],
             msg[75]);
    snprintf(dev->device_key, sizeof(dev->device_key), "%02x", msg[40]);

    if (dev->is_type2) {
        snprintf(dev->ip, sizeof(dev->ip), "%u.%u.%u.%u", msg[77], msg[78],
                 msg[79], msg[80]);
    } else {
        snprintf(dev->ip, sizeof(dev->ip), "%u.%u.%u.%u", msg[76], msg[77],
                 msg[78], msg[79]);
        dev->on = msg[133] == 0x01;
        dev->power_w = ((uint32_t)msg[136] << 8) | msg[135];
    }
    return 0;
}
