#pragma once
/* Narrow I2S/FreeRTOS API declarations for the production capture lifecycle.
 * The fake driver owns resources; notifications and stop acknowledgement are
 * modeled without DMA transfers or concurrent worker execution.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
static inline const char *esp_err_to_name(esp_err_t error) { (void)error; return "host error"; }
#define ESP_RETURN_ON_FALSE(condition, error, tag, ...) do { \
    (void)(tag); if (!(condition)) return (error); \
} while (0)
static inline void host_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
#define ESP_LOGE(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log(tag, __VA_ARGS__)
#define IRAM_ATTR
static inline int64_t esp_timer_get_time(void) { return 0; }

typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portENTER_CRITICAL_ISR(lock) ((void)(lock))
#define portEXIT_CRITICAL_ISR(lock) ((void)(lock))
typedef struct {
    bool allocated;
    unsigned notifications;
    void (*entry)(void *);
    void *argument;
} host_task_t;
typedef host_task_t *TaskHandle_t;
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
        uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *task, int core);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t wait);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait);
BaseType_t xTaskNotifyGive(TaskHandle_t task);

typedef struct {
    int pdm_data, pdm_clk, sd_cs, sd_sclk, sd_miso, sd_mosi;
    const char *sd_profile;
} board_audio_pins_t;
const board_audio_pins_t *board_audio_pins(void);

#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 1
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_MONO 1
#define I2S_PDM_DATA_FMT_PCM 1
typedef struct { int id, role; unsigned dma_desc_num, dma_frame_num; } i2s_chan_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(number, channel_role) \
    { .id = (number), .role = (channel_role), .dma_desc_num = 2, .dma_frame_num = 240 }
typedef struct { unsigned sample_rate_hz; } i2s_pdm_rx_clk_config_t;
#define I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate) { .sample_rate_hz = (rate) }
typedef struct { int data_bit_width, slot_mode, data_fmt; } i2s_pdm_rx_slot_config_t;
#define I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(bits, mode) \
    { .data_bit_width = (bits), .slot_mode = (mode), .data_fmt = I2S_PDM_DATA_FMT_PCM }
typedef struct {
    i2s_pdm_rx_clk_config_t clk_cfg;
    i2s_pdm_rx_slot_config_t slot_cfg;
    struct { int clk, din; struct { bool clk_inv; } invert_flags; } gpio_cfg;
} i2s_pdm_rx_config_t;
typedef struct {
    bool allocated, configured, dma_live, callbacks_registered, enabled;
    i2s_chan_config_t channel_config;
    i2s_pdm_rx_config_t pdm_config;
} host_channel_t;
typedef host_channel_t *i2s_chan_handle_t;
/* ESP32 size_t is 32 bits; preserve that width for the ISR sample counter. */
typedef struct { uint32_t size; } i2s_event_data_t;
typedef struct {
    bool (*on_recv_q_ovf)(i2s_chan_handle_t channel, i2s_event_data_t *event, void *ctx);
} i2s_event_callbacks_t;
esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx,
                         i2s_chan_handle_t *rx);
esp_err_t i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t channel, const i2s_pdm_rx_config_t *config);
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t channel,
        const i2s_event_callbacks_t *callbacks, void *ctx);
esp_err_t i2s_channel_enable(i2s_chan_handle_t channel);
esp_err_t i2s_channel_disable(i2s_chan_handle_t channel);
esp_err_t i2s_del_channel(i2s_chan_handle_t channel);
esp_err_t i2s_channel_read(i2s_chan_handle_t channel, void *buffer, size_t bytes,
                         size_t *read, uint32_t timeout_ms);
