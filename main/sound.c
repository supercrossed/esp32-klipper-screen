/*
 * Alert chimes on the built-in ES8311 codec + speaker amplifier.
 *
 * Mirrors Waveshare's known-good audio example for this board: I2S0 master,
 * 16 kHz, 16-bit stereo, MCLK = 384 x fs from GPIO13, amplifier enable on
 * GPIO53 driven directly. Everything is set up on first use and every step is
 * logged, so a silent speaker shows up in the serial log with a reason.
 */
#include "sound.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "app_settings.h"

static const char *TAG = "sound";

#define RATE        16000
#define MCLK_MULT   384
#define PA_GPIO     BSP_POWER_AMP_IO   /* GPIO53 */

static QueueHandle_t s_q;
static esp_codec_dev_handle_t s_spk;
static i2s_chan_handle_t s_tx;
static bool s_failed;

typedef struct { float hz; int ms; } note_t;

static const note_t DONE[]  = { { 784, 140 }, { 0, 40 }, { 1047, 140 }, { 0, 40 }, { 1319, 260 } };
static const note_t ERROR[] = { { 440, 220 }, { 0, 90 }, { 440, 220 }, { 0, 90 }, { 330, 380 } };
static const note_t TEST[]  = { { 1047, 160 }, { 0, 50 }, { 1319, 160 } };

static bool audio_init(void)
{
    esp_err_t err;
    gpio_config_t pa = { .pin_bit_mask = 1ULL << PA_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&pa);
    gpio_set_level(PA_GPIO, 0);

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    if ((err = i2s_new_channel(&cc, &s_tx, NULL)) != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err)); return false; }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK, .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT, .din = I2S_GPIO_UNUSED,
        },
    };
    sc.clk_cfg.mclk_multiple = MCLK_MULT;
    if ((err = i2s_channel_init_std_mode(s_tx, &sc)) != ESP_OK) { ESP_LOGE(TAG, "i2s init: %s", esp_err_to_name(err)); return false; }
    if ((err = i2s_channel_enable(s_tx)) != ESP_OK) { ESP_LOGE(TAG, "i2s enable: %s", esp_err_to_name(err)); return false; }

    if (bsp_i2c_init() != ESP_OK || !bsp_i2c_get_handle()) { ESP_LOGE(TAG, "I2C bus not available"); return false; }
    audio_codec_i2s_cfg_t ic = { .port = I2S_NUM_0, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&ic);
    audio_codec_i2c_cfg_t cfg = { .port = BSP_I2C_NUM, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bsp_i2c_get_handle() };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl || !gpio_if) { ESP_LOGE(TAG, "codec interfaces failed"); return false; }
    es8311_codec_cfg_t es = {
        .ctrl_if = ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,                 /* amplifier driven by us (as in Waveshare's example) */
        .use_mclk = true,
        .mclk_div = MCLK_MULT,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es);
    if (!codec) { ESP_LOGE(TAG, "ES8311 not responding on I2C 0x%02x", ES8311_CODEC_DEFAULT_ADDR >> 1); return false; }
    esp_codec_dev_cfg_t dc = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    s_spk = esp_codec_dev_new(&dc);
    if (!s_spk) { ESP_LOGE(TAG, "esp_codec_dev_new failed"); return false; }
    ESP_LOGI(TAG, "speaker ready (ES8311, I2S0 %d Hz)", RATE);
    return true;
}

static void play_notes(const note_t *n, int count)
{
    esp_codec_dev_sample_info_t fs = { .sample_rate = RATE, .channel = 2, .bits_per_sample = 16 };
    int r = esp_codec_dev_open(s_spk, &fs);
    if (r != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "codec open failed (%d)", r); return; }
    esp_codec_dev_set_out_mute(s_spk, false);
    esp_codec_dev_set_out_vol(s_spk, g_settings.alert_vol);
    gpio_set_level(PA_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));   /* let the amplifier settle: no pop */
    const int frames = 256;
    int16_t *buf = heap_caps_malloc(frames * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (buf) {
        for (int i = 0; i < count; i++) {
            int total = RATE * n[i].ms / 1000;
            int fade = RATE / 200;   /* 5 ms ramps: no clicks */
            for (int done = 0; done < total; done += frames) {
                int len = total - done < frames ? total - done : frames;
                for (int k = 0; k < len; k++) {
                    int t = done + k;
                    float env = 1.0f;
                    if (t < fade) env = (float)t / fade;
                    else if (total - t < fade) env = (float)(total - t) / fade;
                    int16_t v = n[i].hz > 0 ? (int16_t)(sinf(2.0f * (float)M_PI * n[i].hz * t / RATE) * 14000 * env) : 0;
                    buf[k * 2] = v;
                    buf[k * 2 + 1] = v;
                }
                esp_codec_dev_write(s_spk, buf, len * 2 * sizeof(int16_t));
            }
        }
        heap_caps_free(buf);
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(PA_GPIO, 0);
    esp_codec_dev_close(s_spk);
    ESP_LOGI(TAG, "played %d notes at volume %d", count, g_settings.alert_vol);
}

static void sound_task(void *arg)
{
    sound_t s;
    for (;;) {
        if (xQueueReceive(s_q, &s, portMAX_DELAY) != pdTRUE) continue;
        if (!s_spk && !s_failed) {
            s_failed = !audio_init();
            if (s_failed) ESP_LOGW(TAG, "speaker not available - alerts stay silent");
        }
        if (!s_spk || !g_settings.alert_vol) continue;
        if (s == SOUND_DONE) play_notes(DONE, sizeof(DONE) / sizeof(DONE[0]));
        else if (s == SOUND_ERROR) play_notes(ERROR, sizeof(ERROR) / sizeof(ERROR[0]));
        else play_notes(TEST, sizeof(TEST) / sizeof(TEST[0]));
    }
}

void sound_play(sound_t s)
{
    if (!g_settings.alert_vol) return;
    if (!s_q) {
        s_q = xQueueCreate(4, sizeof(sound_t));
        /* internal stack: I2C/I2S driver calls */
        xTaskCreate(sound_task, "sound", 6144, NULL, 3, NULL);
    }
    xQueueSend(s_q, &s, 0);
}
