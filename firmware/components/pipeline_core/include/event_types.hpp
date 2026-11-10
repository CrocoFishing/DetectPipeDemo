#pragma once

#include <cstdint>

namespace demo {
inline constexpr int64_t EVENT_FACE_INTERVAL_US = 30'000'000;
inline constexpr int64_t EVENT_MAX_DURATION_US = 300'000'000;
inline constexpr uint8_t CLIENT_NO_FACE_MAX_RECAPTURES = 3;
enum class AudioStatus : uint8_t { Pending = 0, Ready = 1, Failed = 2 };
enum class FaceBatchStatus : uint8_t {
    Pending = 0, Completed = 1, NoFace = 2, Partial = 3,
    Busy = 4, Timeout = 5, Disconnected = 6, Failed = 7,
};
enum class EventState : uint8_t { Open, Waiting, Complete, CompleteWithError };

struct FaceBatchResult {
    uint64_t event_id{0};
    uint32_t request_id{0};
    uint16_t expected_faces{0}, completed_faces{0};
    uint16_t recognized_count{0}, unknown_count{0}, failed_count{0};
    FaceBatchStatus status{FaceBatchStatus::Pending};
    bool client_no_face{false};
};
struct FaceAssociation {
    uint64_t event_id{0};
    uint32_t request_id{0}, image_id{0};
    uint16_t face_index{0}, face_count{0};
    uint8_t status{0};
    char person_id[33]{};
    float similarity{0};
    uint32_t processing_time_ms{0};
};
struct EventContext {
    uint64_t event_id{0};
    uint64_t vad_start_sample{0}, vad_end_sample{0};
    int64_t vad_start_timestamp_us{0}, vad_end_timestamp_us{0};
    uint64_t audio_first_sample{0}, audio_last_sample{0}; // exclusive last boundary
    uint32_t request_id{0};
    uint16_t expected_face_count{0}, completed_face_count{0};
    AudioStatus audio_status{AudioStatus::Pending};
    FaceBatchStatus face_status{FaceBatchStatus::Pending};
    EventState state{EventState::Open};
    bool vad_ended{false};
    int64_t next_face_timestamp_us{0};
    uint16_t face_batch_count{0};
    uint8_t face_retry_count{0};
    bool face_had_error{false};
    bool face_retry_pending{false};
};
inline const char* audio_status_name(AudioStatus s) {
    switch (s) { case AudioStatus::Pending: return "PENDING";
        case AudioStatus::Ready: return "READY"; case AudioStatus::Failed: return "FAILED"; }
    return "FAILED";
}
inline bool face_slot_due(EventContext& c, int64_t now) {
    if (c.vad_ended || now >= c.vad_start_timestamp_us + EVENT_MAX_DURATION_US ||
        now < c.next_face_timestamp_us) return false;
    do { c.next_face_timestamp_us += EVENT_FACE_INTERVAL_US; }
    while (c.next_face_timestamp_us <= now);
    return true; // A busy camera skips this slot; never accumulate a backlog.
}
inline void begin_face_batch(EventContext& c, uint32_t request_id, bool retry) {
    c.request_id = request_id;
    c.face_status = FaceBatchStatus::Pending;
    c.expected_face_count = c.completed_face_count = 0;
    c.face_retry_pending = false;
    c.face_retry_count = retry ? c.face_retry_count + 1 : 0;
    ++c.face_batch_count;
}
inline const char* face_status_name(FaceBatchStatus s) {
    switch (s) { case FaceBatchStatus::Pending: return "PENDING";
        case FaceBatchStatus::Completed: return "COMPLETED"; case FaceBatchStatus::NoFace: return "NO_FACE";
        case FaceBatchStatus::Partial: return "PARTIAL"; case FaceBatchStatus::Busy: return "BUSY";
        case FaceBatchStatus::Timeout: return "TIMEOUT"; case FaceBatchStatus::Disconnected: return "DISCONNECTED";
        case FaceBatchStatus::Failed: return "FAILED"; }
    return "FAILED";
}
// Terminal failure preserves the other branch. UNKNOWN is included in completed_faces.
inline bool refresh_event_state(EventContext& c, uint16_t failed_faces = 0) {
    if (!c.vad_ended || c.audio_status == AudioStatus::Pending ||
        c.face_status == FaceBatchStatus::Pending) return false;
    const bool ok = c.audio_status == AudioStatus::Ready && !c.face_had_error && failed_faces == 0 &&
        (c.face_status == FaceBatchStatus::Completed || c.face_status == FaceBatchStatus::NoFace);
    c.state = ok ? EventState::Complete : EventState::CompleteWithError;
    return true;
}
inline bool apply_audio_terminal(EventContext& c, uint64_t event_id, AudioStatus status,
                                 uint64_t first_sample, uint64_t last_sample) {
    if (event_id != c.event_id || c.audio_status != AudioStatus::Pending ||
        (status != AudioStatus::Ready && status != AudioStatus::Failed) || first_sample > last_sample) return false;
    c.audio_status = status;
    c.audio_first_sample = first_sample;
    c.audio_last_sample = last_sample;
    return true;
}
inline bool apply_vad_end(EventContext& c, uint64_t event_id, int64_t timestamp_us, uint64_t sample) {
    if (event_id != c.event_id || c.state == EventState::Complete || c.state == EventState::CompleteWithError ||
        sample < c.vad_start_sample || timestamp_us < c.vad_start_timestamp_us) return false;
    if (c.vad_ended) return c.vad_end_sample == sample && c.vad_end_timestamp_us == timestamp_us;
    c.vad_end_sample = sample;
    c.vad_end_timestamp_us = timestamp_us;
    c.vad_ended = true;
    c.state = EventState::Waiting;
    return true;
}
inline bool apply_face_terminal(EventContext& c, const FaceBatchResult& result) {
    if (result.event_id != c.event_id || result.request_id != c.request_id ||
        c.face_status != FaceBatchStatus::Pending || result.status == FaceBatchStatus::Pending ||
        static_cast<uint8_t>(result.status) > static_cast<uint8_t>(FaceBatchStatus::Failed) ||
        result.completed_faces > result.expected_faces ||
        static_cast<uint32_t>(result.recognized_count) + result.unknown_count + result.failed_count !=
            result.completed_faces) return false;
    if (result.status == FaceBatchStatus::Completed &&
        (!result.expected_faces || result.completed_faces != result.expected_faces)) return false;
    if (result.status == FaceBatchStatus::NoFace && (result.expected_faces || result.completed_faces)) return false;
    c.expected_face_count = result.expected_faces;
    c.completed_face_count = result.completed_faces;
    c.face_status = result.status;
    c.face_retry_pending = result.client_no_face && !c.vad_ended &&
        c.face_retry_count < CLIENT_NO_FACE_MAX_RECAPTURES;
    c.face_had_error = c.face_had_error || result.failed_count != 0 ||
        (result.status != FaceBatchStatus::Completed && result.status != FaceBatchStatus::NoFace);
    refresh_event_state(c, result.failed_count);
    return true;
}
} // namespace demo
