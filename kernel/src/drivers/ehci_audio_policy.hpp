/* Phase 24: bounded USB Audio Class high-speed isochronous policy. */
#pragma once
#include "ehci_spec.hpp"

namespace starlix::drivers::ehci::audio_policy {
constexpr uint8_t kAudioClass = 0x01u;
constexpr uint8_t kAudioStreamingSubclass = 0x02u;
constexpr uint8_t kIsochronousTransferType = 0x01u;
constexpr uint32_t kMinSampleRateHz = 8000u;
constexpr uint32_t kMaxSampleRateHz = 192000u;
constexpr uint8_t kMaxChannels = 8u;
constexpr uint16_t kOneTransactionMax = 1024u;
constexpr uint32_t kQueueSlots = 4u;

enum class Direction : uint8_t { Out = 0, In = 1 };
enum class SyncType : uint8_t { NoSync = 0, Async = 1, Adaptive = 2, Sync = 3 };

struct StreamConfig {
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t alternate_setting;
    uint8_t endpoint_address;
    uint8_t transfer_type;
    uint8_t interval;
    uint16_t encoded_max_packet;
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t subslot_bytes;
    uint8_t bit_resolution;
    SyncType sync_type;
    bool feedback_endpoint_valid;
};

constexpr uint32_t samples_per_microframe_ceil(uint32_t rate) {
    return rate == 0u ? 0u : (rate + 7999u) / 8000u;
}
constexpr uint32_t bytes_per_audio_microframe(uint32_t rate, uint8_t channels,
                                              uint8_t subslot_bytes) {
    return samples_per_microframe_ceil(rate) * static_cast<uint32_t>(channels) * subslot_bytes;
}
constexpr bool valid_stream(const StreamConfig& c, Direction direction) {
    if (c.interface_class != kAudioClass || c.interface_subclass != kAudioStreamingSubclass ||
        c.alternate_setting == 0u || (c.endpoint_address & 0x0Fu) == 0u ||
        c.transfer_type != kIsochronousTransferType || c.interval < 1u || c.interval > 4u ||
        c.sample_rate_hz < kMinSampleRateHz || c.sample_rate_hz > kMaxSampleRateHz ||
        c.channels == 0u || c.channels > kMaxChannels || c.subslot_bytes == 0u ||
        c.subslot_bytes > 4u || c.bit_resolution == 0u ||
        c.bit_resolution > static_cast<uint8_t>(c.subslot_bytes * 8u)) return false;
    const uint16_t payload = static_cast<uint16_t>(c.encoded_max_packet & 0x07FFu);
    const uint16_t transactions = static_cast<uint16_t>((c.encoded_max_packet >> 11u) & 3u);
    if (payload == 0u || payload > kOneTransactionMax || transactions != 0u) return false;
    const bool endpoint_in = (c.endpoint_address & 0x80u) != 0u;
    if (endpoint_in != (direction == Direction::In)) return false;
    if (bytes_per_audio_microframe(c.sample_rate_hz, c.channels, c.subslot_bytes) > payload) return false;
    /* Asynchronous playback needs a valid feedback endpoint to keep sample clocking stable. */
    if (direction == Direction::Out && c.sync_type == SyncType::Async && !c.feedback_endpoint_valid) return false;
    return true;
}

struct PacketSlot { uint8_t bytes[kOneTransactionMax]; uint16_t length; uint32_t generation; };
struct PacketQueue { PacketSlot slots[kQueueSlots]; volatile uint32_t head; volatile uint32_t tail; volatile uint32_t dropped; };
inline void queue_init(PacketQueue* q) {
    if (!q) return;
    __atomic_store_n(&q->head, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&q->tail, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&q->dropped, 0u, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < kQueueSlots; ++i) { q->slots[i].length = 0u; q->slots[i].generation = 0u; }
}
inline bool enqueue(PacketQueue* q, const uint8_t* data, uint32_t length, uint32_t generation) {
    if (!q || !data || length == 0u || length > kOneTransactionMax || generation == 0u) return false;
    const uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    const uint32_t next = (head + 1u) % kQueueSlots;
    const uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    if (next == tail) { __atomic_add_fetch(&q->dropped, 1u, __ATOMIC_RELAXED); return false; }
    for (uint32_t i = 0; i < length; ++i) q->slots[head].bytes[i] = data[i];
    q->slots[head].length = static_cast<uint16_t>(length);
    q->slots[head].generation = generation;
    __atomic_store_n(&q->head, next, __ATOMIC_RELEASE);
    return true;
}
inline bool dequeue(PacketQueue* q, uint8_t* out, uint32_t capacity, uint32_t generation, uint16_t* length) {
    if (length) *length = 0u;
    if (!q || !out || !length || generation == 0u) return false;
    const uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
    const uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    if (tail == head) return false;
    PacketSlot& slot = q->slots[tail];
    if (slot.generation != generation) { slot.length = 0u; slot.generation = 0u; __atomic_store_n(&q->tail, (tail + 1u) % kQueueSlots, __ATOMIC_RELEASE); __atomic_add_fetch(&q->dropped, 1u, __ATOMIC_RELAXED); return false; }
    if (slot.length > capacity) return false;
    for (uint32_t i = 0; i < slot.length; ++i) out[i] = slot.bytes[i];
    *length = slot.length; slot.length = 0u; slot.generation = 0u;
    __atomic_store_n(&q->tail, (tail + 1u) % kQueueSlots, __ATOMIC_RELEASE); return true;
}
constexpr bool self_test() {
    const StreamConfig playback{1u,2u,1u,0x01u,1u,1u,192u,48000u,2u,2u,16u,SyncType::Async,true};
    const StreamConfig capture{1u,2u,1u,0x81u,1u,1u,192u,48000u,2u,2u,16u,SyncType::Sync,false};
    const StreamConfig no_feedback{1u,2u,1u,0x01u,1u,1u,192u,48000u,2u,2u,16u,SyncType::Async,false};
    if (!valid_stream(playback,Direction::Out) || !valid_stream(capture,Direction::In) || valid_stream(capture,Direction::Out) || valid_stream(no_feedback,Direction::Out)) return false;
    StreamConfig bad = playback; bad.sample_rate_hz = 384000u; if (valid_stream(bad,Direction::Out)) return false;
    bad = playback; bad.encoded_max_packet = static_cast<uint16_t>(1024u | (1u << 11u)); if (valid_stream(bad,Direction::Out)) return false;
    bad = playback; bad.bit_resolution = 25u; if (valid_stream(bad,Direction::Out)) return false;
    if (bytes_per_audio_microframe(48000u,2u,2u) != 24u || samples_per_microframe_ceil(44100u) != 6u) return false;
    static PacketQueue q{}; queue_init(&q); uint8_t packet[4]={1u,2u,3u,4u}, out[4]{}; uint16_t n=0u;
    if (!enqueue(&q,packet,4u,7u) || !dequeue(&q,out,4u,7u,&n) || n!=4u || out[3]!=4u) return false;
    if (!enqueue(&q,packet,4u,8u) || dequeue(&q,out,4u,9u,&n) || __atomic_load_n(&q.dropped,__ATOMIC_RELAXED)!=1u) return false;
    return !enqueue(&q,packet,4u,0u) && !enqueue(&q,packet,1025u,7u);
}
} // namespace starlix::drivers::ehci::audio_policy
