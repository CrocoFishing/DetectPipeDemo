#include "audio_capture.h"

#include <inttypes.h>
#include <string.h>
#include "board_config.h"
#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_capture";
static i2s_chan_handle_t s_rx;
static TaskHandle_t s_task;
static audio_capture_config_t s_config;
static volatile bool s_running;
static volatile bool s_stop_requested;
static bool s_rx_enabled;
// Intentionally survives deinit/init during SD export; only reboot resets the
// capture counter. Each resumed DSP epoch maps these indices to current time.
static uint64_t s_next_sample_index;
static volatile uint32_t s_dma_lost_samples;
static volatile uint32_t s_dma_overflows;
static portMUX_TYPE s_dma_lock = portMUX_INITIALIZER_UNLOCKED;

static bool IRAM_ATTR dma_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle;
    (void)ctx;
    portENTER_CRITICAL_ISR(&s_dma_lock);
    s_dma_lost_samples += event->size / sizeof(int16_t);
    s_dma_overflows++;
    portEXIT_CRITICAL_ISR(&s_dma_lock);
    return false;
}

static void capture_task(void *arg)
{
    (void)arg;
    int16_t samples[AUDIO_FRAME_SAMPLES];
    for (;;) {
        /* Init reserves this task's internal stack before AFE allocations.
         * No RX access occurs until start enables the channel and wakes us.
         */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        printf("EVENT,audio_capture_start,%" PRId64 ",%" PRIu64 "\n",
               esp_timer_get_time(), s_next_sample_index);

        while (!s_stop_requested) {
            size_t bytes_read = 0;
            const int64_t begin_us = esp_timer_get_time();
            esp_err_t err = i2s_channel_read(s_rx, samples, sizeof(samples), &bytes_read,
                                             s_config.read_timeout_ms);
            const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - begin_us);
            if (s_config.stats) {
                audio_stats_lock();
                if (elapsed_us > s_config.stats->capture_max_processing_us)
                    s_config.stats->capture_max_processing_us = elapsed_us;
                audio_stats_unlock();
            }
            if (err == ESP_ERR_TIMEOUT) {
                continue;
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(err));
                printf("ERROR,i2s_read,%" PRId64 ",%" PRIu64 ",%s\n",
                       esp_timer_get_time(), s_next_sample_index, esp_err_to_name(err));
                continue;
            }
            if (bytes_read == 0) {
                continue;
            }

            const size_t count = bytes_read / sizeof(int16_t);
            portENTER_CRITICAL(&s_dma_lock);
            uint32_t lost = s_dma_lost_samples;
            uint32_t overflows = s_dma_overflows;
            s_dma_lost_samples = 0;
            s_dma_overflows = 0;
            portEXIT_CRITICAL(&s_dma_lock);
            s_next_sample_index += lost;
            if (lost) printf("ERROR,i2s_dma_overrun,lost_samples,%" PRIu32 ",events,%" PRIu32 "\n", lost, overflows);
            const int64_t end_us = esp_timer_get_time();
            audio_frame_t frame = {
                .samples = samples,
                .sample_count = count,
                .sample_rate = AUDIO_SAMPLE_RATE_HZ,
                .first_sample_index = s_next_sample_index,
                .timestamp_us = end_us - (int64_t)((count * 1000000ULL) / AUDIO_SAMPLE_RATE_HZ),
            };
            s_next_sample_index += count;
            if (s_config.stats) {
                audio_stats_lock();
                s_config.stats->captured_samples += count;
                s_config.stats->dropped_samples += lost;
                s_config.stats->queue_overruns += overflows;
                audio_stats_unlock();
            }
            if (!s_config.frame_cb(&frame, s_config.frame_cb_ctx) && s_config.stats) {
                audio_stats_lock();
                s_config.stats->dropped_samples += count;
                s_config.stats->queue_overruns++;
                audio_stats_unlock();
            }
        }

        printf("EVENT,audio_capture_stop,%" PRId64 ",%" PRIu64 "\n",
               esp_timer_get_time(), s_next_sample_index);
        /* Stop may disable/delete RX after this acknowledgement. Retain the
         * worker and its stack for the next start, with no further RX access.
         */
        s_running = false;
    }
}

esp_err_t audio_capture_init(const audio_capture_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->frame_cb, ESP_ERR_INVALID_ARG, TAG,
                        "frame callback is required");
    ESP_RETURN_ON_FALSE(!s_rx && !s_task, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    s_config = *config;
    if (s_config.read_timeout_ms == 0) {
        s_config.read_timeout_ms = 1000;
    }

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 8;
    channel.dma_frame_num = AUDIO_FRAME_SAMPLES;
    esp_err_t err = i2s_new_channel(&channel, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S0 RX channel allocation failed: %s", esp_err_to_name(err));
        goto fail;
    }

    const board_audio_pins_t *pins = board_audio_pins();
    i2s_pdm_rx_config_t pdm = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = pins->pdm_clk,
            .din = pins->pdm_data,
            .invert_flags = {.clk_inv = false},
        },
    };
    err = i2s_channel_init_pdm_rx_mode(s_rx, &pdm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PDM/DMA initialization failed: %s (buffers=8 frame_samples=%u dma_bytes=%u)",
                 esp_err_to_name(err), (unsigned)AUDIO_FRAME_SAMPLES,
                 (unsigned)(8U * AUDIO_FRAME_BYTES));
        goto fail;
    }
    const i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = dma_overflow};
    err = i2s_channel_register_event_callback(s_rx, &callbacks, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S callback registration failed: %s", esp_err_to_name(err));
        goto fail;
    }
    if (xTaskCreatePinnedToCore(capture_task, "audio_capture", 4096, NULL, 20,
                                &s_task, 0) != pdPASS) {
        s_task = NULL;
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "capture task allocation failed: stack_bytes=4096");
        goto fail;
    }
    ESP_LOGI(TAG, "resources ready: dma_buffers=8 frame_samples=%u dma_bytes=%u task_stack_bytes=4096",
             (unsigned)AUDIO_FRAME_SAMPLES, (unsigned)(8U * AUDIO_FRAME_BYTES));
    return ESP_OK;

fail:
    if (s_rx) {
        (void)i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    memset(&s_config, 0, sizeof(s_config));
    return err;
}

esp_err_t audio_capture_start(void)
{
    if (!s_rx || !s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) {
        return ESP_OK;
    }
    s_stop_requested = false;
    esp_err_t err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) return err;
    s_rx_enabled = true;
    s_running = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t audio_capture_stop(void)
{
    if (!s_running && !s_rx_enabled) return ESP_OK;
    s_stop_requested = true;
    for (int i = 0; s_running && i < 150; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_running) return ESP_ERR_TIMEOUT;
    esp_err_t err = s_rx_enabled ? i2s_channel_disable(s_rx) : ESP_OK;
    if (err == ESP_OK) s_rx_enabled = false;
    return err;
}

bool audio_capture_is_running(void)
{
    return s_running;
}

void audio_capture_deinit(void)
{
    esp_err_t err = audio_capture_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "capture deinit stopped: %s; RX and worker retained", esp_err_to_name(err));
        return;
    }
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_rx) {
        err = i2s_del_channel(s_rx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S channel deletion failed: %s", esp_err_to_name(err));
            return;
        }
        s_rx = NULL;
    }
    s_stop_requested = false;
    memset(&s_config, 0, sizeof(s_config));
}
