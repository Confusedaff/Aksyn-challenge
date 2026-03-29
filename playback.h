#pragma once
#include <cstdint>
#include <functional>
#include <atomic>

struct ma_device;

// ─────────────────────────────────────────────────────────────────────────────
//  Playback — wraps the miniaudio playback device.
//
//  The device requests audio from a caller-supplied PullCallback every
//  ~10 ms.  The callback must fill exactly `frame_count` frames of 32-bit
//  float mono audio into `output`.  It is called from the audio thread and
//  MUST NOT block.
//
//  Adaptive jitter-buffer gating lives inside the Receiver; Playback just
//  exposes the device and calls back to ask for data.
// ─────────────────────────────────────────────────────────────────────────────

// Callback:  (output_buffer, frame_count)  — fill `frame_count` f32 frames
using PullCallback = std::function<void(float*, uint32_t)>;

class Playback {
public:
    Playback() = default;
    ~Playback();

    Playback(const Playback&)            = delete;
    Playback& operator=(const Playback&) = delete;

    // Open and start the playback device at the given sample rate.
    bool start(uint32_t sample_rate, PullCallback cb);

    // Stop and release the device.
    void stop();

    uint32_t sample_rate() const { return sample_rate_; }

private:
    ma_device*   device_      = nullptr;
    uint32_t     sample_rate_ = 0;
    PullCallback pull_cb_;

    static void ma_callback(ma_device* dev, void* out,
                            const void* in, uint32_t frames);
};
