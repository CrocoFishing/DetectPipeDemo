#pragma once

#include <cstdint>

namespace demo {
class MetricsService {
public:
    explicit MetricsService(uint8_t phase) : phase_(phase) {}
    void emit(uint32_t image_id, const char* metric, double value, const char* unit) const;
    void add_rssi(int8_t dbm);
    void log_rssi(uint32_t image_id) const;
    void reset_rssi();
private:
    uint8_t phase_;
    uint32_t rssi_count_{0};
    double rssi_sum_{0};
    double rssi_sum_sq_{0};
    int8_t rssi_current_{0};
    int8_t rssi_min_{127};
    int8_t rssi_max_{-127};
};
}  // namespace demo
