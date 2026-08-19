/* Philips Hue Bridge local API client — see hue.h. */
#include "hue.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

static const char *TAG = "hue";

#define HTTP_TIMEOUT_MS 4000
#define BODY_MAX 2048

/* --- small HTTP helper ------------------------------------------------ */

typedef struct {
    char body[BODY_MAX];
    int len;
} http_buf_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        http_buf_t *buf = evt->user_data;
        int space = BODY_MAX - 1 - buf->len;
        int copy = evt->data_len < space ? evt->data_len : space;
        if (copy > 0) {
            memcpy(buf->body + buf->len, evt->data, copy);
            buf->len += copy;
            buf->body[buf->len] = '\0';
        }
    }
    return ESP_OK;
}

/* Perform a request with optional JSON body; response lands in `buf`. */
static esp_err_t request(const char *ip, const char *path,
                         esp_http_client_method_t method, const char *json,
                         http_buf_t *buf)
{
    char url[160];
    snprintf(url, sizeof(url), "http://%s%s", ip, path);

    buf->len = 0;
    buf->body[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = on_http_event,
        .user_data = buf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    if (json) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, json, strlen(json));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s: %s", path, json ? json : "", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "%s -> HTTP %d", path, status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* The v1 API answers 200 even for logical errors; errors come back as
 * [{"error":{"type":N,...}}]. Returns the error type, 0 if none, or -1
 * when the body is not parseable JSON (treated as failure — never assume
 * success from a response we did not understand). */
static int api_error_type(const char *body)
{
    int type = 0;
    cJSON *root = cJSON_Parse(body);
    if (!root) return -1;
    cJSON *first = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : root;
    cJSON *error = cJSON_GetObjectItem(first, "error");
    cJSON *t = cJSON_GetObjectItem(error, "type");
    if (cJSON_IsNumber(t)) type = t->valueint;
    cJSON_Delete(root);
    return type;
}

/* --- public API ------------------------------------------------------- */

esp_err_t hue_pair(const char *bridge_ip, char *username_out, size_t cap)
{
    http_buf_t buf;
    esp_err_t err = request(bridge_ip, "/api", HTTP_METHOD_POST,
                            "{\"devicetype\":\"espisstant#esp32\"}", &buf);
    if (err != ESP_OK) return err;

    int etype = api_error_type(buf.body);
    if (etype == 101) {
        ESP_LOGW(TAG, "link button not pressed");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_FAIL;
    cJSON *root = cJSON_Parse(buf.body);
    cJSON *first = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : NULL;
    cJSON *success = cJSON_GetObjectItem(first, "success");
    cJSON *user = cJSON_GetObjectItem(success, "username");
    if (cJSON_IsString(user)) {
        strlcpy(username_out, user->valuestring, cap);
        ESP_LOGI(TAG, "paired with bridge %s", bridge_ip);
        result = ESP_OK;
    }
    cJSON_Delete(root);
    return result;
}

static esp_err_t put_state(const char *ip, const char *user, const char *path,
                           const char *json)
{
    char full[128];
    snprintf(full, sizeof(full), "/api/%s%s", user, path);
    http_buf_t buf;
    esp_err_t err = request(ip, full, HTTP_METHOD_PUT, json, &buf);
    if (err != ESP_OK) return err;
    int etype = api_error_type(buf.body);
    if (etype != 0) {
        ESP_LOGW(TAG, "bridge error %d for %s %s", etype, path, json);
        return etype == 1 ? ESP_ERR_INVALID_STATE /* unauthorized */
                          : ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t hue_set_group(const char *bridge_ip, const char *username,
                        int group, bool on)
{
    char path[48];
    snprintf(path, sizeof(path), "/groups/%d/action", group);
    return put_state(bridge_ip, username, path,
                     on ? "{\"on\":true}" : "{\"on\":false}");
}

esp_err_t hue_set_light(const char *bridge_ip, const char *username,
                        int light, bool on)
{
    char path[48];
    snprintf(path, sizeof(path), "/lights/%d/state", light);
    return put_state(bridge_ip, username, path,
                     on ? "{\"on\":true}" : "{\"on\":false}");
}

esp_err_t hue_group_dim(const char *bridge_ip, const char *username,
                        int group, int delta)
{
    char path[48], json[64];
    snprintf(path, sizeof(path), "/groups/%d/action", group);
    if (delta > 0)
        snprintf(json, sizeof(json), "{\"on\":true,\"bri_inc\":%d}", delta);
    else
        snprintf(json, sizeof(json), "{\"bri_inc\":%d}", delta);
    return put_state(bridge_ip, username, path, json);
}

esp_err_t hue_check(const char *bridge_ip, const char *username)
{
    char path[80];
    snprintf(path, sizeof(path), "/api/%s/config", username);
    http_buf_t buf;
    esp_err_t err = request(bridge_ip, path, HTTP_METHOD_GET, NULL, &buf);
    if (err != ESP_OK) return err;
    return api_error_type(buf.body) == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* --- SSDP discovery --------------------------------------------------- */

esp_err_t hue_discover(char *ip_out, size_t cap)
{
    static const char msearch[] =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: ssdp:all\r\n\r\n";

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) return ESP_FAIL;

    struct timeval tv = { .tv_sec = 3 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(1900),
    };
    inet_pton(AF_INET, "239.255.255.250", &dest.sin_addr);

    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (sendto(sock, msearch, sizeof(msearch) - 1, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        close(sock);
        return ESP_FAIL;
    }

    /* Wall-clock deadline: the 3 s receive timeout restarts on every
     * datagram, which on a chatty network could block us far longer. */
    int64_t deadline = esp_timer_get_time() + 3 * 1000000LL;
    char buf[512];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n;
    while (esp_timer_get_time() < deadline &&
           (n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fromlen)) > 0) {
        buf[n] = '\0';
        /* All Hue bridge generations announce "IpBridge" in the SERVER
         * header; newer firmware also sends "hue-bridgeid". */
        if (strstr(buf, "IpBridge") || strstr(buf, "hue-bridgeid")) {
            inet_ntop(AF_INET, &from.sin_addr, ip_out, cap);
            ESP_LOGI(TAG, "found hue bridge at %s", ip_out);
            err = ESP_OK;
            break;
        }
        fromlen = sizeof(from);
    }
    close(sock);
    return err;
}
