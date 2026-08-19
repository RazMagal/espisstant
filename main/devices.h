/* Switcher discovery cache + auto-assignment into settings role slots. */
#ifndef DEVICES_H
#define DEVICES_H

#include <stdint.h>

#include "switcher_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICES_SEEN_MAX 8

typedef struct {
    sw_discovered_t info;
    int64_t last_seen_us;
} seen_dev_t;

/* Create the cache lock. Must run before discovery or the web UI start. */
void devices_init(void);

/* Hook for switcher_start_discovery(). Updates the cache and fills empty
 * role slots (plug model -> plug, breeze -> breeze, other type-1 ->
 * heater); refreshes a known device's IP if it changed (DHCP). */
void devices_on_discovered(const sw_discovered_t *dev, void *arg);

/* Snapshot of recently heard devices. Returns count. */
int devices_seen(seen_dev_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEVICES_H */
