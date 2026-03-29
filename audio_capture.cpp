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

// ── RNNoise — compiled in if the header is found (set by CMake) ───────────────
//
//  RNNoise is a recurrent neural network noise suppressor from Mozilla / Xiph.
//  It removes background noise (fans, keyboard, hiss) in real time with
//  negligible CPU cost (~1% of a single core).
//
//  It ALWAYS processes exactly 480 float samples per call at 48 kHz.
//  At 48 kHz our 10 ms period == 480 frames, so processing is synchronous.
//  At other rates we accumulate into rnn_buf_ and flush in 480-frame chunks.
//
#ifdef HAVE_RNNOISE
#include "rnnoise.h"
#endif

#include "audio_capture.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <new>

// ─────────────────────────────────────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr ma_format  CAPTURE_FORMAT    = ma_format_f32;
static constexpr uint32_t   CAPTURE_CHANNELS  = 1;
static constexpr uint32_t   RNN_FRAME_SIZE    = 480; // RNNoise hardcoded frame size

// ─────────────────────────────────────────────────────────────────────────────
AudioCapture::~AudioCapture() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  probe_native_sample_rate  (unchanged)
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
    if (chosen > 96000) chosen = 96000;

    std::cout << "[CAPTURE] → Rate  : " << chosen << " Hz"
              << (best_f32 ? "  (native f32)\n" : "  (converted to f32)\n");
    return chosen;
}

// ─────────────────────────────────────────────────────────────────────────────
//  init_rnnoise / destroy_rnnoise
// ─────────────────────────────────────────────────────────────────────────────
void AudioCapture::init_rnnoise() {
#ifdef HAVE_RNNOISE
    rnn_ = rnnoise_create(nullptr);
    if (rnn_) {
        // Pre-allocate buffers — worst case is one full RNN frame
        rnn_buf_.reserve(RNN_FRAME_SIZE * 2);
        rnn_out_.resize(RNN_FRAME_SIZE);
        std::cout << "[CAPTURE] RNNoise : ENABLED (480-frame neural denoiser)\n";
    } else {
        std::cerr << "[CAPTURE] RNNoise : rnnoise_create() failed — running without denoising\n";
    }
#else
    std::cout << "[CAPTURE] RNNoise : NOT built in (recompile with -DHAVE_RNNOISE)\n";
#endif
}

void AudioCapture::destroy_rnnoise() {
#ifdef HAVE_RNNOISE
    if (rnn_) {
        rnnoise_destroy(rnn_);
        rnn_ = nullptr;
    }
#endif
    rnn_buf_.clear();
    rnn_buf_.shrink_to_fit();
    rnn_out_.clear();
    rnn_out_.shrink_to_fit();
}

// ─────────────────────────────────────────────────────────────────────────────
//  process_and_forward
//
//  Called from the audio thread for every captured period.
//  If RNNoise is active, processes in 480-frame chunks.
//  Leftover frames are held in rnn_buf_ and prepended next callback.
//  If RNNoise is not active, passes through unchanged.
// ─────────────────────────────────────────────────────────────────────────────
void AudioCapture::process_and_forward(const float* pcm, uint32_t frames, uint32_t sr) {
#ifdef HAVE_RNNOISE
    if (rnn_) {
        // Append incoming frames to the accumulation buffer
        rnn_buf_.insert(rnn_buf_.end(), pcm, pcm + frames);

        // Process as many complete 480-frame chunks as we have
        size_t offset = 0;
        while (rnn_buf_.size() - offset >= RNN_FRAME_SIZE) {
            // RNNoise works in-place on a mutable buffer
            std::copy(rnn_buf_.begin() + offset,
                      rnn_buf_.begin() + offset + RNN_FRAME_SIZE,
                      rnn_out_.begin());

            rnnoise_process_frame(rnn_, rnn_out_.data(), rnn_out_.data());

            // Forward the denoised chunk to the transmitter
            on_frames_(rnn_out_.data(), RNN_FRAME_SIZE, sr);
            offset += RNN_FRAME_SIZE;
        }

        // Keep any leftover frames for the next callback
        if (offset > 0)
            rnn_buf_.erase(rnn_buf_.begin(), rnn_buf_.begin() + offset);

        return;
    }
#endif
    // No denoising — forward the raw PCM directly
    on_frames_(pcm, frames, sr);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Static miniaudio trampoline
// ─────────────────────────────────────────────────────────────────────────────
void AudioCapture::ma_callback(ma_device* dev, void* /*out*/,
                                const void* in, uint32_t frames)
{
    auto* self = static_cast<AudioCapture*>(dev->pUserData);
    if (self && self->on_frames_)
        self->process_and_forward(static_cast<const float*>(in), frames, dev->sampleRate);
}

// ─────────────────────────────────────────────────────────────────────────────
//  start
// ─────────────────────────────────────────────────────────────────────────────
bool AudioCapture::start(uint32_t sample_rate, OnFramesCallback cb) {
    on_frames_   = std::move(cb);
    sample_rate_ = sample_rate;

    // Initialise RNNoise before opening the device so the log lines appear
    // together in the startup block
    init_rnnoise();

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
        destroy_rnnoise();
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
    destroy_rnnoise();
}