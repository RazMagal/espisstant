/* Switcher LAN control — see switcher.h. */
#include "switcher.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "switcher";

#define RECV_TIMEOUT_S 5
#define RESP_MAX 1024

/* Breeze IR payloads can run to a couple of KB of hex. */
#define BREEZE_PACKET_MAX 4096

/* --- TCP transaction helpers ----------------------------------------- */

static int tcp_connect(const char *ip, uint16_t port)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return -1;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;

    struct timeval tv = { .tv_sec = RECV_TIMEOUT_S };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "connect %s:%u failed: errno %d", ip, port, errno);
        close(sock);
        return -1;
    }
    return sock;
}

/* Send a signed hex packet, then read a response of at least `min_resp`
 * bytes (responses may arrive split across TCP segments). Returns bytes
 * read or -1. */
static int txn(int sock, const char *hex_packet, uint8_t *resp, size_t cap,
               size_t min_resp)
{
    size_t nbytes = strlen(hex_packet) / 2;
    uint8_t *bin = malloc(nbytes);
    if (!bin) return -1;
    int n = sw_hex_to_bin(hex_packet, bin, nbytes);
    bool sent = n >= 0 && send(sock, bin, n, 0) == n;
    free(bin);
    if (!sent) return -1;

    size_t total = 0;
    while (total < min_resp) {
        int r = recv(sock, resp + total, cap - total, 0);
        if (r <= 0) break; /* timeout or peer closed */
        total += r;
    }
    return total >= min_resp ? (int)total : -1;
}

/* Login on an open socket; fills `session`. Type 1 wants the device key,
 * type 2 the device id. */
static int login(int sock, bool type2, const char *device_id,
                 const char *device_key, uint32_t ts,
                 char session[SW_SESSION_LEN + 1])
{
    char pkt[SW_PACKET_HEX_MAX];
    int len = type2 ? sw_packet_login2(pkt, sizeof(pkt), ts, device_id)
                    : sw_packet_login1(pkt, sizeof(pkt), ts, device_key);
    if (len < 0) return -1;

    uint8_t resp[RESP_MAX];
    int r = txn(sock, pkt, resp, sizeof(resp), 12);
    if (r < 0 || sw_parse_session(resp, r, session) != 0) {
        ESP_LOGW(TAG, "login failed (device %s)", device_id);
        return -1;
    }
    return 0;
}

/* --- public API ------------------------------------------------------- */

esp_err_t switcher_turn(const char *ip, const char *device_id,
                        const char *device_key, bool on,
                        uint32_t timer_minutes)
{
    int sock = tcp_connect(ip, SW_TCP_PORT_TYPE1);
    if (sock < 0) return ESP_FAIL;

    esp_err_t err = ESP_FAIL;
    uint32_t ts = (uint32_t)time(NULL);
    char session[SW_SESSION_LEN + 1];
    char pkt[SW_PACKET_HEX_MAX];
    uint8_t resp[RESP_MAX];

    if (login(sock, false, device_id, device_key, ts, session) == 0 &&
        sw_packet_control1(pkt, sizeof(pkt), session, ts, device_id, on,
                           timer_minutes) > 0 &&
        txn(sock, pkt, resp, sizeof(resp), 1) > 0) {
        ESP_LOGI(TAG, "%s -> %s", device_id, on ? "ON" : "OFF");
        err = ESP_OK;
    }
    close(sock);
    return err;
}

esp_err_t switcher_get_state(const char *ip, const char *device_id,
                             const char *device_key, bool *on,
                             uint32_t *power_w)
{
    int sock = tcp_connect(ip, SW_TCP_PORT_TYPE1);
    if (sock < 0) return ESP_FAIL;

    esp_err_t err = ESP_FAIL;
    uint32_t ts = (uint32_t)time(NULL);
    char session[SW_SESSION_LEN + 1];
    char pkt[SW_PACKET_HEX_MAX];
    uint8_t resp[RESP_MAX];
    int r;

    if (login(sock, false, device_id, device_key, ts, session) == 0 &&
        sw_packet_get_state1(pkt, sizeof(pkt), session, ts, device_id) > 0 &&
        (r = txn(sock, pkt, resp, sizeof(resp), 81)) > 0 &&
        sw_parse_state1(resp, r, on, power_w) == 0) {
        err = ESP_OK;
    }
    close(sock);
    return err;
}

static esp_err_t breeze_get_state_on(int sock, const char *device_id,
                                     uint32_t ts, const char *session,
                                     sw_breeze_state_t *state)
{
    char pkt[SW_PACKET_HEX_MAX];
    uint8_t resp[RESP_MAX];
    int r;
    if (sw_packet_get_state2(pkt, sizeof(pkt), session, ts, device_id) < 0 ||
        (r = txn(sock, pkt, resp, sizeof(resp), 92)) < 0 ||
        sw_parse_breeze_state(resp, r, state) != 0)
        return ESP_FAIL;
    return ESP_OK;
}

esp_err_t switcher_breeze_get_state(const char *ip, const char *device_id,
                                    sw_breeze_state_t *state)
{
    int sock = tcp_connect(ip, SW_TCP_PORT_TYPE2);
    if (sock < 0) return ESP_FAIL;

    esp_err_t err = ESP_FAIL;
    uint32_t ts = (uint32_t)time(NULL);
    char session[SW_SESSION_LEN + 1];

    if (login(sock, true, device_id, NULL, ts, session) == 0)
        err = breeze_get_state_on(sock, device_id, ts, session, state);
    close(sock);
    return err;
}

esp_err_t switcher_breeze_set(const char *ip, const char *device_id,
                              const br_request_t *req)
{
    if (!br_is_loaded()) {
        ESP_LOGE(TAG, "no breeze remote set loaded — run "
                      "tools/fetch_breeze_remote.py and reflash storage");
        return ESP_ERR_INVALID_STATE;
    }

    int sock = tcp_connect(ip, SW_TCP_PORT_TYPE2);
    if (sock < 0) return ESP_FAIL;

    esp_err_t err = ESP_FAIL;
    uint32_t ts = (uint32_t)time(NULL);
    char session[SW_SESSION_LEN + 1];
    sw_breeze_state_t cur;

    char *cmd = malloc(BREEZE_PACKET_MAX);
    char *pkt = malloc(BREEZE_PACKET_MAX);
    uint8_t resp[RESP_MAX];

    if (cmd && pkt &&
        login(sock, true, device_id, NULL, ts, session) == 0 &&
        breeze_get_state_on(sock, device_id, ts, session, &cur) == ESP_OK &&
        br_build_command(req, cur.on, cmd, BREEZE_PACKET_MAX / 2) > 0 &&
        sw_packet_breeze_command(pkt, BREEZE_PACKET_MAX, session, ts,
                                 device_id, cmd) > 0 &&
        txn(sock, pkt, resp, sizeof(resp), 1) > 0) {
        ESP_LOGI(TAG, "breeze %s -> %s mode=%d temp=%d", device_id,
                 req->on ? "ON" : "OFF", req->mode, req->target_temp);
        err = ESP_OK;
    }
    free(cmd);
    free(pkt);
    close(sock);
    return err;
}

/* --- discovery -------------------------------------------------------- */

typedef struct {
    uint16_t port;
    switcher_found_cb_t cb;
    void *arg;
} listener_ctx_t;

static void discovery_task(void *pv)
{
    listener_ctx_t *ctx = pv;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) goto out;

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ctx->port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind :%u failed, errno %d", ctx->port, errno);
        close(sock);
        goto out;
    }
    ESP_LOGI(TAG, "discovery listening on udp/%u", ctx->port);

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&from, &fromlen);
        if (len <= 0) continue;
        sw_discovered_t dev;
        if (sw_parse_broadcast(buf, len, &dev) != 0) continue;
        /* Anti-spoofing: the IP advertised inside the datagram must match
         * the sender — a forged broadcast could otherwise hijack a role
         * slot and receive the device credentials. */
        char src[16];
        inet_ntop(AF_INET, &from.sin_addr, src, sizeof(src));
        if (strcmp(src, dev.ip) != 0) {
            ESP_LOGW(TAG, "dropping broadcast claiming %s from %s", dev.ip,
                     src);
            continue;
        }
        ctx->cb(&dev, ctx->arg);
    }
out:
    free(ctx);
    vTaskDelete(NULL);
}

esp_err_t switcher_start_discovery(switcher_found_cb_t cb, void *arg)
{
    static const uint16_t ports[] = { SW_UDP_PORT_TYPE1, SW_UDP_PORT_TYPE2 };
    for (size_t i = 0; i < 2; i++) {
        listener_ctx_t *ctx = calloc(1, sizeof(*ctx));
        if (!ctx) return ESP_ERR_NO_MEM;
        ctx->port = ports[i];
        ctx->cb = cb;
        ctx->arg = arg;
        char name[16];
        snprintf(name, sizeof(name), "sw_disc_%u", ports[i]);
        if (xTaskCreate(discovery_task, name, 6144, ctx, 5, NULL) != pdPASS) {
            free(ctx);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}
