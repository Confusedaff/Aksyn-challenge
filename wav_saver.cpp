// wav_saver.cpp
// miniaudio is included here for ma_encoder only (no device usage).
// The MINIAUDIO_IMPLEMENTATION guard is already defined in audio_capture.cpp
// (node_A) or playback.cpp (node_B).  Here we need only the encoder API, which
// is header-only when the implementation is compiled in another TU.
#include "miniaudio.h"

#include "wav_saver.h"
#include "protocol.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <new>
#include "db_writer.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

static constexpr ma_format  ENCODER_FORMAT   = ma_format_f32;
static constexpr uint16_t   ENCODER_CHANNELS = 1;
static constexpr uint16_t   BITS_PER_SAMPLE  = 32;
static constexpr size_t     BYTES_PER_FRAME  = ENCODER_CHANNELS * (BITS_PER_SAMPLE / 8); // 4

// ─────────────────────────────────────────────────────────────────────────────
//  recordings_dir / make_clip_path
// ─────────────────────────────────────────────────────────────────────────────
std::string WavSaver::recordings_dir() {
    std::filesystem::path cwd = std::filesystem::current_path();
    if (cwd.filename() == "build") cwd = cwd.parent_path();
    return (cwd / "recordings").string();
}

std::string WavSaver::make_clip_path(uint32_t clip_index,
                                      uint64_t session_start_us,
                                      uint32_t session_id,
                                      uint32_t sr)
{
    std::string dir = recordings_dir();
    MKDIR(dir.c_str());
    std::ostringstream oss;
    oss << dir << "/clip_"
        << std::setfill('0') << std::setw(4) << clip_index
        << "_" << timestamp_for_filename(session_start_us)
        << "_sid" << std::hex << std::setfill('0') << std::setw(8) << session_id
        << std::dec
        << "_" << sr / 1000 << "k_f32_mono.wav";
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────
WavSaver::WavSaver()  = default;
WavSaver::~WavSaver() { stop(); }

// ─────────────────────────────────────────────────────────────────────────────
//  start / stop
// ─────────────────────────────────────────────────────────────────────────────
void WavSaver::start() {
    running_.store(true, std::memory_order_release);
    writer_thread_ = std::thread(&WavSaver::writer_loop, this);
    std::cout << "[WAV SAVER] Writer thread started\n";
}

void WavSaver::stop() {
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (writer_thread_.joinable()) writer_thread_.join();
}

// ─────────────────────────────────────────────────────────────────────────────
//  open_clip — flush old encoder, create new WAV file
// ─────────────────────────────────────────────────────────────────────────────
std::string WavSaver::open_clip(uint32_t clip_index, uint32_t session_id,
                                 uint64_t session_start_us, uint32_t sample_rate)
{
    // Drain any queued PCM that belongs to the old clip before switching files
    {
        std::vector<uint8_t> buf;
        while (write_queue_.pop(buf)) {
            if (buf.empty()) continue;
            std::lock_guard<std::mutex> lk(wav_mtx_);
            if (encoder_ready_)
                ma_encoder_write_pcm_frames(encoder_,
                    buf.data(), buf.size() / BYTES_PER_FRAME, nullptr);
        }
    }

    std::lock_guard<std::mutex> lk(wav_mtx_);

    // Close previous encoder
    if (encoder_ready_) {
        ma_encoder_uninit(encoder_);
        encoder_ready_ = false;
        std::cout << "[WAV SAVER] Closed  : " << current_path_ << "\n";
    }

    // Open new file
    current_path_ = make_clip_path(clip_index, session_start_us, session_id, sample_rate);

    if (!encoder_) encoder_ = new (std::nothrow) ma_encoder{};

    ma_encoder_config cfg = ma_encoder_config_init(
        ma_encoding_format_wav, ENCODER_FORMAT, ENCODER_CHANNELS, sample_rate);

    if (ma_encoder_init_file(current_path_.c_str(), &cfg, encoder_) == MA_SUCCESS) {
        encoder_ready_ = true;
        std::cout << "[WAV SAVER] New clip: " << current_path_ << "\n";
    } else {
        std::cerr << "[WAV SAVER] WARNING: cannot create WAV: " << current_path_ << "\n";
    }

    return current_path_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  write_pcm  (audio-thread safe — non-blocking)
// ─────────────────────────────────────────────────────────────────────────────
void WavSaver::write_pcm(const uint8_t* pcm, size_t bytes) {
    if (bytes == 0) return;
    std::vector<uint8_t> buf(pcm, pcm + bytes);
    write_queue_.push(buf);
    cv_.notify_one();
}

// ─────────────────────────────────────────────────────────────────────────────
//  writer_loop — background thread
// ─────────────────────────────────────────────────────────────────────────────
void WavSaver::writer_loop() {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(cv_mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(5),
                [this]{ return write_queue_.size() > 0
                             || !running_.load(std::memory_order_acquire); });
        }
        std::vector<uint8_t> buf;
        while (write_queue_.pop(buf)) {
            if (buf.empty()) continue;
            std::lock_guard<std::mutex> lk(wav_mtx_);
            if (encoder_ready_)
                ma_encoder_write_pcm_frames(encoder_,
                    buf.data(), buf.size() / BYTES_PER_FRAME, nullptr);
        }
    }
    // Drain remaining frames after stop signal
    std::vector<uint8_t> buf;
    while (write_queue_.pop(buf)) {
        if (buf.empty()) continue;
        std::lock_guard<std::mutex> lk(wav_mtx_);
        if (encoder_ready_)
            ma_encoder_write_pcm_frames(encoder_,
                buf.data(), buf.size() / BYTES_PER_FRAME, nullptr);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  close_and_embed_metadata — call after stop()
// ─────────────────────────────────────────────────────────────────────────────
void WavSaver::close_and_embed_metadata(const ClipStats& stats) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(wav_mtx_);
        if (encoder_ready_) {
            ma_encoder_uninit(encoder_);
            encoder_ready_ = false;
        }
        path = current_path_;
    }
    if (!path.empty()) {
        append_info_chunk(path, stats);
        std::cout << "[WAV SAVER] Metadata embedded → " << path << "\n";
    }
    db_record_clip(
    stats.clip_index,
    static_cast<uint32_t>(stats.session_id),
    stats.session_start_us,
    stats.clip_start_us,
    stats.sample_rate,
    path,
    stats.pkts_rx,
    stats.pkts_dropped,
    stats.mean_latency_ms,
    stats.p95_latency_ms
    );
}

std::string WavSaver::current_path() const {
    std::lock_guard<std::mutex> lk(wav_mtx_);
    return current_path_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  append_info_chunk — writes a RIFF LIST/INFO chunk with session statistics
// ─────────────────────────────────────────────────────────────────────────────
void WavSaver::append_info_chunk(const std::string& path, const ClipStats& s) {
    auto pack = [](const char fcc[4], const std::string& text) {
        std::vector<uint8_t> out(fcc, fcc + 4);
        uint32_t sz = static_cast<uint32_t>(text.size() + 1);
        for (int i = 0; i < 4; ++i) out.push_back((sz >> (8*i)) & 0xFF);
        out.insert(out.end(), text.begin(), text.end());
        out.push_back(0);
        if (sz & 1) out.push_back(0);
        return out;
    };
    auto dbl = [](double v) {
        std::ostringstream o; o << std::fixed << std::setprecision(3) << v; return o.str();
    };
    auto hex32 = [](uint32_t v) {
        std::ostringstream o; o << "0x" << std::hex << std::uppercase << v; return o.str();
    };

    std::vector<uint8_t> body;
    auto ap = [&](const char c[4], const std::string& v) {
        auto x = pack(c, v); body.insert(body.end(), x.begin(), x.end());
    };

    ap("INAM", "Clip #" + std::to_string(s.clip_index) + "  session " + hex32(static_cast<uint32_t>(s.session_id)));
    ap("ICRD", timestamp_for_filename(s.clip_start_us));
    ap("ISFT", "AudioLinkV2 / node_B / miniaudio");
    ap("SESS", hex32(static_cast<uint32_t>(s.session_id)));
    ap("SIDX", std::to_string(s.clip_index));
    ap("SSTT", std::to_string(s.session_start_us));
    ap("CSTT", std::to_string(s.clip_start_us));
    ap("CSTH", timestamp_for_filename(s.clip_start_us));
    ap("SRTE", std::to_string(s.sample_rate));
    ap("BDEP", std::to_string(BITS_PER_SAMPLE));
    ap("CHAN", std::to_string(ENCODER_CHANNELS));
    ap("SRCA", s.node_a_url);
    ap("RXPK", std::to_string(s.pkts_rx));
    ap("DRPK", std::to_string(s.pkts_dropped));
    ap("MLAT", dbl(s.mean_latency_ms) + " ms");
    ap("P95L", dbl(s.p95_latency_ms)  + " ms");

    uint32_t list_sz = 4 + static_cast<uint32_t>(body.size());
    std::vector<uint8_t> chunk;
    chunk.reserve(8 + list_sz);
    for (auto c : {'L','I','S','T'}) chunk.push_back(static_cast<uint8_t>(c));
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&list_sz);
    chunk.insert(chunk.end(), p, p + 4);
    for (auto c : {'I','N','F','O'}) chunk.push_back(static_cast<uint8_t>(c));
    chunk.insert(chunk.end(), body.begin(), body.end());

    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f.is_open()) return;
    f.seekg(0, std::ios::end);
    uint32_t old_sz = static_cast<uint32_t>(f.tellg());
    f.seekp(0, std::ios::end);
    f.write(reinterpret_cast<const char*>(chunk.data()),
            static_cast<std::streamsize>(chunk.size()));
    uint32_t new_riff = old_sz + static_cast<uint32_t>(chunk.size()) - 8;
    f.seekp(4);
    f.write(reinterpret_cast<const char*>(&new_riff), 4);
}
