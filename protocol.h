#pragma once
#include <cstdint>
#include <vector>
#include <chrono>
#include <mutex>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
//  Wire protocol – version 2
//
//   Offset  Size  Field
//   ──────  ────  ──────────────────────────────────────────────────────────
//      0      4   magic           0x41554431  ("AUD1")
//      4      4   sequence_number per-session monotonic counter
//      8      8   timestamp_us    sender wall-clock (µs since Unix epoch)
//     16      8   session_start_us wall-clock when Node A started
//     24      4   session_id      random 32-bit ID generated at Node A startup
//     28      4   clip_index      incremented each time Node B reconnects
//     32      4   sample_rate     Hz — authoritative, read by Node B at runtime
//     36      2   channels        1 = mono, 2 = stereo
//     38      2   bits_per_sample 16 or 32
//     40      2   payload_bytes   byte length of the PCM block that follows
//     42      2   (reserved / padding – zero)
//   ──────  ────
//     44 bytes total header
//
//  The raw PCM samples immediately follow the header.
// ─────────────────────────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct AudioPacket {
    uint32_t magic;
    uint32_t sequence_number;
    uint64_t timestamp_us;
    uint64_t session_start_us;
    uint32_t session_id;
    uint32_t clip_index;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t payload_bytes;
    uint16_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(AudioPacket) == 44, "AudioPacket size mismatch");

constexpr uint32_t AUDIO_PACKET_MAGIC = 0x41554431u; // "AUD1"

inline uint64_t get_current_time_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

inline std::string timestamp_for_filename(uint64_t epoch_us) {
    time_t secs = static_cast<time_t>(epoch_us / 1'000'000ULL);
    struct tm t{};
#ifdef _WIN32
    gmtime_s(&t, &secs);
#else
    gmtime_r(&secs, &t);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H-%M-%SZ", &t);
    return buf;
}

class AudioQueue {
    std::vector<std::vector<uint8_t>> buffer;
    size_t head = 0, tail = 0, count = 0, capacity;
    std::mutex mtx;
public:
    explicit AudioQueue(size_t cap = 200) : capacity(cap), buffer(cap) {}

    bool push(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(mtx);
        if (count == capacity) return false;
        buffer[tail] = data;
        tail = (tail + 1) % capacity;
        ++count;
        return true;
    }

    bool pop(std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(mtx);
        if (count == 0) return false;
        data = buffer[head];
        head = (head + 1) % capacity;
        --count;
        return true;
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return count;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        head = tail = count = 0;
    }
};