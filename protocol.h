#pragma once
#include <cstdint>
#include <vector>
#include <chrono>
#include <mutex>

// Ensures the compiler doesn't pad the struct, keeping it network-ready
#pragma pack(push, 1)
struct AudioPacket {
    uint32_t sequence_number;
    uint64_t timestamp_us; // For latency measurement
    uint32_t sample_rate;
    uint16_t payload_bytes;
    // The actual raw PCM audio data will be appended immediately after this header
};
#pragma pack(pop)

// Helper to get microsecond timestamps for latency tracking
inline uint64_t get_current_time_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

// A basic thread-safe Ring Buffer for the prototype.
// NOTE: For a true production system, replace this with a lock-free SPSC queue 
// (like moodycamel) to guarantee the audio thread is never blocked.
class AudioQueue {
    std::vector<std::vector<uint8_t>> buffer;
    size_t head = 0, tail = 0, count = 0, capacity;
    std::mutex mtx;

public:
    AudioQueue(size_t cap = 100) : capacity(cap), buffer(cap) {}

    bool push(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(mtx);
        if (count == capacity) return false; // Queue full (Drop packet to maintain realtime)
        buffer[tail] = data;
        tail = (tail + 1) % capacity;
        count++;
        return true;
    }

    bool pop(std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(mtx);
        if (count == 0) return false; // Queue empty (Underflow)
        data = buffer[head];
        head = (head + 1) % capacity;
        count--;
        return true;
    }
};