#include "sound.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "app_settings.h"

static const char *TAG = "sound";

#define RATE 22050

static QueueHandle_t s_q;
static esp_codec_dev_handle_t s_spk;
static bool s_failed;

typedef struct { float hz; int ms; } note_t;

static const note_t DONE[]  = { { 784, 140 }, { 0, 40 }, { 1047, 140 }, { 0, 40 }, { 1319, 260 } };
static const note_t ERROR[] = { { 440, 220 }, { 0, 90 }, { 440, 220 }, { 0, 90 }, { 330, 380 } };
static const note_t TEST[]  = { { 1047, 120 } };

static void play_notes(const note_t *n, int count)
{
    esp_codec_dev_sample_info_t fs = { .sample_rate = RATE, .channel = 1, .bits_per_sample = 16 };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) return;
    esp_codec_dev_set_out_vol(s_spk, g_settings.alert_vol);
    const int chunk = 512;
    int16_t *buf = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (buf) {
        for (int i = 0; i < count; i++) {
            int total = RATE * n[i].ms / 1000;
            int fade = RATE / 200;   /* 5 ms ramps: no clicks */
            for (int done = 0; done < total; done += chunk) {
                int len = total - done < chunk ? total - done : chunk;
                for (int k = 0; k < len; k++) {
                    int t = done + k;
                    float env = 1.0f;
                    if (t < fade) env = (float)t / fade;
                    else if (total - t < fade) env = (float)(total - t) / fade;
                    buf[k] = n[i].hz > 0 ? (int16_t)(sinf(2.0f * (float)M_PI * n[i].hz * t / RATE) * 12000 * env) : 0;
                }
                esp_codec_dev_write(s_spk, buf, len * sizeof(int16_t));
            }
        }
        heap_caps_free(buf);
    }
    esp_codec_dev_close(s_spk);   /* powers the amplifier down again */
}

static void sound_task(void *arg)
{
    sound_t s;
    for (;;) {
        if (xQueueReceive(s_q, &s, portMAX_DELAY) != pdTRUE) continue;
        if (!s_spk && !s_failed) {
            s_spk = bsp_audio_codec_speaker_init();
            if (!s_spk) {
                s_failed = true;
                ESP_LOGW(TAG, "speaker not available");
            }
        }
        if (!s_spk || !g_settings.alert_vol) continue;
        if (s == SOUND_DONE) play_notes(DONE, sizeof(DONE) / sizeof(DONE[0]));
        else if (s == SOUND_ERROR) play_notes(ERROR, sizeof(ERROR) / sizeof(ERROR[0]));
        else play_notes(TEST, 1);
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
