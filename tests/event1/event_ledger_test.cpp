#include <cassert>
#include <set>
#include "event_ledger.hpp"

using namespace demo;
int main() {
    std::set<uint64_t> ids;
    for (uint32_t boot = 37; boot <= 38; ++boot) {
        EventLedger ledger(boot);
        for (unsigned i = 0; i < 100; ++i) {
            auto* c = ledger.open(1000 + i, 16000 + i * 16000);
            assert(c && ids.insert(c->event_id).second);
            assert((c->event_id >> 32) == boot);
            c->request_id = i + 1;
            FaceBatchResult b{c->event_id, c->request_id, 3, 3, 2, 1, 0, FaceBatchStatus::Completed};
            auto stale = b;
            stale.request_id++;
            assert(!apply_face_terminal(*c, stale));
            stale = b;
            stale.event_id++;
            assert(!apply_face_terminal(*c, stale));
            if (i % 2) {
                c->audio_status = AudioStatus::Ready;
                assert(!refresh_event_state(*c));
                assert(apply_face_terminal(*c, b));
            } else {
                assert(apply_face_terminal(*c, b));
                assert(!apply_face_terminal(*c, b));
                assert(!refresh_event_state(*c));
                c->audio_status = AudioStatus::Ready;
                assert(refresh_event_state(*c));
            }
            assert(c->state == EventState::Complete);
            auto id = c->event_id;
            assert(ledger.release(id));
            assert(!ledger.find(id));
        }
        assert(ledger.active_count() == 0);
    }
    EventLedger ledger(39);
    auto* raced = ledger.open(100, 100);
    assert(raced);
    const auto raced_id = raced->event_id;
    assert(!apply_audio_terminal(*raced, raced_id + 1, AudioStatus::Ready, 1, 200));
    assert(!apply_audio_terminal(*raced, raced_id, AudioStatus::Ready, 201, 200));
    assert(apply_vad_end(*raced, raced_id, 200, 200));
    assert(apply_vad_end(*raced, raced_id, 200, 200)); // exact duplicate END is idempotent
    assert(!apply_vad_end(*raced, raced_id, 201, 200));
    assert(apply_audio_terminal(*raced, raced_id, AudioStatus::Ready, 1, 201));
    // A second ingress while the first publish yielded must not overwrite its terminal.
    assert(!apply_audio_terminal(*raced, raced_id, AudioStatus::Failed, 1, 100));
    assert(raced->audio_status == AudioStatus::Ready && raced->audio_last_sample == 201);
    FaceBatchResult no_face{raced_id, 0, 0, 0, 0, 0, 0, FaceBatchStatus::NoFace};
    auto malformed = no_face;
    malformed.expected_faces = 1;
    assert(!apply_face_terminal(*raced, malformed));
    assert(apply_face_terminal(*raced, no_face));
    assert(raced->state == EventState::Complete);
    assert(!apply_vad_end(*raced, raced_id, 200, 200)); // never regress COMPLETE to WAITING
    assert(ledger.release(raced_id));
    for (auto status : {FaceBatchStatus::NoFace, FaceBatchStatus::Busy, FaceBatchStatus::Timeout,
                        FaceBatchStatus::Disconnected, FaceBatchStatus::Failed, FaceBatchStatus::Partial}) {
        auto* c = ledger.open(1, 2);
        assert(c);
        FaceBatchResult b{c->event_id, 0, 0, 0, 0, 0, 0, status};
        if (status == FaceBatchStatus::Partial) { b.expected_faces = 3; b.completed_faces = 2; b.recognized_count = 2; }
        assert(apply_face_terminal(*c, b));
        c->audio_status = AudioStatus::Ready;
        assert(refresh_event_state(*c));
        assert(c->state == (status == FaceBatchStatus::NoFace ? EventState::Complete : EventState::CompleteWithError));
        assert(ledger.release(c->event_id));
    }
    auto* failed = ledger.open(1, 2);
    failed->audio_status = AudioStatus::Failed;
    assert(apply_face_terminal(*failed, {failed->event_id, 0, 1, 1, 1, 0, 0, FaceBatchStatus::Completed}));
    assert(failed->completed_face_count == 1 && failed->state == EventState::CompleteWithError);
    assert(ledger.release(failed->event_id));
    for (size_t i = 0; i < EventLedger::Capacity; ++i) assert(ledger.open(0, 0));
    assert(!ledger.open(0, 0)); // explicit bounded overflow, never overwrite an owner
    assert(ledger.active_count() == EventLedger::Capacity);
    EventLedger invalid;
    assert(!invalid.open(0, 0));
}
