#include "sd_recorder.h"
#include <dirent.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "board_config.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "diskio_sdmmc.h"
#include "wav_writer.h"
#include "recorder_queue_policy.h"

#define MOUNT_POINT "/sd"
#define RECORDER_QUEUE_DEPTH 32
#define RECORDER_PACKET_SAMPLES 512
#define METADATA_ROW_BYTES 512
typedef struct {
    packet_type_t type;
    uint64_t event_id;
    uint64_t first_sample_index;
    int64_t timestamp_us;
    size_t count;
    bool producer_failed;
    uint32_t barrier_token;
    int16_t *owned_samples;
    sd_event_metadata_t metadata;
    sd_metadata_kind_t metadata_kind;
    union {
        int16_t samples[RECORDER_PACKET_SAMPLES];
        char csv_row[METADATA_ROW_BYTES];
    } data;
} recorder_packet_t;
typedef struct {
    sd_recorder_config_t config;
    QueueHandle_t queue;
    StaticQueue_t queue_control;
    uint8_t *queue_storage;
    TaskHandle_t task;
    bool mounted;
    volatile bool running;
    sdmmc_card_t *card;
    sdmmc_host_t host;
    uint32_t barrier_requested, barrier_acknowledged;
    esp_err_t barrier_error;
} recorder_state_t;
static recorder_state_t s_recorder;
static portMUX_TYPE s_enqueue_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *TAG = "sd_recorder";
static const char *const METADATA_FILES[] = {
    "audio_events.csv", "face_associations.csv", "event_lifecycle.csv"
};
static const char *const METADATA_HEADERS[] = {
    "event_id,audio_file,audio_source,sample_rate,channels,bits_per_sample,audio_first_sample,"
    "vad_start_sample,vad_end_sample,audio_last_sample,vad_start_timestamp_us,vad_end_timestamp_us,audio_status",
    "event_id,request_id,image_id,face_index,face_count,status,person_id,similarity,processing_time_ms",
    "event_id,state,timestamp_us,status"
};

static bool enqueue(const recorder_packet_t *packet)
{
    if (!s_recorder.running || !s_recorder.queue) return false;
    /* Data and metadata leave two slots; BEGIN leaves one for its END.
     * Admission is linearized across producers, so the sole VAD event's END
     * cannot lose its control slot under slow SD operation. No capture waits.
     */
    portENTER_CRITICAL(&s_enqueue_lock);
    bool accepted = recorder_queue_admits(packet->type, uxQueueSpacesAvailable(s_recorder.queue)) &&
        xQueueSend(s_recorder.queue, packet, 0) == pdTRUE;
    if (accepted && s_recorder.config.stats) {
        UBaseType_t waiting = uxQueueMessagesWaiting(s_recorder.queue);
        audio_stats_lock();
        if (waiting > s_recorder.config.stats->sd_queue_high_water)
            s_recorder.config.stats->sd_queue_high_water = waiting;
        audio_stats_unlock();
    }
    portEXIT_CRITICAL(&s_enqueue_lock);
    return accepted;
}
static esp_err_t append_metadata(sd_metadata_kind_t kind, const char *row)
{
    if ((unsigned)kind > SD_METADATA_EVENT_LIFECYCLE) return ESP_ERR_INVALID_ARG;
    char path[96];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", METADATA_FILES[kind]);
    FILE *file = fopen(path, "a+");
    if (!file) return ESP_FAIL;
    esp_err_t error = ESP_OK;
    if (fseek(file, 0, SEEK_END) != 0) error = ESP_FAIL;
    else if (ftell(file) == 0 && fprintf(file, "%s\n", METADATA_HEADERS[kind]) < 0) error = ESP_FAIL;
    if (fprintf(file, "%s\n", row) < 0) error = ESP_FAIL;
    if (fflush(file) != 0) error = ESP_FAIL;
    if (fclose(file) != 0) error = ESP_FAIL;
    return error;
}
static void recover_newest_event(void)
{
    DIR *directory = opendir(MOUNT_POINT);
    if (!directory) return;
    uint64_t newest = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        uint64_t id = 0;
        int used = 0;
        if (strlen(entry->d_name) == 26 &&
            sscanf(entry->d_name, "event_%16" SCNx64 ".wav%n", &id, &used) == 1 &&
            used == 26 && id > newest) newest = id;
    }
    closedir(directory);
    if (!newest) return;
    char path[96];
    snprintf(path, sizeof(path), MOUNT_POINT "/event_%016" PRIX64 ".wav", newest);
    uint64_t repaired = 0;
    esp_err_t error = wav_writer_repair(path, &repaired);
    printf("RECOVERY,%s,%s,%" PRIu64 ",%s\n", error != ESP_OK ? "failed" :
           repaired ? "repaired" : "no_action", path, repaired, esp_err_to_name(error));
}
static esp_err_t mount_card(void)
{
    if (s_recorder.mounted) return ESP_OK;
    const board_audio_pins_t *pins = board_audio_pins();
    s_recorder.host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus = {
        .mosi_io_num = pins->sd_mosi, .miso_io_num = pins->sd_miso,
        .sclk_io_num = pins->sd_sclk, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(s_recorder.host.slot, &bus, SDSPI_DEFAULT_DMA),
                        TAG, "SPI bus initialization failed");
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = pins->sd_cs;
    slot.host_id = s_recorder.host.slot;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        // Export traverses at most 16 nested directories plus one data FILE.
        .format_if_mount_failed = false, .max_files = 20, .allocation_unit_size = 16 * 1024,
    };
    esp_err_t error = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &s_recorder.host, &slot,
                                             &mount, &s_recorder.card);
    if (error != ESP_OK) {
        spi_bus_free(s_recorder.host.slot);
        printf("ERROR,sd_mount,%s\n", esp_err_to_name(error));
        return error;
    }
    s_recorder.mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s", MOUNT_POINT);
    recover_newest_event();
    return ESP_OK;
}
static void finish_event(wav_writer_t *writer, sd_event_result_t *result)
{
    if (!result->event_id) return;
    if (writer->file) {
        if (wav_writer_close(writer) != ESP_OK) result->error = ESP_FAIL;
    } else if (result->error == ESP_OK) result->error = ESP_FAIL;
    result->ready = result->error == ESP_OK;
    char row[METADATA_ROW_BYTES];
    const char *name = strrchr(result->audio_path, '/');
    name = name ? name + 1 : result->audio_path;
    snprintf(row, sizeof(row), "%016" PRIX64 ",%s,RAW,16000,1,16,%" PRIu64
        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRId64 ",%" PRId64 ",%s",
        result->event_id, name, result->audio_first_sample, result->vad_start_sample,
        result->vad_end_sample, result->audio_last_sample,
        result->vad_start_timestamp_us, result->vad_end_timestamp_us,
        result->ready ? "READY" : "FAILED");
    if (append_metadata(SD_METADATA_AUDIO_EVENTS, row) != ESP_OK) {
        result->ready = false;
        result->error = ESP_FAIL;
        if (s_recorder.config.stats) {
            audio_stats_lock(); s_recorder.config.stats->sd_metadata_drops++; audio_stats_unlock();
        }
        printf("ERROR,audio_metadata,event_id,%016" PRIX64 "\n", result->event_id);
    }
    printf("EVENT,audio_terminal,event_id,%016" PRIX64 ",status,%s,path,%s,first,%" PRIu64
        ",last,%" PRIu64 ",error,%s\n", result->event_id, result->ready ? "READY" : "FAILED",
        result->audio_path, result->audio_first_sample, result->audio_last_sample,
        esp_err_to_name(result->error));
    if (s_recorder.config.terminal_cb)
        s_recorder.config.terminal_cb(result, s_recorder.config.callback_ctx);
    memset(result, 0, sizeof(*result));
}
static void recorder_task(void *arg)
{
    (void)arg;
    wav_writer_t writer = {0};
    sd_event_result_t result = {0};
    uint64_t since_flush = 0;
    recorder_packet_t packet;
    while (xQueueReceive(s_recorder.queue, &packet, portMAX_DELAY) == pdTRUE) {
        if (packet.type == PACKET_STOP) break;
        if (packet.type == PACKET_BARRIER) {
            // Metadata append closes each FILE; END closes/finalizes its WAV.
            // Never acknowledge a snapshot while an audio owner remains open.
            portENTER_CRITICAL(&s_enqueue_lock);
            s_recorder.barrier_error = result.event_id || writer.file ? ESP_ERR_INVALID_STATE : ESP_OK;
            s_recorder.barrier_acknowledged = packet.barrier_token;
            portEXIT_CRITICAL(&s_enqueue_lock);
            continue;
        }
        if (packet.type == PACKET_METADATA) {
            if (append_metadata(packet.metadata_kind, packet.data.csv_row) != ESP_OK) {
                if (s_recorder.config.stats) {
                    audio_stats_lock(); s_recorder.config.stats->sd_metadata_drops++; audio_stats_unlock();
                }
                printf("ERROR,sd_metadata,kind,%d\n", packet.metadata_kind);
            }
            continue;
        }
        if (packet.type == PACKET_BEGIN) {
            if (result.event_id) { result.error = ESP_FAIL; finish_event(&writer, &result); }
            result = (sd_event_result_t) {
                .event_id = packet.event_id,
                .audio_first_sample = packet.first_sample_index,
                .audio_last_sample = packet.first_sample_index,
                .vad_start_sample = packet.metadata.trigger_sample,
                .vad_start_timestamp_us = packet.metadata.vad_start_timestamp_us,
                .error = ESP_OK,
            };
            snprintf(result.audio_path, sizeof(result.audio_path), MOUNT_POINT "/event_%016" PRIX64 ".wav",
                     result.event_id);
            struct stat existing;
            if (stat(result.audio_path, &existing) == 0) result.error = ESP_ERR_INVALID_STATE;
            else result.error = wav_writer_open(&writer, result.audio_path, AUDIO_SAMPLE_RATE_HZ, 1, 16);
            if (result.error == ESP_OK && packet.count) {
                result.error = wav_writer_write(&writer, packet.owned_samples, packet.count);
                result.audio_last_sample = result.audio_first_sample + writer.pcm_bytes / sizeof(int16_t);
            }
            if (result.error != ESP_OK && s_recorder.config.stats) {
                audio_stats_lock();
                s_recorder.config.stats->sd_short_writes++;
                s_recorder.config.stats->dropped_samples += packet.count -
                    (result.audio_last_sample - result.audio_first_sample);
                audio_stats_unlock();
            }
            free(packet.owned_samples);
            since_flush = packet.count;
            continue;
        }
        if (packet.event_id != result.event_id) {
            printf("ERROR,sd_event_owner,expected,%016" PRIX64 ",received,%016" PRIX64 "\n",
                   result.event_id, packet.event_id);
            continue;
        }
        if (packet.type == PACKET_END) {
            result.vad_end_sample = packet.first_sample_index;
            result.vad_end_timestamp_us = packet.timestamp_us;
            if (packet.producer_failed) result.error = ESP_FAIL;
            finish_event(&writer, &result);
            continue;
        }
        if (packet.type == PACKET_AUDIO && writer.file) {
            if (packet.first_sample_index != result.audio_last_sample) result.error = ESP_FAIL;
            int64_t started = esp_timer_get_time();
            uint64_t before = writer.pcm_bytes;
            esp_err_t error = wav_writer_write(&writer, packet.data.samples, packet.count);
            uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
            if (s_recorder.config.stats) {
                audio_stats_lock();
                if (elapsed > s_recorder.config.stats->sd_max_write_us)
                    s_recorder.config.stats->sd_max_write_us = elapsed;
                audio_stats_unlock();
            }
            result.audio_last_sample = packet.first_sample_index +
                (writer.pcm_bytes - before) / sizeof(int16_t);
            if (error != ESP_OK) {
                result.error = error;
                if (s_recorder.config.stats) {
                    audio_stats_lock();
                    s_recorder.config.stats->sd_short_writes++;
                    s_recorder.config.stats->dropped_samples += packet.count -
                        (writer.pcm_bytes - before) / sizeof(int16_t);
                    audio_stats_unlock();
                }
            }
            since_flush += packet.count;
            if (since_flush >= AUDIO_SAMPLE_RATE_HZ) {
                if (wav_writer_flush(&writer) != ESP_OK) result.error = ESP_FAIL;
                since_flush = 0;
            }
        } else if (packet.type == PACKET_AUDIO && s_recorder.config.stats) {
            audio_stats_lock(); s_recorder.config.stats->dropped_samples += packet.count; audio_stats_unlock();
        }
    }
    if (result.event_id) { result.error = ESP_FAIL; finish_event(&writer, &result); }
    s_recorder.running = false;
    s_recorder.task = NULL;
    vTaskDelete(NULL);
}
esp_err_t sd_recorder_init(const sd_recorder_config_t *config)
{
    if (!config || s_recorder.queue) return ESP_ERR_INVALID_ARG;
    s_recorder.config = *config;
    const size_t bytes = RECORDER_QUEUE_DEPTH * sizeof(recorder_packet_t);
    /* xQueueCreate allocates both control and payload in internal RAM. Keep
     * the control block internal, but put these task-only PCM packets in PSRAM
     * so camera/BLE startup leaves internal RAM for task stacks and DMA.
     */
    bool in_psram = true;
    s_recorder.queue_storage = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_recorder.queue_storage) {
        in_psram = false;
        s_recorder.queue_storage = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_recorder.queue_storage) {
        ESP_LOGE(TAG, "queue allocation failed: bytes=%u internal_free=%u internal_largest=%u "
                 "psram_free=%u psram_largest=%u",
                 (unsigned)bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    s_recorder.queue = xQueueCreateStatic(RECORDER_QUEUE_DEPTH, sizeof(recorder_packet_t),
                                         s_recorder.queue_storage, &s_recorder.queue_control);
    if (!s_recorder.queue) {
        heap_caps_free(s_recorder.queue_storage);
        s_recorder.queue_storage = NULL;
        ESP_LOGE(TAG, "queue creation failed: bytes=%u", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "queue ready: depth=%u bytes=%u storage=%s", RECORDER_QUEUE_DEPTH,
             (unsigned)bytes, in_psram ? "PSRAM" : "internal");
    return ESP_OK;
}
esp_err_t sd_recorder_start(void)
{
    if (!s_recorder.queue) return ESP_ERR_INVALID_STATE;
    if (s_recorder.running) return ESP_OK;
    ESP_RETURN_ON_ERROR(mount_card(), TAG, "SD unavailable; audio capture remains available");
    s_recorder.running = true;
    if (xTaskCreatePinnedToCore(recorder_task, "sd_writer", 6144, NULL, 8, &s_recorder.task, 1) != pdPASS) {
        s_recorder.running = false; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t sd_recorder_stop(void)
{
    if (!s_recorder.running) return ESP_OK;
    recorder_packet_t packet = {.type = PACKET_STOP};
    if (xQueueSend(s_recorder.queue, &packet, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    for (int i = 0; s_recorder.running && i < 500; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    return s_recorder.running ? ESP_ERR_TIMEOUT : ESP_OK;
}
bool sd_recorder_is_running(void) { return s_recorder.running; }
bool sd_recorder_is_mounted(void) { return s_recorder.mounted; }
esp_err_t sd_recorder_fatfs_drive(char *destination, size_t capacity)
{
    if (!destination || capacity < 5) return ESP_ERR_INVALID_ARG;
    if (!s_recorder.mounted || !s_recorder.card) return ESP_ERR_INVALID_STATE;
    unsigned drive = ff_diskio_get_pdrv_card(s_recorder.card);
    if (drive == 0xff) return ESP_ERR_INVALID_STATE;
    int length = snprintf(destination, capacity, "%u:", drive);
    return length > 0 && (size_t)length < capacity ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t sd_recorder_flush_barrier(uint32_t timeout_ms)
{
    if (!s_recorder.running || !s_recorder.mounted) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_enqueue_lock);
    uint32_t token = ++s_recorder.barrier_requested;
    portEXIT_CRITICAL(&s_enqueue_lock);
    recorder_packet_t packet = {.type = PACKET_BARRIER, .barrier_token = token};
    if (xQueueSend(s_recorder.queue, &packet, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    for (uint32_t waited = 0; waited <= timeout_ms; waited += 10) {
        portENTER_CRITICAL(&s_enqueue_lock);
        bool acknowledged = s_recorder.barrier_acknowledged == token;
        esp_err_t error = s_recorder.barrier_error;
        portEXIT_CRITICAL(&s_enqueue_lock);
        if (acknowledged) return error;
        if (!s_recorder.running || timeout_ms - waited < 10) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}
bool sd_recorder_event_begin(const audio_frame_t *buffered, const sd_event_metadata_t *metadata)
{
    if (!s_recorder.running || !buffered || !metadata || !metadata->event_id ||
        (!buffered->samples && buffered->sample_count)) return false;
    size_t bytes = buffered->sample_count * sizeof(int16_t);
    int16_t *copy = bytes ? heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!copy && bytes) copy = malloc(bytes);
    if (!copy && bytes) return false;
    if (bytes) memcpy(copy, buffered->samples, bytes);
    recorder_packet_t packet = {.type = PACKET_BEGIN, .event_id = metadata->event_id,
        .first_sample_index = buffered->first_sample_index, .count = buffered->sample_count,
        .owned_samples = copy, .metadata = *metadata};
    if (!enqueue(&packet)) { free(copy); return false; }
    return true;
}
bool sd_recorder_event_submit(uint64_t event_id, const audio_frame_t *frame)
{
    if (!frame || !frame->samples) return false;
    size_t offset = 0;
    while (offset < frame->sample_count) {
        size_t count = frame->sample_count - offset;
        if (count > RECORDER_PACKET_SAMPLES) count = RECORDER_PACKET_SAMPLES;
        recorder_packet_t packet = {.type = PACKET_AUDIO, .event_id = event_id,
            .first_sample_index = frame->first_sample_index + offset, .count = count};
        memcpy(packet.data.samples, frame->samples + offset, count * sizeof(int16_t));
        if (!enqueue(&packet)) {
            if (s_recorder.config.stats) {
                audio_stats_lock();
                s_recorder.config.stats->queue_overruns++;
                s_recorder.config.stats->dropped_samples += frame->sample_count - offset;
                audio_stats_unlock();
            }
            printf("ERROR,event_queue_overrun,event_id,%016" PRIX64 ",sample,%" PRIu64 "\n",
                   event_id, frame->first_sample_index + offset);
            return false;
        }
        offset += count;
    }
    return true;
}
bool sd_recorder_event_end(uint64_t id, int64_t timestamp, uint64_t sample, bool failed)
{
    recorder_packet_t packet = {.type = PACKET_END, .event_id = id,
        .first_sample_index = sample, .timestamp_us = timestamp, .producer_failed = failed};
    return enqueue(&packet);
}
bool sd_recorder_metadata_append(sd_metadata_kind_t kind, const char *row)
{
    if (!row || (unsigned)kind > SD_METADATA_EVENT_LIFECYCLE || strlen(row) >= METADATA_ROW_BYTES) return false;
    recorder_packet_t packet = {.type = PACKET_METADATA, .metadata_kind = kind};
    strlcpy(packet.data.csv_row, row, sizeof(packet.data.csv_row));
    if (enqueue(&packet)) return true;
    if (s_recorder.config.stats) {
        audio_stats_lock(); s_recorder.config.stats->sd_metadata_drops++; audio_stats_unlock();
    }
    printf("ERROR,sd_metadata_queue,kind,%d\n", kind);
    return false;
}
void sd_recorder_deinit(void)
{
    if (sd_recorder_stop() != ESP_OK) return;
    if (s_recorder.mounted) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_recorder.card);
        spi_bus_free(s_recorder.host.slot);
    }
    if (s_recorder.queue) vQueueDelete(s_recorder.queue);
    heap_caps_free(s_recorder.queue_storage);
    memset(&s_recorder, 0, sizeof(s_recorder));
}
