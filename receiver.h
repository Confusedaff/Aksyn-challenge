#pragma once
#include <string>
#include <atomic>
#include <array>
#include <mutex>
#include <cstdint>
#include <vector>
#include "protocol.h"
#include "wav_saver.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Receiver — IXWebSocket client, adaptive jitter buffer, latency stats.
//
//  Responsibilities:
//    • Connect to node_A (auto-reconnect on drop).
//    • Validate incoming AudioPacket headers (magic + payload size).
//    • Detect new clips (clip_index change) and signal WavSaver to open a new
//      file.
//    • Maintain the jitter buffer (AudioQueue) and playback-armed gate.
//    • Track per-session latency statistics (rolling 200-sample window).
//    • Expose pull_audio() which is called from the Playback callback.
//
//  Thread model:
//    IXWebSocket delivers messages on its own thread → on_message() runs there.
//    pull_audio() runs on miniaudio's audio thread.
//    All shared state is guarded by atomics or the jitter_buffer's internal mutex.
// ─────────────────────────────────────────────────────────────────────────────

class WavSaver;

// Jitter-buffer tuning
static constexpr int TARGET_FILL_PACKETS = 6;   // 60 ms pre-fill
static constexpr int LOW_WATER_MARK      = 3;   // 30 ms re-arm threshold
static constexpr int HIGH_WATER_MARK     = 180; // drop oldest above this

class Receiver {
public:
    explicit Receiver(WavSaver& saver) : saver_(saver) {}
    ~Receiver();

    Receiver(const Receiver&)            = delete;
    Receiver& operator=(const Receiver&) = delete;

    // Connect to node_A and begin receiving (blocking until stop() is called).
    // Returns only after stop().
    void run(const std::string& ws_url);

    // Thread-safe stop (safe to call from any thread or signal handler)
    void stop();

    // Called from miniaudio audio thread — pull `frame_count` f32 frames.
    void pull_audio(float* output, uint32_t frame_count);

    // ── Stats accessors (thread-safe) ─────────────────────────────────────────
    uint64_t packets_received() const { return pkts_rx_.load();   }
    uint64_t packets_dropped()  const { return pkts_drop_.load(); }
    uint64_t underruns()        const { return underruns_.load(); }

    // Fill a ClipStats struct for the last (or current) clip
    ClipStats current_clip_stats() const;

    const std::string& url() const { return url_; }

private:
    WavSaver& saver_;
    std::string url_;

    // ── Jitter buffer ──────────────────────────────────────────────────────────
    AudioQueue jitter_buffer_{200};
    std::atomic<bool> playback_armed_{false};

    // Residual PCM from a packet that was only partially consumed last callback
    std::vector<uint8_t> residual_pcm_;
    // Last-good PCM for Packet Loss Concealment (PLC)
    std::vector<uint8_t> last_good_pcm_;

    // ── Per-session state ──────────────────────────────────────────────────────
    std::atomic<uint32_t> current_clip_{0};
    std::atomic<uint32_t> g_sample_rate_{48000};
    uint32_t              g_session_id_       = 0;
    uint64_t              g_session_start_us_ = 0;
    uint64_t              clip_start_us_      = 0;

    // ── Stats ──────────────────────────────────────────────────────────────────
    std::atomic<uint64_t> pkts_rx_  {0};
    std::atomic<uint64_t> pkts_drop_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint32_t> last_seq_ {UINT32_MAX};

    std::array<double, 200> latency_window_{};
    int        window_idx_  = 0;
    bool       window_full_ = false;
    mutable std::mutex window_mtx_;

    // ── Internal helpers ───────────────────────────────────────────────────────
    void on_message(const std::string& raw);
    void reset_clip_state(uint32_t new_clip, uint32_t session_id,
                          uint64_t session_start_us, uint32_t sr);
    void compute_stats(double latency_ms, double& mean_out, double& p95_out);
    void print_header();
    void print_row(uint32_t clip, uint32_t seq, double lat, double mean, double p95);
};
