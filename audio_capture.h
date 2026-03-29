#pragma once
#include <cstdint>
#include <functional>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  AudioCapture — wraps miniaudio device initialisation and the capture
//  callback.  node_A constructs one instance and calls start(); the provided
//  OnFrames callback is invoked from the audio thread for every period.
//
//  Responsibilities:
//    • Probe the hardware's native sample rate (avoids OS resampling).
//    • Open the WASAPI-exclusive (Windows) or default (other OS) capture device.
//    • Run RNNoise suppression on every captured period (when available).
//    • Invoke the OnFrames callback with clean 32-bit float PCM every ~10 ms.
//    • Log device name, native formats, chosen rate, and RNNoise status.
//
//  RNNoise notes:
//    RNNoise expects exactly 480 f32 frames at 48 kHz per call.
//    At 48 kHz our 10 ms period is exactly 480 frames — a perfect match.
//    At other rates (44100, 96000, etc.) captured frames are accumulated in
//    rnn_buf_ until 480 are available, processed, then flushed to the callback.
//    This adds at most one extra 10 ms period of latency at non-48k rates.
//
//  Thread safety:
//    OnFrames is called from miniaudio's internal audio thread.  The callback
//    must not block, allocate heap memory, or acquire contended mutexes.
// ─────────────────────────────────────────────────────────────────────────────

struct ma_device;

// Forward-declare RNNoise state so the header compiles without rnnoise.h
// everywhere.  The .cpp includes rnnoise.h directly.
struct DenoiseState;

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
    // rate (capped at 96 kHz).  Call this before start().
    static uint32_t probe_native_sample_rate();

    // Open and start the capture device.  Returns false on failure.
    bool start(uint32_t sample_rate, OnFramesCallback cb);

    // Stop and release the device.  Safe to call even if start() failed.
    void stop();

    uint32_t sample_rate()    const { return sample_rate_; }
    bool     rnnoise_active() const { return rnn_ != nullptr; }

private:
    ma_device*       device_      = nullptr;
    uint32_t         sample_rate_ = 0;
    OnFramesCallback on_frames_;

    // ── RNNoise state ─────────────────────────────────────────────────────────
    DenoiseState*        rnn_     = nullptr; // null when RNNoise unavailable
    std::vector<float>   rnn_buf_;           // accumulation buffer for non-48k rates
    std::vector<float>   rnn_out_;           // processed output staging buffer

    void init_rnnoise();
    void destroy_rnnoise();

    // Process `frames` samples through RNNoise (or pass through if rnn_==null).
    // Calls on_frames_ with the processed data.
    void process_and_forward(const float* pcm, uint32_t frames, uint32_t sr);

    // miniaudio data callback — static trampoline into process_and_forward
    static void ma_callback(ma_device* dev, void* out,
                            const void* in, uint32_t frames);
};