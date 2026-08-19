/* Offline voice pipeline — see voice.h. */
#include "voice.h"

#if !CONFIG_ESPISSTANT_VOICE

esp_err_t voice_start(voice_intent_cb_t cb)
{
    (void)cb;
    return ESP_ERR_NOT_SUPPORTED;
}

#else /* CONFIG_ESPISSTANT_VOICE */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"
#include "voice_commands.h"

static const char *TAG = "voice";

#define SAMPLE_RATE 16000
#define MN_TIMEOUT_MS 6000

static voice_intent_cb_t s_cb;
static i2s_chan_handle_t s_rx;
static esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mn_data;

/* --- microphone ------------------------------------------------------- */

static esp_err_t mic_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) return err;

    /* INMP441: 24-bit data in 32-bit frames, left slot. */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_ESPISSTANT_MIC_I2S_SCK,
            .ws = CONFIG_ESPISSTANT_MIC_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_ESPISSTANT_MIC_I2S_SD,
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) return err;
    return i2s_channel_enable(s_rx);
}

/* Read `n` 16-bit samples from the mic into `out`. */
static bool mic_read(int16_t *out, size_t n)
{
    static int32_t raw[1024];
    size_t done = 0;
    while (done < n) {
        size_t want = n - done;
        if (want > 1024) want = 1024;
        size_t got_bytes = 0;
        if (i2s_channel_read(s_rx, raw, want * sizeof(int32_t), &got_bytes,
                             portMAX_DELAY) != ESP_OK)
            return false;
        size_t got = got_bytes / sizeof(int32_t);
        for (size_t i = 0; i < got; i++) {
            /* >>14 gives ~+12 dB of gain for distant speech; saturate so
             * loud input clips instead of wrapping. */
            int32_t v = raw[i] >> 14;
            if (v > INT16_MAX) v = INT16_MAX;
            if (v < INT16_MIN) v = INT16_MIN;
            out[done + i] = (int16_t)v;
        }
        done += got;
    }
    return true;
}

/* --- tasks ------------------------------------------------------------ */

static void feed_task(void *pv)
{
    int chunk = s_afe->get_feed_chunksize(s_afe_data);
    int nch = s_afe->get_feed_channel_num(s_afe_data);
    int16_t *buf = malloc(chunk * nch * sizeof(int16_t));
    assert(buf && nch == 1);

    for (;;) {
        if (mic_read(buf, chunk))
            s_afe->feed(s_afe_data, buf);
        else
            vTaskDelay(pdMS_TO_TICKS(20)); /* don't spin on a dead mic */
    }
}

static void detect_task(void *pv)
{
    bool listening = false;

    for (;;) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "wake word detected, listening...");
            s_afe->disable_wakenet(s_afe_data);
            s_mn->clean(s_mn_data);
            listening = true;
            continue;
        }
        if (!listening) continue;

        esp_mn_state_t state = s_mn->detect(s_mn_data, res->data);
        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            if (r->num > 0) {
                int id = r->command_id[0];
                if (id >= 0 && id < (int)VOICE_COMMANDS_COUNT) {
                    ESP_LOGI(TAG, "command: \"%s\" -> %s",
                             VOICE_COMMANDS[id].phrase,
                             VOICE_COMMANDS[id].intent);
                    if (s_cb) s_cb(VOICE_COMMANDS[id].intent);
                }
            }
            listening = false;
            s_afe->enable_wakenet(s_afe_data);
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGI(TAG, "no command heard, back to wake word");
            listening = false;
            s_afe->enable_wakenet(s_afe_data);
        }
    }
}

/* --- setup ------------------------------------------------------------ */

esp_err_t voice_start(voice_intent_cb_t cb)
{
    s_cb = cb;

    esp_err_t err = mic_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s init failed: %s", esp_err_to_name(err));
        return err;
    }

    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "no sr models found in the 'model' partition");
        return ESP_FAIL;
    }

    /* AFE with wake word, mono mic. */
    afe_config_t *afe_cfg =
        afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afe_cfg) return ESP_FAIL;
    s_afe = esp_afe_handle_from_config(afe_cfg);
    s_afe_data = s_afe->create_from_config(afe_cfg);
    afe_config_free(afe_cfg);
    if (!s_afe_data) return ESP_FAIL;

    /* MultiNet with our English command set. */
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (!mn_name) {
        ESP_LOGE(TAG, "no english multinet model in this build");
        return ESP_FAIL;
    }
    s_mn = esp_mn_handle_from_name(mn_name);
    s_mn_data = s_mn->create(mn_name, MN_TIMEOUT_MS);
    if (!s_mn_data) return ESP_FAIL;

    esp_mn_commands_alloc(s_mn, s_mn_data);
    esp_mn_commands_clear();
    for (int i = 0; i < (int)VOICE_COMMANDS_COUNT; i++)
        esp_mn_commands_add(i, (char *)VOICE_COMMANDS[i].phrase);
    esp_mn_commands_update();
    s_mn->print_active_speech_commands(s_mn_data);

    xTaskCreatePinnedToCore(feed_task, "voice_feed", 8 * 1024, NULL, 5, NULL,
                            0);
    xTaskCreatePinnedToCore(detect_task, "voice_detect", 8 * 1024, NULL, 5,
                            NULL, 1);

    ESP_LOGI(TAG, "voice pipeline running (say \"Hi ESP\")");
    return ESP_OK;
}

#endif /* CONFIG_ESPISSTANT_VOICE */
