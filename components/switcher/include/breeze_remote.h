/*
 * Switcher Breeze IR remote handling.
 *
 * The Breeze is an IR blaster: to control the AC it replays IR wave codes
 * from a "remote set" (IRSet) matching the AC's original remote. This
 * module loads one remote's IRSet JSON (fetched onto the storage partition
 * by tools/fetch_breeze_remote.py) and builds command payloads for
 * sw_packet_breeze_command().
 *
 * Key-building logic is a port of aioswitcher's SwitcherBreezeRemote
 * (Apache-2.0).
 */
#ifndef BREEZE_REMOTE_H
#define BREEZE_REMOTE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BR_MODE_AUTO = 1,
    BR_MODE_DRY = 2,
    BR_MODE_FAN = 3,
    BR_MODE_COOL = 4,
    BR_MODE_HEAT = 5,
} br_mode_t;

typedef struct {
    bool on;
    br_mode_t mode;
    int target_temp;  /* °C, clamped to the remote's range */
    char fan_level;   /* '0' auto, '1' low, '2' medium, '3' high */
    bool swing;
} br_request_t;

/* Load the IRSet JSON file (e.g. "/spiffs/breeze_remote.json").
 * Returns 0 on success. Safe to call again to reload. */
int br_load(const char *path);

bool br_is_loaded(void);
const char *br_remote_id(void);

/* Build the command payload ("00000000" + hex of "Para|HexCode") for the
 * desired state. `current_on` is the AC's present state, needed for
 * toggle-type remotes. Returns payload length in chars or -1. */
int br_build_command(const br_request_t *req, bool current_on, char *out,
                     size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* BREEZE_REMOTE_H */
