/* Breeze IR remote set — see breeze_remote.h.
 *
 * The IRSet JSON is parsed once at load and flattened into a compact
 * string blob (placed in PSRAM when available); the cJSON tree is freed
 * immediately. Keeping the tree resident would pin 150-350 KB of heap for
 * the life of the app — enough to OOM a plain ESP32 or starve the S3's
 * voice models.
 */
#include "breeze_remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "breeze";

typedef struct {
    const char *key;
    const char *para;
    const char *hex;
} br_wave_t;

static br_wave_t *s_waves;
static int s_nwaves;
static char *s_blob; /* backing storage for all wave strings */
static char s_remote_id[16];
static bool s_on_off_type; /* toggle-style remote (single on/off key) */
static int s_min_temp = 100, s_max_temp = -100;

static const char *MODE_CMD[] = { NULL, "aa", "ad", "aw", "ar", "ah" };

/* Prefer PSRAM for the bulky IR tables; fall back to regular heap. */
static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static void unload(void)
{
    free(s_waves);
    free(s_blob);
    s_waves = NULL;
    s_blob = NULL;
    s_nwaves = 0;
}

int br_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "no remote set at %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) {
        fclose(f);
        return -1;
    }
    char *buf = big_alloc(size + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t rd = fread(buf, 1, size, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "cannot parse %s (bad JSON or out of memory)", path);
        return -1;
    }

    cJSON *waves = cJSON_GetObjectItem(root, "IRWaveList");
    cJSON *id = cJSON_GetObjectItem(root, "IRSetID");
    if (!cJSON_IsArray(waves) || !cJSON_IsString(id)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "%s is not an IRSet (need IRSetID + IRWaveList)", path);
        return -1;
    }

    /* Pass 1: count entries and total string bytes. */
    int n = 0;
    size_t blob_size = 0;
    cJSON *w;
    cJSON_ArrayForEach(w, waves) {
        cJSON *k = cJSON_GetObjectItem(w, "Key");
        cJSON *p = cJSON_GetObjectItem(w, "Para");
        cJSON *h = cJSON_GetObjectItem(w, "HexCode");
        if (!cJSON_IsString(k) || !cJSON_IsString(p) || !cJSON_IsString(h))
            continue;
        n++;
        blob_size += strlen(k->valuestring) + strlen(p->valuestring) +
                     strlen(h->valuestring) + 3;
    }
    if (n == 0) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "empty IRWaveList in %s", path);
        return -1;
    }

    br_wave_t *nw = big_alloc(n * sizeof(br_wave_t));
    char *blob = big_alloc(blob_size);
    if (!nw || !blob) {
        free(nw);
        free(blob);
        cJSON_Delete(root);
        ESP_LOGE(TAG, "out of memory for remote set (%d waves)", n);
        return -1;
    }

    /* Pass 2: flatten strings, scan the temperature range (keys like
     * "ar25_f2" carry the temp in chars [2:4]). */
    int min_t = 100, max_t = -100;
    char *pos = blob;
    int i = 0;
    cJSON_ArrayForEach(w, waves) {
        cJSON *k = cJSON_GetObjectItem(w, "Key");
        cJSON *p = cJSON_GetObjectItem(w, "Para");
        cJSON *h = cJSON_GetObjectItem(w, "HexCode");
        if (!cJSON_IsString(k) || !cJSON_IsString(p) || !cJSON_IsString(h))
            continue;
        const char *kk = k->valuestring;
        if (strlen(kk) >= 4 && kk[2] >= '0' && kk[2] <= '9' &&
            kk[3] >= '0' && kk[3] <= '9') {
            int t = (kk[2] - '0') * 10 + (kk[3] - '0');
            if (t < min_t) min_t = t;
            if (t > max_t) max_t = t;
        }
        nw[i].key = pos;
        pos = stpcpy(pos, kk) + 1;
        nw[i].para = pos;
        pos = stpcpy(pos, p->valuestring) + 1;
        nw[i].hex = pos;
        pos = stpcpy(pos, h->valuestring) + 1;
        i++;
    }

    strlcpy(s_remote_id, id->valuestring, sizeof(s_remote_id));
    cJSON *onoff = cJSON_GetObjectItem(root, "OnOffType");
    s_on_off_type = cJSON_IsNumber(onoff) && onoff->valueint == 1;
    cJSON_Delete(root);

    unload();
    s_waves = nw;
    s_blob = blob;
    s_nwaves = n;
    s_min_temp = min_t;
    s_max_temp = max_t;

    ESP_LOGI(TAG, "loaded remote %s (%s, %d waves, temps %d-%d, %u KB)",
             s_remote_id, s_on_off_type ? "toggle" : "stateful", n,
             s_min_temp, s_max_temp,
             (unsigned)((blob_size + n * sizeof(br_wave_t)) / 1024));
    return 0;
}

bool br_is_loaded(void)
{
    return s_waves != NULL;
}

const char *br_remote_id(void)
{
    return s_waves ? s_remote_id : "";
}

static const br_wave_t *find_wave(const char *key)
{
    for (int i = 0; i < s_nwaves; i++)
        if (strcmp(s_waves[i].key, key) == 0) return &s_waves[i];
    return NULL;
}

/* Join key parts [0..n) into buf. */
static void join_key(char *buf, size_t cap, const char *const parts[], int n)
{
    buf[0] = '\0';
    for (int i = 0; i < n; i++) strlcat(buf, parts[i], cap);
}

int br_build_command(const br_request_t *req, bool current_on, char *out,
                     size_t cap)
{
    if (!s_waves) {
        ESP_LOGE(TAG, "no remote set loaded");
        return -1;
    }

    int temp = req->target_temp;
    if (temp > s_max_temp) temp = s_max_temp;
    if (temp < s_min_temp) temp = s_min_temp;
    if (req->mode < BR_MODE_AUTO || req->mode > BR_MODE_HEAT) return -1;

    /* Build the lookup key the way the official app does: a list of parts,
     * dropped from the tail one by one until a key in the IRSet matches
     * (swing suffix first, then fan level, down to the bare mode). */
    const char *parts[5];
    int nparts = 0;
    char tempstr[4], fanstr[4];

    if (!s_on_off_type && !req->on) {
        parts[nparts++] = "off";
    } else {
        if (s_on_off_type && current_on != req->on) parts[nparts++] = "on_";
        snprintf(fanstr, sizeof(fanstr), "_f%c", req->fan_level);
        parts[nparts++] = MODE_CMD[req->mode];
        if (req->mode == BR_MODE_COOL || req->mode == BR_MODE_HEAT) {
            snprintf(tempstr, sizeof(tempstr), "%d", temp);
            parts[nparts++] = tempstr;
        }
        parts[nparts++] = fanstr;
        if (req->swing) parts[nparts++] = "_d1";
    }

    char key[32];
    const br_wave_t *wave = NULL;
    int n = nparts;
    do {
        join_key(key, sizeof(key), parts, n);
        wave = find_wave(key);
    } while (!wave && --n > 0);

    if (!wave) {
        ESP_LOGE(TAG, "no IR wave for key '%s' on remote %s", key,
                 s_remote_id);
        return -1;
    }

    /* Payload: "00000000" + hex("<Para>|<HexCode>") */
    size_t need = 8 + 2 * (strlen(wave->para) + 1 + strlen(wave->hex)) + 1;
    if (cap < need) return -1;

    strcpy(out, "00000000");
    char *p = out + 8;
    const char *srcs[] = { wave->para, "|", wave->hex };
    for (int i = 0; i < 3; i++)
        for (const char *c = srcs[i]; *c; c++) {
            sprintf(p, "%02x", (unsigned char)*c);
            p += 2;
        }
    *p = '\0';
    return (int)(p - out);
}
