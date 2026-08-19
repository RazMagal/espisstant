/*
 * Philips Hue Bridge client — local v1 REST API over HTTP.
 *
 * The v1 API is served by every bridge generation, including the original
 * round bridge, so this works without cloud accounts or the v2 CLIP API.
 */
#ifndef HUE_H
#define HUE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUE_USERNAME_MAX 48

/* Find a bridge via SSDP (works for v1 and v2 bridges). Blocks up to
 * ~3 s. Writes the dotted-quad IP on success. */
esp_err_t hue_discover(char *ip_out, size_t cap);

/* Create an application key. The bridge's physical link button must have
 * been pressed within the last 30 s, otherwise ESP_ERR_INVALID_STATE is
 * returned (error 101). */
esp_err_t hue_pair(const char *bridge_ip, char *username_out, size_t cap);

/* Group 0 is the built-in "all lights" group. */
esp_err_t hue_set_group(const char *bridge_ip, const char *username,
                        int group, bool on);

esp_err_t hue_set_light(const char *bridge_ip, const char *username,
                        int light, bool on);

/* Relative brightness change, delta in -254..254 (also turns the group on
 * when raising brightness). */
esp_err_t hue_group_dim(const char *bridge_ip, const char *username,
                        int group, int delta);

/* Quick reachability / auth check: GET /api/<user>/config. */
esp_err_t hue_check(const char *bridge_ip, const char *username);

#ifdef __cplusplus
}
#endif

#endif /* HUE_H */
