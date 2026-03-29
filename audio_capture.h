#pragma once
#include <cstdint>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
//  AudioCapture — wraps miniaudio device initialisation and the capture
//  callback.  node_A constructs one instance and calls start(); the provided
//  OnFrames callback is invoked from the audio thread for every period.
//
//  Responsibilities:
//    • Probe the hardware's native sample rate (avoids OS resampling).
//    • Open the WASAPI-exclusive (Windows) or default (other OS) capture device.
//    • Invoke the OnFrames callback with raw 32-bit float PCM every ~10 ms.
//    • Log device name, native formats, and chosen rate to stdout.
//
//  Thread safety:
//    OnFrames is called from miniaudio's internal audio thread.  The callback
//    must not block, allocate heap memory, or acquire contended mutexes.
// ─────────────────────────────────────────────────────────────────────────────

// Forward-declare the miniaudio device type so the header stays self-contained
// without pulling in all of miniaudio.h everywhere.
struct ma_device;

// Callback type:  (pcm_data, frame_count, sample_rate_hz)
using OnFramesCallback = std::function<void(const float*, uint32_t, uint32_t)>;

class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture();

    // Non-copyable / non-movable — owns a live device handle
    AudioCapture(const AudioCapture&)            = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // Probe the default capture device and return its highest suitable sample
    // rate (capped at 96 kHz).  Call this before start() so callers can log
    // the chosen rate.
    static uint32_t probe_native_sample_rate();

    // Open and start the capture device.  Returns false on failure.
    // sample_rate must be the value returned by probe_native_sample_rate().
    bool start(uint32_t sample_rate, OnFramesCallback cb);

    // Stop and release the device.  Safe to call even if start() failed.
    void stop();

    uint32_t sample_rate() const { return sample_rate_; }

private:
    ma_device*       device_      = nullptr;
    uint32_t         sample_rate_ = 0;
    OnFramesCallback on_frames_;

    // miniaudio data callback — static trampoline into on_frames_
    static void ma_callback(ma_device* dev, void* out,
                            const void* in, uint32_t frames);
};
