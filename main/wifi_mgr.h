/* Wi-Fi manager: STA when provisioned, SoftAP setup portal otherwise. */
#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Blocks until either STA got an IP or the fallback AP is up. */
void wifi_mgr_start(void);

bool wifi_in_ap_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MGR_H */
