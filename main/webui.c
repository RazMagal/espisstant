/* Web UI + REST API — see webui.h. */
#include "webui.h"

#include <string.h>

#include "breeze_remote.h"
#include "cJSON.h"
#include "devices.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hue.h"
#include "intent.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "webui";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* --- helpers ---------------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return err;
}

static esp_err_t send_err(httpd_req_t *req, const char *status,
                          const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* If an API token is configured, state-changing requests must present it.
 * Sends the 401 itself and returns false on mismatch. */
static bool auth_ok(httpd_req_t *req)
{
    const char *token = settings()->api_token;
    if (token[0] == '\0') return true; /* open mode until a token is set */

    /* Fixed-width, NUL-padded, constant-time compare. */
    char want[33] = { 0 }, given[33] = { 0 };
    strncpy(want, token, sizeof(want) - 1);
    httpd_req_get_hdr_value_str(req, "X-Api-Key", given, sizeof(given));
    unsigned char diff = 0;
    for (size_t i = 0; i < sizeof(want); i++)
        diff |= (unsigned char)(want[i] ^ given[i]);
    if (diff == 0) return true;

    send_err(req, "401 Unauthorized", "missing or wrong X-Api-Key");
    return false;
}

/* Read + parse a small JSON request body. Caller must cJSON_Delete. On
 * failure this sends the error response itself and returns NULL.
 *
 * The Content-Type requirement doubles as CSRF protection: without it,
 * any web page could fire state-changing "simple" POSTs at
 * http://espisstant.local without a CORS preflight. Requiring
 * application/json forces a preflight, which fails (we send no CORS
 * headers). */
static cJSON *read_body(httpd_req_t *req)
{
    char ctype[40] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype));
    if (strncmp(ctype, "application/json", 16) != 0) {
        send_err(req, "415 Unsupported Media Type",
                 "send Content-Type: application/json");
        return NULL;
    }

    char buf[512];
    int total = req->content_len;
    if (total >= (int)sizeof(buf)) {
        send_err(req, "413 Payload Too Large", "body too large");
        return NULL;
    }
    if (total <= 0) return cJSON_Parse("{}");

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            send_err(req, "400 Bad Request", "truncated body");
            return NULL;
        }
        got += r;
    }
    buf[got] = '\0';
    cJSON *body = cJSON_Parse(buf);
    if (!body) send_err(req, "400 Bad Request", "invalid json");
    return body;
}

static void copy_str(cJSON *obj, const char *key, char *dst, size_t cap)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(v)) strlcpy(dst, v->valuestring, cap);
}

/* --- handlers --------------------------------------------------------- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* EMBED_TXTFILES appends a NUL terminator; don't send it. */
    return httpd_resp_send(req, index_html_start,
                           index_html_end - index_html_start - 1);
}

static cJSON *slot_json(const sw_slot_t *slot)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", slot->id);
    cJSON_AddStringToObject(o, "ip", slot->ip);
    return o;
}

static esp_err_t h_status(httpd_req_t *req)
{
    settings_t *cfg = settings();
    cJSON *root = cJSON_CreateObject();

    cJSON_AddBoolToObject(root, "ap_mode", wifi_in_ap_mode());
    cJSON_AddStringToObject(root, "hostname", "espisstant");
    cJSON_AddBoolToObject(root, "token_set", cfg->api_token[0] != '\0');
#if CONFIG_ESPISSTANT_VOICE
    cJSON_AddBoolToObject(root, "voice", true);
#else
    cJSON_AddBoolToObject(root, "voice", false);
#endif

    cJSON *hue = cJSON_CreateObject();
    cJSON_AddStringToObject(hue, "ip", cfg->hue_ip);
    cJSON_AddBoolToObject(hue, "paired", cfg->hue_user[0] != '\0');
    cJSON_AddNumberToObject(hue, "group", cfg->hue_group);
    cJSON_AddItemToObject(root, "hue", hue);

    cJSON_AddItemToObject(root, "heater", slot_json(&cfg->heater));
    cJSON_AddItemToObject(root, "plug", slot_json(&cfg->plug));
    cJSON_AddItemToObject(root, "breeze", slot_json(&cfg->breeze));
    cJSON_AddStringToObject(root, "breeze_remote", br_remote_id());

    seen_dev_t seen[DEVICES_SEEN_MAX];
    int n = devices_seen(seen, DEVICES_SEEN_MAX);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "id", seen[i].info.device_id);
        cJSON_AddStringToObject(d, "name", seen[i].info.name);
        cJSON_AddStringToObject(d, "type", seen[i].info.type_hex);
        cJSON_AddStringToObject(d, "ip", seen[i].info.ip);
        cJSON_AddItemToArray(arr, d);
    }
    cJSON_AddItemToObject(root, "seen", arr);

    return send_json(req, root);
}

static esp_err_t h_intent(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    cJSON *body = read_body(req);
    if (!body) return ESP_OK; /* error response already sent */
    cJSON *name = cJSON_GetObjectItem(body, "intent");
    intent_t intent = cJSON_IsString(name) ? intent_from_name(name->valuestring)
                                           : INTENT_MAX;
    cJSON_Delete(body);

    if (intent == INTENT_MAX)
        return send_err(req, "400 Bad Request", "unknown intent");
    if (intent_execute(intent) != ESP_OK)
        return send_err(req, "502 Bad Gateway", "device unreachable");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static esp_err_t h_hue_discover(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    settings_t *cfg = settings();
    char ip[16];
    if (hue_discover(ip, sizeof(ip)) != ESP_OK)
        return send_err(req, "404 Not Found", "no bridge answered");
    strlcpy(cfg->hue_ip, ip, sizeof(cfg->hue_ip));
    settings_save();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ip", ip);
    return send_json(req, root);
}

static esp_err_t h_hue_pair(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    settings_t *cfg = settings();
    cJSON *body = read_body(req);
    if (!body) return ESP_OK; /* error response already sent */
    copy_str(body, "ip", cfg->hue_ip, sizeof(cfg->hue_ip));
    cJSON *group = cJSON_GetObjectItem(body, "group");
    if (cJSON_IsNumber(group)) cfg->hue_group = group->valueint;
    cJSON_Delete(body);

    if (cfg->hue_ip[0] == '\0')
        return send_err(req, "400 Bad Request", "no bridge ip");

    char user[HUE_USERNAME_MAX];
    esp_err_t err = hue_pair(cfg->hue_ip, user, sizeof(user));
    if (err == ESP_ERR_INVALID_STATE)
        return send_err(req, "409 Conflict", "link button not pressed");
    if (err != ESP_OK)
        return send_err(req, "502 Bad Gateway", "bridge unreachable");

    strlcpy(cfg->hue_user, user, sizeof(cfg->hue_user));
    settings_save();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static void reboot_task(void *pv)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t h_settings(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    settings_t *cfg = settings();
    cJSON *body = read_body(req);
    if (!body) return ESP_OK; /* error response already sent */

    copy_str(body, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    copy_str(body, "wifi_pass", cfg->wifi_pass, sizeof(cfg->wifi_pass));
    copy_str(body, "hue_ip", cfg->hue_ip, sizeof(cfg->hue_ip));
    copy_str(body, "api_token", cfg->api_token, sizeof(cfg->api_token));
    cJSON *group = cJSON_GetObjectItem(body, "hue_group");
    if (cJSON_IsNumber(group)) cfg->hue_group = group->valueint;

    /* Manual role-slot overrides, e.g. {"heater":{"ip":..,"id":..,"key":..}} */
    struct { const char *name; sw_slot_t *slot; } slots[] = {
        { "heater", &cfg->heater },
        { "plug", &cfg->plug },
        { "breeze", &cfg->breeze },
    };
    for (size_t i = 0; i < 3; i++) {
        cJSON *o = cJSON_GetObjectItem(body, slots[i].name);
        if (!cJSON_IsObject(o)) continue;
        copy_str(o, "ip", slots[i].slot->ip, sizeof(slots[i].slot->ip));
        copy_str(o, "id", slots[i].slot->id, sizeof(slots[i].slot->id));
        copy_str(o, "key", slots[i].slot->key, sizeof(slots[i].slot->key));
    }

    bool reboot = cJSON_IsTrue(cJSON_GetObjectItem(body, "reboot"));
    cJSON_Delete(body);
    settings_save();

    if (reboot) {
        ESP_LOGI(TAG, "rebooting on request");
        xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

/* --- server ----------------------------------------------------------- */

esp_err_t webui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    /* Leave lwIP sockets free for outbound Hue/Switcher connections and
     * the two discovery listeners (default is 7 + 3 internal = all 10). */
    cfg.max_open_sockets = 4;

    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) return err;

    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_index },
        { .uri = "/api/status", .method = HTTP_GET, .handler = h_status },
        { .uri = "/api/intent", .method = HTTP_POST, .handler = h_intent },
        { .uri = "/api/hue/discover", .method = HTTP_POST,
          .handler = h_hue_discover },
        { .uri = "/api/hue/pair", .method = HTTP_POST, .handler = h_hue_pair },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = h_settings },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(server, &routes[i]);

    ESP_LOGI(TAG, "web ui up on :80");
    return ESP_OK;
}
