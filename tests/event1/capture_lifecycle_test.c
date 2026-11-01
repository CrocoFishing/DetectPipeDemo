/* Exercise the actual capture APIs against a resource-owning I2S/task model.
 * A single initial-gate probe exits at the notification wait; normal capture
 * work and its final quiescence acknowledgement are otherwise inert.
 */
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "capture_host_shims.h"
#include "audio_capture.h"

enum fault { NO_FAULT, CHANNEL_CREATE, PDM_INIT, CALLBACK_REGISTER, TASK_CREATE,
             CHANNEL_ENABLE, CHANNEL_DISABLE, CHANNEL_DELETE };
static enum fault fault;
static host_channel_t rx_model;
static host_task_t worker;
static unsigned channel_creates, channel_deletes, dma_setups, callback_registrations;
static unsigned task_creates, task_deletes, enables, disables, notifications, delays;
static unsigned call_sequence, task_delete_sequence, channel_delete_sequence, cases;
static bool hold_worker, probing_gate;
static unsigned gate_waits, read_calls;
static jmp_buf initial_gate;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4267) /* ESP32 sizeof/size_t is 32 bits; the host is 64 bits. */
#endif
#include "../../firmware/components/audio_capture/audio_capture.c"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

static bool accept_frame(const audio_frame_t *frame, void *ctx) {
    (void)frame; (void)ctx;
    assert(!"lifecycle model must not receive real audio frames");
    return true;
}
static audio_capture_config_t config(void) {
    return (audio_capture_config_t) {.frame_cb = accept_frame, .read_timeout_ms = 1000};
}
static void assert_resources(bool expected) {
    assert(rx_model.allocated == expected && worker.allocated == expected);
    if (expected) assert(rx_model.configured && rx_model.dma_live && rx_model.callbacks_registered);
    else assert(!rx_model.enabled && !rx_model.dma_live);
}
static void reset_case(void) {
    assert_resources(false);
    assert(!audio_capture_is_running());
    memset(&rx_model, 0, sizeof(rx_model));
    memset(&worker, 0, sizeof(worker));
    fault = NO_FAULT;
    channel_creates = channel_deletes = dma_setups = callback_registrations = 0;
    task_creates = task_deletes = enables = disables = notifications = delays = 0;
    call_sequence = task_delete_sequence = channel_delete_sequence = 0;
    gate_waits = read_calls = 0;
    hold_worker = probing_gate = false;
    ++cases;
}

const board_audio_pins_t *board_audio_pins(void) {
    static const board_audio_pins_t pins = {41, 42, 21, 7, 8, 9, "legacy-gpio21"};
    return &pins;
}
esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx,
                         i2s_chan_handle_t *rx) {
    assert(cfg && !tx && rx && !rx_model.allocated && !worker.allocated);
    assert(cfg->id == I2S_NUM_0 && cfg->role == I2S_ROLE_MASTER);
    assert(cfg->dma_desc_num == 8 && cfg->dma_frame_num == 320);
    ++channel_creates;
    if (fault == CHANNEL_CREATE) return ESP_ERR_NO_MEM;
    rx_model.allocated = true;
    rx_model.channel_config = *cfg;
    *rx = &rx_model;
    return ESP_OK;
}
esp_err_t i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t handle, const i2s_pdm_rx_config_t *cfg) {
    assert(handle == &rx_model && rx_model.allocated && !rx_model.enabled && !worker.allocated);
    assert(cfg->clk_cfg.sample_rate_hz == 16000);
    assert(cfg->slot_cfg.data_bit_width == 16 && cfg->slot_cfg.slot_mode == I2S_SLOT_MODE_MONO);
    assert(cfg->slot_cfg.data_fmt == I2S_PDM_DATA_FMT_PCM);
    assert(cfg->gpio_cfg.clk == 42 && cfg->gpio_cfg.din == 41 && !cfg->gpio_cfg.invert_flags.clk_inv);
    ++dma_setups;
    rx_model.dma_live = true; /* Also model partial DMA ownership on failure. */
    rx_model.pdm_config = *cfg;
    if (fault == PDM_INIT) return ESP_ERR_NO_MEM;
    rx_model.configured = true;
    return ESP_OK;
}
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t handle,
        const i2s_event_callbacks_t *callbacks, void *ctx) {
    assert(handle == &rx_model && rx_model.configured && !worker.allocated && !rx_model.enabled);
    assert(callbacks && callbacks->on_recv_q_ovf && !ctx);
    ++callback_registrations;
    if (fault == CALLBACK_REGISTER) return ESP_FAIL;
    rx_model.callbacks_registered = true;
    return ESP_OK;
}
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
        uint32_t stack, void *argument, UBaseType_t priority, TaskHandle_t *task, int core) {
    assert(!worker.allocated && rx_model.dma_live && rx_model.configured && rx_model.callbacks_registered);
    assert(!rx_model.enabled && notifications == 0);
    assert(!strcmp(name, "audio_capture") && stack == 4096 && priority == 20 && core == 0 && !argument);
    ++task_creates;
    if (fault == TASK_CREATE) return pdFALSE;
    worker = (host_task_t) {.allocated = true, .entry = entry, .argument = argument};
    *task = &worker;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) {
    assert(task == &worker && worker.allocated && !audio_capture_is_running() && !rx_model.enabled);
    worker.allocated = false;
    ++task_deletes;
    task_delete_sequence = ++call_sequence;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task) {
    assert(task == &worker && worker.allocated && rx_model.enabled && audio_capture_is_running());
    ++worker.notifications;
    ++notifications;
    return pdPASS;
}
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait) {
    assert(probing_gate && clear_on_exit == pdTRUE && wait == portMAX_DELAY);
    assert(worker.allocated && !rx_model.enabled && notifications == 0 && read_calls == 0);
    ++gate_waits;
    longjmp(initial_gate, 1);
}
void vTaskDelay(TickType_t wait) {
    assert(wait == pdMS_TO_TICKS(10));
    assert(worker.allocated && rx_model.enabled && s_stop_requested);
    ++delays;
    /* Model the read loop's final acknowledgement. It owns no RX accesses
     * after this point; the allocated worker remains gated for the next run.
     */
    if (!hold_worker) s_running = false;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t handle) {
    assert(handle == &rx_model && rx_model.configured && worker.allocated && !rx_model.enabled);
    ++enables;
    if (fault == CHANNEL_ENABLE) return ESP_FAIL;
    rx_model.enabled = true;
    return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t handle) {
    assert(handle == &rx_model && rx_model.enabled && worker.allocated && !audio_capture_is_running());
    ++disables;
    if (fault == CHANNEL_DISABLE) return ESP_FAIL;
    rx_model.enabled = false;
    return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t handle) {
    assert(handle == &rx_model && rx_model.allocated && !rx_model.enabled && !worker.allocated);
    ++channel_deletes;
    channel_delete_sequence = ++call_sequence;
    if (fault == CHANNEL_DELETE) return ESP_FAIL;
    memset(&rx_model, 0, sizeof(rx_model));
    return ESP_OK;
}
esp_err_t i2s_channel_read(i2s_chan_handle_t handle, void *buffer, size_t bytes,
                         size_t *read, uint32_t timeout_ms) {
    (void)handle; (void)buffer; (void)bytes; (void)read; (void)timeout_ms;
    ++read_calls;
    assert(!"gated task must not read RX before start");
    return ESP_ERR_TIMEOUT;
}
void audio_stats_lock(void) {}
void audio_stats_unlock(void) {}

static void test_invalid_and_uninitialized(void) {
    reset_case();
    audio_capture_config_t invalid = {0};
    assert(audio_capture_init(NULL) == ESP_ERR_INVALID_ARG);
    assert(audio_capture_init(&invalid) == ESP_ERR_INVALID_ARG);
    assert(audio_capture_start() == ESP_ERR_INVALID_STATE);
    assert(audio_capture_stop() == ESP_OK);
    audio_capture_deinit();
    assert(channel_creates == 0 && task_creates == 0 && channel_deletes == 0 && task_deletes == 0);
    assert_resources(false);
}
static void test_initial_reservation_and_gate(void) {
    reset_case();
    audio_capture_config_t cfg = config();
    assert(audio_capture_init(&cfg) == ESP_OK);
    assert_resources(true);
    assert(task_creates == 1 && dma_setups == 1 && enables == 0 && notifications == 0);
    assert(!audio_capture_is_running());
    probing_gate = true;
    if (setjmp(initial_gate) == 0) {
        worker.entry(worker.argument);
        assert(!"worker must block at initial notification gate");
    }
    probing_gate = false;
    assert(gate_waits == 1 && read_calls == 0);
    assert(audio_capture_init(&cfg) == ESP_ERR_INVALID_STATE);
    assert(audio_capture_stop() == ESP_OK && disables == 0);
    audio_capture_deinit();
    assert_resources(false);
    assert(task_deletes == 1 && channel_deletes == 1 && task_delete_sequence < channel_delete_sequence);
    audio_capture_deinit();
    assert(task_deletes == 1 && channel_deletes == 1);
}
static void test_start_stop_reuses_worker(void) {
    reset_case();
    audio_capture_config_t cfg = config();
    assert(audio_capture_init(&cfg) == ESP_OK);
    for (unsigned run = 0; run < 3; ++run) {
        assert(audio_capture_start() == ESP_OK && audio_capture_is_running());
        assert(audio_capture_start() == ESP_OK);
        assert(task_creates == 1 && channel_creates == 1 && dma_setups == 1);
        assert(notifications == run + 1 && enables == run + 1);
        assert(audio_capture_stop() == ESP_OK && !audio_capture_is_running());
        assert(audio_capture_stop() == ESP_OK);
        assert(disables == run + 1 && task_deletes == 0 && channel_deletes == 0);
        assert_resources(true);
    }
    /* An active deinit must quiesce before deleting the retained worker/RX. */
    assert(audio_capture_start() == ESP_OK);
    audio_capture_deinit();
    assert_resources(false);
    assert(task_creates == 1 && task_deletes == 1 && task_delete_sequence < channel_delete_sequence);
}
static void test_init_failures_cleanup_and_retry(void) {
    const enum fault stages[] = {CHANNEL_CREATE, PDM_INIT, CALLBACK_REGISTER, TASK_CREATE};
    for (unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        reset_case();
        audio_capture_config_t cfg = config();
        fault = stages[i];
        esp_err_t expected = fault == CALLBACK_REGISTER ? ESP_FAIL : ESP_ERR_NO_MEM;
        assert(audio_capture_init(&cfg) == expected);
        assert_resources(false);
        assert(enables == 0 && notifications == 0 && task_deletes == 0);
        assert(channel_deletes == (stages[i] == CHANNEL_CREATE ? 0U : 1U));
        fault = NO_FAULT;
        assert(audio_capture_init(&cfg) == ESP_OK);
        assert_resources(true);
        assert(audio_capture_start() == ESP_OK);
        audio_capture_deinit();
        assert_resources(false);
    }
}
static void test_enable_failure_does_not_release_worker(void) {
    reset_case();
    audio_capture_config_t cfg = config();
    assert(audio_capture_init(&cfg) == ESP_OK);
    fault = CHANNEL_ENABLE;
    assert(audio_capture_start() == ESP_FAIL && !audio_capture_is_running());
    assert(notifications == 0 && !rx_model.enabled);
    assert_resources(true);
    assert(audio_capture_stop() == ESP_OK && disables == 0);
    fault = NO_FAULT;
    assert(audio_capture_start() == ESP_OK && notifications == 1);
    audio_capture_deinit();
    assert_resources(false);
    assert(task_creates == 1 && dma_setups == 1);
}
static void test_stop_timeout_preserves_resources(void) {
    reset_case();
    audio_capture_config_t cfg = config();
    assert(audio_capture_init(&cfg) == ESP_OK && audio_capture_start() == ESP_OK);
    hold_worker = true;
    assert(audio_capture_stop() == ESP_ERR_TIMEOUT);
    assert(audio_capture_is_running() && rx_model.enabled && delays == 150);
    audio_capture_deinit();
    assert_resources(true);
    assert(task_deletes == 0 && channel_deletes == 0 && disables == 0);
    hold_worker = false;
    audio_capture_deinit();
    assert_resources(false);
    assert(task_deletes == 1 && channel_deletes == 1 && disables == 1);
}
static void test_disable_failure_retries_without_resource_loss(void) {
    reset_case();
    audio_capture_config_t cfg = config();
    assert(audio_capture_init(&cfg) == ESP_OK && audio_capture_start() == ESP_OK);
    fault = CHANNEL_DISABLE;
    assert(audio_capture_stop() == ESP_FAIL);
    assert(!audio_capture_is_running() && rx_model.enabled);
    audio_capture_deinit();
    assert_resources(true);
    assert(task_deletes == 0 && channel_deletes == 0 && disables == 2);
    fault = NO_FAULT;
    assert(audio_capture_stop() == ESP_OK && !rx_model.enabled && disables == 3);
    assert_resources(true);
    audio_capture_deinit();
    assert_resources(false);
}
static void test_channel_delete_failure_can_be_retried(void) {
    reset_case();
    audio_capture_config_t cfg = config();
    assert(audio_capture_init(&cfg) == ESP_OK);
    fault = CHANNEL_DELETE;
    audio_capture_deinit();
    assert(rx_model.allocated && rx_model.dma_live && !worker.allocated && !rx_model.enabled);
    assert(task_deletes == 1 && channel_deletes == 1);
    assert(audio_capture_start() == ESP_ERR_INVALID_STATE);
    fault = NO_FAULT;
    audio_capture_deinit();
    assert_resources(false);
    assert(task_deletes == 1 && channel_deletes == 2);
    assert(audio_capture_init(&cfg) == ESP_OK);
    audio_capture_deinit();
    assert_resources(false);
}
int main(void) {
    test_invalid_and_uninitialized();
    test_initial_reservation_and_gate();
    test_start_stop_reuses_worker();
    test_init_failures_cleanup_and_retry();
    test_enable_failure_does_not_release_worker();
    test_stop_timeout_preserves_resources();
    test_disable_failure_retries_without_resource_loss();
    test_channel_delete_failure_can_be_retried();
    printf("capture startup/lifecycle tests passed (%u cases)\n", cases);
    return 0;
}
