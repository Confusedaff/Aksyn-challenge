#pragma once
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "protocol.h"

// Forward declarations
struct ma_encoder;

// ─────────────────────────────────────────────────────────────────────────────
//  WavSaver — manages WAV file creation, a background writer thread, and the
//  post-close INFO metadata chunk.
//
//  Responsibilities:
//    • open_clip()   — flush & close the previous file, open a new one.
//    • write_pcm()   — non-blocking enqueue of raw PCM bytes (audio-thread safe).
//    • stop()        — signal the writer thread to drain and exit; call before
//                      the final close_and_embed_metadata().
//    • close_and_embed_metadata() — finalise the encoder and append a RIFF
//                      LIST/INFO chunk with session stats.
//
//  All actual fwrite() calls happen on the dedicated writer thread so the
//  miniaudio audio callback is never stalled by disk latency.
//
//  Audio format written:  32-bit float PCM, mono, sample rate from packet header.
// ─────────────────────────────────────────────────────────────────────────────

struct ClipStats {
    uint64_t    session_id       = 0;
    uint32_t    clip_index       = 0;
    uint64_t    session_start_us = 0;
    uint64_t    clip_start_us    = 0;
    uint32_t    sample_rate      = 48000;
    std::string node_a_url;
    uint64_t    pkts_rx          = 0;
    uint64_t    pkts_dropped     = 0;
    double      mean_latency_ms  = 0.0;
    double      p95_latency_ms   = 0.0;
};

class WavSaver {
public:
    WavSaver();
    ~WavSaver();

    // Non-copyable
    WavSaver(const WavSaver&)            = delete;
    WavSaver& operator=(const WavSaver&) = delete;

    // Start the background writer thread.  Call once before open_clip().
    void start();

    // Flush old clip, open new WAV file.
    // Returns the full path of the newly created file.
    std::string open_clip(uint32_t clip_index, uint32_t session_id,
                          uint64_t session_start_us, uint32_t sample_rate);

    // Enqueue raw PCM bytes for writing (audio-thread safe, non-blocking).
    void write_pcm(const uint8_t* pcm, size_t bytes);

    // Signal the writer thread to stop after draining the queue.
    void stop();

    // Close the WAV encoder and append an INFO metadata chunk.
    // Call after stop() has returned.
    void close_and_embed_metadata(const ClipStats& stats);

    // Current WAV file path (empty if no clip is open)
    std::string current_path() const;

    // Recordings root directory
    static std::string recordings_dir();

private:
    // ── Writer thread ──────────────────────────────────────────────────────────
    void writer_loop();
    void flush_write_queue_locked(); // called with wav_mtx_ held

    AudioQueue             write_queue_{2000}; // 20 s headroom @ 10 ms/packet
    std::thread            writer_thread_;
    std::atomic<bool>      running_{false};
    std::condition_variable cv_;
    std::mutex             cv_mtx_;

    // ── Encoder state ──────────────────────────────────────────────────────────
    ma_encoder*  encoder_      = nullptr;
    bool         encoder_ready_= false;
    std::string  current_path_;
    mutable std::mutex wav_mtx_;

    // ── Helpers ────────────────────────────────────────────────────────────────
    static std::string make_clip_path(uint32_t clip_index,
                                      uint64_t session_start_us,
                                      uint32_t session_id,
                                      uint32_t sr);

    static void append_info_chunk(const std::string& path,
                                  const ClipStats& stats);
};
