#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include "network_shims.hpp"
#include "application_protocol.hpp"
// Expose the production service state to inject hardware shutdown/read failures.
#define private public
#pragma warning(push)
#pragma warning(disable: 4458) // Existing local FILINFO shadows the BLE info member.
#include "../../firmware/components/wifi_sd_export/wifi_sd_export_service.cpp"
#pragma warning(pop)
#undef private

namespace protocol { uint32_t crc32(const uint8_t*, size_t) { return 0; } }
using protocol::SdExportState;
using protocol::ErrorCode;
using State = demo::WifiSdExportService::State;
static unsigned resumed = 0;
static esp_err_t resume(void*) {
    // No task may read SD after snapshot ownership returns to the recorder.
    assert(frees == 1 && wifi_stops == 1 && netif_destroys == 1);
    ++resumed; return ESP_OK;
}
static esp_err_t notify(void*, uint32_t, const protocol::SdExportStatusPayload&) { return ESP_OK; }

int main() {
    State state;
    state.hooks.resume = resume; state.hooks.notify = notify;
    state.preparation_attempted = true;
    assert(state.start_network() == ESP_OK);
    assert(started_config.stack_size == 8192);
    assert(started_config.task_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    const auto server = state.server;
    const auto buffer = state.io_buffer;
    stop_result = ESP_FAIL;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        state.cleanup(SdExportState::Cancelled, ErrorCode::None);
        assert(state.network_stop_pending && state.server == server && state.io_buffer == buffer);
        assert(state.snapshot().state == SdExportState::Restoring);
        assert(state.snapshot().error == ErrorCode::Internal);
        assert(state.preparation_attempted && resumed == 0 && frees == 0 && wifi_stops == 0);
    }
    stop_result = ESP_OK;
    state.cleanup(state.pending_terminal, state.pending_error);
    assert(stops == 4 && resumed == 1 && frees == 1);
    assert(!state.network_stop_pending && !state.server && !state.io_buffer);
    assert(!state.preparation_attempted && state.snapshot().state == SdExportState::Cancelled);
    assert(state.snapshot().error == ErrorCode::None);
    // Retrying a completed cleanup must not double-stop, double-free or resume.
    state.cleanup(SdExportState::Cancelled, ErrorCode::None);
    assert(stops == 4 && resumed == 1 && frees == 1);

    State reader;
    uint8_t read_buffer[512]{};
    reader.io_buffer = read_buffer;
    FIL source;
    UINT count = 0;
    assert(reader.read_file(source, 512, count) && count == 512);
    short_read = true;
    assert(!reader.read_file(source, 512, count));
    assert(reader.fatal_error == static_cast<uint16_t>(ErrorCode::SdIo) && reader.stop_readers);
    short_read = false; read_result = FR_DISK_ERR;
    reader.fatal_error = 0; reader.stop_readers = false;
    assert(!reader.read_file(source, 512, count) && count == 0);
    assert(reader.fatal_error == static_cast<uint16_t>(ErrorCode::SdIo) && reader.stop_readers);
    std::puts("PASS: production HTTP shutdown retry retains SD ownership, releases once before resume; short/error reads fail export");
}
