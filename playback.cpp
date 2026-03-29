// playback.cpp
// MINIAUDIO_IMPLEMENTATION is defined here for node_B's translation unit.
#define MA_WASAPI_NO_DEFAULT_QUALITY_CONVERSION
#define MA_ENABLE_WASAPI
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "playback.h"
#include <iostream>
#include <new>

static constexpr ma_format PLAYBACK_FORMAT   = ma_format_f32;
static constexpr uint32_t  PLAYBACK_CHANNELS = 1;

// ─────────────────────────────────────────────────────────────────────────────
Playback::~Playback() { stop(); }

// ─────────────────────────────────────────────────────────────────────────────
void Playback::ma_callback(ma_device* dev, void* out, const void* /*in*/,
                            uint32_t frames)
{
    auto* self = static_cast<Playback*>(dev->pUserData);
    if (self && self->pull_cb_)
        self->pull_cb_(static_cast<float*>(out), frames);
}

// ─────────────────────────────────────────────────────────────────────────────
bool Playback::start(uint32_t sample_rate, PullCallback cb) {
    pull_cb_     = std::move(cb);
    sample_rate_ = sample_rate;

    device_ = new (std::nothrow) ma_device{};
    if (!device_) return false;

    ma_device_config cfg    = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format     = PLAYBACK_FORMAT;
    cfg.playback.channels   = PLAYBACK_CHANNELS;
    cfg.sampleRate          = sample_rate;
    cfg.dataCallback        = ma_callback;
    cfg.pUserData           = this;
    cfg.periodSizeInFrames  = sample_rate / 100; // 10 ms

    if (ma_device_init(NULL, &cfg, device_) != MA_SUCCESS) {
        std::cerr << "[PLAYBACK] Failed to open playback device\n";
        delete device_;
        device_ = nullptr;
        return false;
    }

    ma_device_start(device_);
    sample_rate_ = device_->sampleRate;
    std::cout << "[PLAYBACK] Active: " << sample_rate_ << " Hz / 32-bit float / mono\n";
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void Playback::stop() {
    if (device_) {
        ma_device_uninit(device_);
        delete device_;
        device_ = nullptr;
    }
}
