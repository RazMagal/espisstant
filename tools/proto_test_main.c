/*
 * Host-side test driver for switcher_proto.c.
 * Invoked by tools/test_switcher_proto.py; prints one packet or one parsed
 * record per invocation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "switcher_proto.h"

static uint8_t g_buf[4096];

static int unhex_arg(const char *hex)
{
    return sw_hex_to_bin(hex, g_buf, sizeof(g_buf));
}

int main(int argc, char **argv)
{
    static char out[8192];
    if (argc < 2) return 2;
    const char *cmd = argv[1];
    int len = -1;

    if (strcmp(cmd, "login1") == 0 && argc == 4) {
        len = sw_packet_login1(out, sizeof(out),
                               (uint32_t)strtoul(argv[2], NULL, 10), argv[3]);
    } else if (strcmp(cmd, "login2") == 0 && argc == 4) {
        len = sw_packet_login2(out, sizeof(out),
                               (uint32_t)strtoul(argv[2], NULL, 10), argv[3]);
    } else if (strcmp(cmd, "state1") == 0 && argc == 5) {
        len = sw_packet_get_state1(out, sizeof(out), argv[2],
                                   (uint32_t)strtoul(argv[3], NULL, 10),
                                   argv[4]);
    } else if (strcmp(cmd, "state2") == 0 && argc == 5) {
        len = sw_packet_get_state2(out, sizeof(out), argv[2],
                                   (uint32_t)strtoul(argv[3], NULL, 10),
                                   argv[4]);
    } else if (strcmp(cmd, "control1") == 0 && argc == 7) {
        len = sw_packet_control1(out, sizeof(out), argv[2],
                                 (uint32_t)strtoul(argv[3], NULL, 10),
                                 argv[4], atoi(argv[5]) != 0,
                                 (uint32_t)strtoul(argv[6], NULL, 10));
    } else if (strcmp(cmd, "breeze") == 0 && argc == 6) {
        len = sw_packet_breeze_command(out, sizeof(out), argv[2],
                                       (uint32_t)strtoul(argv[3], NULL, 10),
                                       argv[4], argv[5]);
    } else if (strcmp(cmd, "parse_session") == 0 && argc == 3) {
        int n = unhex_arg(argv[2]);
        char session[SW_SESSION_LEN + 1];
        if (n < 0 || sw_parse_session(g_buf, n, session) != 0) return 1;
        printf("%s\n", session);
        return 0;
    } else if (strcmp(cmd, "parse_state1") == 0 && argc == 3) {
        int n = unhex_arg(argv[2]);
        bool on;
        uint32_t power;
        if (n < 0 || sw_parse_state1(g_buf, n, &on, &power) != 0) return 1;
        printf("%d %u\n", on ? 1 : 0, (unsigned)power);
        return 0;
    } else if (strcmp(cmd, "parse_breeze") == 0 && argc == 3) {
        int n = unhex_arg(argv[2]);
        sw_breeze_state_t st;
        if (n < 0 || sw_parse_breeze_state(g_buf, n, &st) != 0) return 1;
        printf("%d %d %.1f %d %c %c %s\n", st.on ? 1 : 0, st.mode,
               st.temperature, st.target_temp, st.fan_level, st.swing,
               st.remote_id);
        return 0;
    } else if (strcmp(cmd, "parse_broadcast") == 0 && argc == 3) {
        int n = unhex_arg(argv[2]);
        sw_discovered_t dev;
        if (n < 0 || sw_parse_broadcast(g_buf, n, &dev) != 0) return 1;
        printf("%s %s %s %s %s %d %d %u\n", dev.device_id, dev.device_key,
               dev.type_hex, dev.ip, dev.name, dev.is_type2 ? 1 : 0,
               dev.on ? 1 : 0, (unsigned)dev.power_w);
        return 0;
    } else {
        fprintf(stderr, "usage: %s <cmd> <args...>\n", argv[0]);
        return 2;
    }

    if (len < 0) return 1;
    printf("%s\n", out);
    return 0;
}
