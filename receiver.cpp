#include "receiver.h"
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <algorithm>

static constexpr size_t BYTES_PER_FRAME = 1 * 4; // mono × 32-bit float

// ─────────────────────────────────────────────────────────────────────────────
Receiver::~Receiver() { stop(); }

// ─────────────────────────────────────────────────────────────────────────────
void Receiver::stop() {
    // IXWebSocket::stop() is called from run() after std::cin.get() returns
    // (see node_B.cpp).  Nothing special needed here.
}

// ─────────────────────────────────────────────────────────────────────────────
//  run  — connect and receive until stopped
// ─────────────────────────────────────────────────────────────────────────────
void Receiver::run(const std::string& ws_url) {
    url_ = ws_url;

    ix::WebSocket ws;
    ws.setUrl(url_);
    ws.enableAutomaticReconnection();
    ws.setMaxWaitBetweenReconnectionRetries(2000);

    ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            std::cout << "\n[RECEIVER] Connected to " << url_ << "\n";
            print_header();
        }
        else if (msg->type == ix::WebSocketMessageType::Close) {
            std::cout << "\n[RECEIVER] Disconnected — reconnecting...\n";
        }
        else if (msg->type == ix::WebSocketMessageType::Message && msg->binary) {
            on_message(msg->str);
        }
    });

    std::cout << "[RECEIVER] Connecting to " << url_ << "...\n";
    ws.start();

    // Block until user presses ENTER
    std::cin.get();
    ws.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  on_message  (IXWebSocket thread)
// ─────────────────────────────────────────────────────────────────────────────
void Receiver::on_message(const std::string& raw) {
    if (raw.size() < sizeof(AudioPacket)) return;

    const AudioPacket* hdr = reinterpret_cast<const AudioPacket*>(raw.data());
    if (hdr->magic != AUDIO_PACKET_MAGIC) return;

    // Validate payload size
    if (raw.size() != sizeof(AudioPacket) + hdr->payload_bytes) return;

    // ── Sample-rate sync ────────────────────────────────────────────────────
    if (hdr->sample_rate > 0 && hdr->sample_rate != g_sample_rate_.load())
        g_sample_rate_.store(hdr->sample_rate);

    // ── New-clip detection ──────────────────────────────────────────────────
    uint32_t incoming_clip = hdr->clip_index;
    if (incoming_clip != current_clip_.load()) {
        reset_clip_state(incoming_clip, hdr->session_id,
                         hdr->session_start_us, hdr->sample_rate);
        saver_.open_clip(incoming_clip, hdr->session_id,
                         hdr->session_start_us, hdr->sample_rate);
    }

    // ── Sequence-gap drop counting ──────────────────────────────────────────
    uint32_t prev = last_seq_.load();
    if (prev != UINT32_MAX) {
        uint32_t gap = hdr->sequence_number - (prev + 1);
        if (gap > 0) pkts_drop_ += gap;
    }
    last_seq_.store(hdr->sequence_number);

    // ── Latency ─────────────────────────────────────────────────────────────
    uint64_t now_us    = get_current_time_us();
    double   lat_ms    = (now_us - hdr->timestamp_us) / 1000.0;
    pkts_rx_++;

    double mean_ms = 0.0, p95_ms = 0.0;
    compute_stats(lat_ms, mean_ms, p95_ms);

    // ── Jitter buffer ────────────────────────────────────────────────────────
    if (jitter_buffer_.size() >= static_cast<size_t>(HIGH_WATER_MARK)) {
        std::vector<uint8_t> discard;
        jitter_buffer_.pop(discard);
    }
    std::vector<uint8_t> data(raw.begin(), raw.end());
    jitter_buffer_.push(data);

    // Arm playback once the pre-fill threshold is reached
    if (!playback_armed_.load(std::memory_order_acquire)) {
        if (jitter_buffer_.size() >= static_cast<size_t>(TARGET_FILL_PACKETS))
            playback_armed_.store(true, std::memory_order_release);
    }

    print_row(incoming_clip, hdr->sequence_number, lat_ms, mean_ms, p95_ms);
}

// ─────────────────────────────────────────────────────────────────────────────
//  pull_audio  (miniaudio audio thread — never block)
// ─────────────────────────────────────────────────────────────────────────────
void Receiver::pull_audio(float* output, uint32_t frame_count) {
    size_t    expected = frame_count * BYTES_PER_FRAME;
    uint8_t*  out      = reinterpret_cast<uint8_t*>(output);

    // ── Pre-fill gate ─────────────────────────────────────────────────────────
    if (!playback_armed_.load(std::memory_order_acquire)) {
        std::memset(out, 0, expected);
        return;
    }

    // ── Low-water re-arm ──────────────────────────────────────────────────────
    if (jitter_buffer_.size() < static_cast<size_t>(LOW_WATER_MARK)
        && residual_pcm_.empty())
    {
        playback_armed_.store(false, std::memory_order_release);
        underruns_.fetch_add(1, std::memory_order_relaxed);

        // Write time-aligned silence to WAV so the recording stays in sync
        static const std::vector<uint8_t> silence(480 * BYTES_PER_FRAME, 0);
        for (size_t rem = expected; rem > 0; ) {
            size_t chunk = std::min(rem, silence.size());
            saver_.write_pcm(silence.data(), chunk);
            rem -= chunk;
        }
        std::memset(out, 0, expected);
        return;
    }

    size_t written = 0;

    // ── Step 1: drain residual from last callback ─────────────────────────────
    if (!residual_pcm_.empty()) {
        size_t copy = std::min(expected, residual_pcm_.size());
        std::memcpy(out, residual_pcm_.data(), copy);
        saver_.write_pcm(residual_pcm_.data(), copy);
        written += copy;
        residual_pcm_.erase(residual_pcm_.begin(), residual_pcm_.begin() + copy);
    }

    // ── Step 2: pull from jitter buffer ──────────────────────────────────────
    while (written < expected) {
        std::vector<uint8_t> pkt;
        if (!jitter_buffer_.pop(pkt)) break;

        const uint8_t* pcm  = pkt.data() + sizeof(AudioPacket);
        size_t         plen = pkt.size()  - sizeof(AudioPacket);
        size_t         need = expected - written;
        size_t         copy = std::min(need, plen);

        std::memcpy(out + written, pcm, copy);
        saver_.write_pcm(pcm, copy);
        written += copy;

        if (copy < plen)
            residual_pcm_.assign(pcm + copy, pcm + plen);

        last_good_pcm_.assign(pcm, pcm + plen);
    }

    // ── Step 3: PLC / silence for deficit ────────────────────────────────────
    if (written < expected) {
        size_t deficit = expected - written;
        if (!last_good_pcm_.empty()) {
            size_t copy = std::min(deficit, last_good_pcm_.size());
            std::memcpy(out + written, last_good_pcm_.data(), copy);
            saver_.write_pcm(last_good_pcm_.data(), copy);
            written += copy;
            deficit  = expected - written;
        }
        if (deficit > 0) {
            std::memset(out + written, 0, deficit);
            static const std::array<uint8_t, 64> zero{};
            for (size_t rem = deficit; rem > 0; ) {
                size_t chunk = std::min(rem, zero.size());
                saver_.write_pcm(zero.data(), chunk);
                rem -= chunk;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  reset_clip_state
// ─────────────────────────────────────────────────────────────────────────────
void Receiver::reset_clip_state(uint32_t new_clip, uint32_t session_id,
                                 uint64_t session_start_us, uint32_t sr)
{
    current_clip_.store(new_clip);
    g_sample_rate_.store(sr);
    g_session_id_       = session_id;
    g_session_start_us_ = session_start_us;
    clip_start_us_      = get_current_time_us();

    pkts_rx_.store(0);
    pkts_drop_.store(0);
    underruns_.store(0);
    last_seq_.store(UINT32_MAX);
    {
        std::lock_guard<std::mutex> lk(window_mtx_);
        window_idx_  = 0;
        window_full_ = false;
    }
    jitter_buffer_.clear();
    last_good_pcm_.clear();
    residual_pcm_.clear();
    playback_armed_.store(false);
}

// ─────────────────────────────────────────────────────────────────────────────
//  compute_stats — rolling 200-sample latency window
// ─────────────────────────────────────────────────────────────────────────────
void Receiver::compute_stats(double lat_ms, double& mean_out, double& p95_out) {
    std::lock_guard<std::mutex> lk(window_mtx_);
    latency_window_[window_idx_] = lat_ms;
    window_idx_ = (window_idx_ + 1) % 200;
    if (window_idx_ == 0) window_full_ = true;
    int n = window_full_ ? 200 : window_idx_;
    double sum = 0.0;
    std::array<double, 200> sorted{};
    for (int i = 0; i < n; ++i) { sum += latency_window_[i]; sorted[i] = latency_window_[i]; }
    mean_out = sum / n;
    std::sort(sorted.begin(), sorted.begin() + n);
    p95_out = sorted[static_cast<int>(n * 0.95)];
}

// ─────────────────────────────────────────────────────────────────────────────
//  current_clip_stats
// ─────────────────────────────────────────────────────────────────────────────
ClipStats Receiver::current_clip_stats() const {
    ClipStats s;
    s.session_id       = g_session_id_;
    s.clip_index       = current_clip_.load();
    s.session_start_us = g_session_start_us_;
    s.clip_start_us    = clip_start_us_;
    s.sample_rate      = g_sample_rate_.load();
    s.node_a_url       = url_;
    s.pkts_rx          = pkts_rx_.load();
    s.pkts_dropped     = pkts_drop_.load();

    std::lock_guard<std::mutex> lk(window_mtx_);
    int n = window_full_ ? 200 : window_idx_;
    if (n > 0) {
        std::array<double,200> sorted{};
        double sum = 0.0;
        for (int i = 0; i < n; ++i) { sum += latency_window_[i]; sorted[i] = latency_window_[i]; }
        s.mean_latency_ms = sum / n;
        std::sort(sorted.begin(), sorted.begin() + n);
        s.p95_latency_ms = sorted[static_cast<int>(n * 0.95)];
    }
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Console output helpers
// ─────────────────────────────────────────────────────────────────────────────
void Receiver::print_header() {
    std::cout << "────────────────────────────────────────────────────────────────\n";
    std::cout << std::left
              << std::setw(6)  << "CLIP"
              << std::setw(8)  << "SEQ"
              << std::setw(8)  << "BUF"
              << std::setw(14) << "LATENCY(ms)"
              << std::setw(14) << "MEAN(ms)"
              << std::setw(14) << "P95(ms)"
              << std::setw(8)  << "UNDRNS"
              << "\n";
    std::cout << "────────────────────────────────────────────────────────────────\n";
}

void Receiver::print_row(uint32_t clip, uint32_t seq,
                          double lat, double mean, double p95) {
    std::cout << std::left << std::fixed << std::setprecision(2)
              << std::setw(6)  << clip
              << std::setw(8)  << seq
              << std::setw(8)  << jitter_buffer_.size()
              << std::setw(14) << lat
              << std::setw(14) << mean
              << std::setw(14) << p95
              << std::setw(8)  << underruns_.load()
              << "\n";
}
