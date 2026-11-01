#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef enum {
    PACKET_BEGIN, PACKET_AUDIO, PACKET_END, PACKET_METADATA, PACKET_STOP, PACKET_BARRIER
} packet_type_t;
/* One VAD producer owns BEGIN/END. All producers check this policy inside the
 * shared enqueue critical section; no other producer may spend its END slot.
 */
static inline bool recorder_queue_admits(packet_type_t type, size_t free_slots)
{
    size_t reserve = type == PACKET_BEGIN ? 1 :
        (type == PACKET_AUDIO || type == PACKET_METADATA) ? 2 : 0;
    return free_slots > reserve;
}
