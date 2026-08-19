/*
 * Switcher device control over the LAN (ESP-IDF side).
 *
 * Type-1 devices (water heater / smart heater line, power plug) speak on
 * TCP :9957; the Breeze on TCP :10002. Discovery is passive: devices
 * broadcast a datagram every ~4 s on UDP :20002/:20003.
 */
#ifndef SWITCHER_H
#define SWITCHER_H

#include "breeze_remote.h"
#include "esp_err.h"
#include "switcher_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the UDP discovery listeners. `cb` fires (from the listener task)
 * for every broadcast heard; devices repeat them every few seconds. */
typedef void (*switcher_found_cb_t)(const sw_discovered_t *dev, void *arg);
esp_err_t switcher_start_discovery(switcher_found_cb_t cb, void *arg);

/* Turn a type-1 device (heater/plug) on or off. `timer_minutes` > 0 arms
 * the auto-off timer when turning on. */
esp_err_t switcher_turn(const char *ip, const char *device_id,
                        const char *device_key, bool on,
                        uint32_t timer_minutes);

/* Query a type-1 device's state. */
esp_err_t switcher_get_state(const char *ip, const char *device_id,
                             const char *device_key, bool *on,
                             uint32_t *power_w);

/* Query the Breeze thermostat state (also yields its remote ID). */
esp_err_t switcher_breeze_get_state(const char *ip, const char *device_id,
                                    sw_breeze_state_t *state);

/* Drive the Breeze to the requested state. Requires a loaded remote set
 * (br_load). Reads the current state first — toggle-type remotes need it. */
esp_err_t switcher_breeze_set(const char *ip, const char *device_id,
                              const br_request_t *req);

#ifdef __cplusplus
}
#endif

#endif /* SWITCHER_H */
