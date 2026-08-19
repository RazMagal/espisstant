/* Persistent app settings, backed by NVS. */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ip[16];
    char id[7];   /* 6-hex-char switcher device id */
    char key[3];  /* 2-hex-char device key (type-1 login) */
} sw_slot_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];

    char hue_ip[16];
    char hue_user[48];
    int hue_group; /* 0 = all lights */

    /* When set, every state-changing API request must carry it in an
     * X-Api-Key header. Empty = open (LAN-trusted) mode. */
    char api_token[33];

    /* Switcher role slots, auto-filled by discovery, editable in the UI. */
    sw_slot_t heater;
    sw_slot_t plug;
    sw_slot_t breeze;
} settings_t;

/* Load from NVS (called once at boot, after nvs_flash_init). */
void settings_init(void);

/* The live settings singleton. Mutate + settings_save(). Writes happen
 * only from the web-UI/discovery paths; concurrent readers tolerate a
 * torn string at worst, which the next retry fixes. */
settings_t *settings(void);

esp_err_t settings_save(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
