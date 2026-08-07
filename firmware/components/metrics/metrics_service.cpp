#include "metrics_service.hpp"

#include <algorithm>
#include <cmath>
#include "esp_log.h"

namespace demo {
void MetricsService::emit(uint32_t image_id, const char* metric, double value, const char* unit) const {
    ESP_LOGI("METRIC", "[METRIC] phase=%u image_id=%lu metric=%s value=%.3f unit=%s",
             phase_, static_cast<unsigned long>(image_id), metric, value, unit);
}
void MetricsService::add_rssi(int8_t v) {
    rssi_current_ = v;
    rssi_count_++; rssi_sum_ += v; rssi_sum_sq_ += static_cast<double>(v) * v;
    rssi_min_ = std::min(rssi_min_, v); rssi_max_ = std::max(rssi_max_, v);
}
void MetricsService::log_rssi(uint32_t image_id) const {
    if (!rssi_count_) return;
    const double mean = rssi_sum_ / rssi_count_;
    const double variance = std::max(0.0, rssi_sum_sq_ / rssi_count_ - mean * mean);
    emit(image_id, "rssi_current_dbm", rssi_current_, "dBm");
    emit(image_id, "rssi_min_dbm", rssi_min_, "dBm"); emit(image_id, "rssi_max_dbm", rssi_max_, "dBm");
    emit(image_id, "rssi_mean_dbm", mean, "dBm"); emit(image_id, "rssi_stddev_dbm", std::sqrt(variance), "dBm");
}
void MetricsService::reset_rssi() { rssi_count_ = 0; rssi_sum_ = rssi_sum_sq_ = 0; rssi_current_ = 0; rssi_min_ = 127; rssi_max_ = -127; }
}  // namespace demo
