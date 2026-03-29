// ── miniaudio – configure BEFORE the implementation include ──────────────────
//
//  MA_WASAPI_NO_DEFAULT_QUALITY_CONVERSION disables the Windows Audio Engine
//  sample-rate conversion so we get the device's true native rate in exclusive
//  mode, bypassing the Windows mixer entirely (no hidden 16 kHz→96 kHz upsample).
//
#define MA_WASAPI_NO_DEFAULT_QUALITY_CONVERSION
#define MA_ENABLE_WASAPI
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio_capture.h"
#include <iostream>
#include <cstring>
#include <new>

// ─────────────────────────────────────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr ma_format  CAPTURE_FORMAT   = ma_format_f32;
static constexpr uint32_t   CAPTURE_CHANNELS = 1;

// ─────────────────────────────────────────────────────────────────────────────
AudioCapture::~AudioCapture() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  probe_native_sample_rate
// ─────────────────────────────────────────────────────────────────────────────
uint32_t AudioCapture::probe_native_sample_rate() {
    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return 48000;

    ma_device_info* pInfos = nullptr;
    ma_uint32        count  = 0;
    if (ma_context_get_devices(&ctx, nullptr, nullptr, &pInfos, &count) != MA_SUCCESS
        || count == 0)
    {
        ma_context_uninit(&ctx);
        return 48000;
    }

    ma_device_info info = pInfos[0];
    if (ma_context_get_device_info(&ctx, ma_device_type_capture,
                                   &info.id, &info) != MA_SUCCESS)
    {
        ma_context_uninit(&ctx);
        return 48000;
    }

    std::cout << "[CAPTURE] Device  : " << info.name << "\n";
    std::cout << "[CAPTURE] Formats :\n";

    uint32_t best_f32 = 0, best_any = 0;
    for (ma_uint32 i = 0; i < info.nativeDataFormatCount; ++i) {
        const auto& f = info.nativeDataFormats[i];
        std::cout << "  [" << i << "] fmt=" << f.format
                  << " ch=" << f.channels
                  << " sr=" << f.sampleRate << "\n";
        if (f.sampleRate > best_any) best_any = f.sampleRate;
        if (f.format == ma_format_f32 && f.sampleRate > best_f32)
            best_f32 = f.sampleRate;
    }
    ma_context_uninit(&ctx);

    uint32_t chosen = best_f32 ? best_f32 : best_any;
    if (!chosen) chosen = 48000;
    if (chosen > 96000) chosen = 96000; // cap — beyond this brings no perceptible benefit

    std::cout << "[CAPTURE] → Rate  : " << chosen << " Hz"
              << (best_f32 ? "  (native f32)\n" : "  (converted to f32)\n");
    return chosen;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Static miniaudio trampoline
// ─────────────────────────────────────────────────────────────────────────────
void AudioCapture::ma_callback(ma_device* dev, void* /*out*/,
                                const void* in, uint32_t frames)
{
    auto* self = static_cast<AudioCapture*>(dev->pUserData);
    if (self && self->on_frames_)
        self->on_frames_(static_cast<const float*>(in), frames, dev->sampleRate);
}

// ─────────────────────────────────────────────────────────────────────────────
//  start
// ─────────────────────────────────────────────────────────────────────────────
bool AudioCapture::start(uint32_t sample_rate, OnFramesCallback cb) {
    on_frames_   = std::move(cb);
    sample_rate_ = sample_rate;

    device_ = new (std::nothrow) ma_device{};
    if (!device_) return false;

    ma_device_config cfg   = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format     = CAPTURE_FORMAT;
    cfg.capture.channels   = CAPTURE_CHANNELS;
    cfg.sampleRate         = sample_rate;
    cfg.dataCallback       = ma_callback;
    cfg.pUserData          = this;
    cfg.periodSizeInFrames = sample_rate / 100; // 10 ms period

#ifdef _WIN32
    cfg.wasapi.noAutoConvertSRC = MA_TRUE; // try exclusive first
#endif

    ma_result rc = ma_device_init(NULL, &cfg, device_);

#ifdef _WIN32
    if (rc != MA_SUCCESS) {
        std::cerr << "[CAPTURE] Exclusive WASAPI failed — falling back to shared mode.\n"
                  << "          Quality may be degraded if the OS inserts a resampler.\n";
        cfg.wasapi.noAutoConvertSRC = MA_FALSE;
        rc = ma_device_init(NULL, &cfg, device_);
        if (rc == MA_SUCCESS)
            std::cout << "[CAPTURE] WASAPI mode: SHARED (OS resampling possible)\n";
    } else {
        std::cout << "[CAPTURE] WASAPI mode: EXCLUSIVE\n";
    }
#endif

    if (rc != MA_SUCCESS) {
        std::cerr << "[CAPTURE] Failed to open capture device\n";
        delete device_;
        device_ = nullptr;
        return false;
    }

    // Update sample_rate_ to what the driver actually opened
    sample_rate_ = device_->sampleRate;
    if (sample_rate_ != sample_rate)
        std::cout << "[CAPTURE] WARNING: requested " << sample_rate
                  << " Hz, driver gave " << sample_rate_ << " Hz\n";

    ma_device_start(device_);
    std::cout << "[CAPTURE] Active  : " << sample_rate_ << " Hz"
              << " / 32-bit float / mono"
              << " / " << cfg.periodSizeInFrames << " frames/period\n";
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  stop
// ─────────────────────────────────────────────────────────────────────────────
void AudioCapture::stop() {
    if (device_) {
        ma_device_uninit(device_);
        delete device_;
        device_ = nullptr;
    }
}
