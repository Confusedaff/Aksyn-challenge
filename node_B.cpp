#include "protocol.h"
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>

// ── miniaudio – same flags as node_A so format handling is symmetric ──────────
#define MA_WASAPI_NO_DEFAULT_QUALITY_CONVERSION
#define MA_ENABLE_WASAPI
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <array>
#include <vector>
#include <string>
#include <chrono>
#include <cstdio>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Audio format (node_B reads sample_rate from the packet header at runtime
//  so it stays in sync with whatever Node A probed — no hardcoding needed).
// ─────────────────────────────────────────────────────────────────────────────
static constexpr ma_format  MA_FMT          = ma_format_f32;
static constexpr uint16_t   CHANNELS        = 1;
static constexpr uint16_t   BITS_PER_SAMPLE = 32;
static constexpr size_t     BYTES_PER_FRAME = CHANNELS * (BITS_PER_SAMPLE / 8); // 4

// Set from the first packet's header — not constexpr.
static std::atomic<uint32_t> g_sample_rate{48000};

// ─────────────────────────────────────────────────────────────────────────────
//  Recordings folder
// ─────────────────────────────────────────────────────────────────────────────
#include <filesystem>  // make sure this is added at the top

static std::string get_recordings_dir() {
    std::filesystem::path cwd = std::filesystem::current_path();

    // If running from build/, go one level up
    if (cwd.filename() == "build") {
        cwd = cwd.parent_path();
    }

    return (cwd / "recordings").string();
}

static std::string RECORDINGS_DIR = get_recordings_dir();

static void ensure_recordings_dir() {
    MKDIR(RECORDINGS_DIR.c_str()); // ok if it already exists
}

static std::string make_clip_path(uint32_t clip_index,
                                   uint64_t session_start_us,
                                   uint32_t session_id,
                                   uint32_t sr)
{
    std::ostringstream oss;
    oss << RECORDINGS_DIR << "/clip_"
        << std::setfill('0') << std::setw(4) << clip_index
        << "_" << timestamp_for_filename(session_start_us)
        << "_sid" << std::hex << std::setfill('0') << std::setw(8) << session_id
        << std::dec
        << "_" << sr / 1000 << "k_f32_mono.wav";
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
//  WAV LIST/INFO chunk — embed rich metadata into the file
// ─────────────────────────────────────────────────────────────────────────────
static void append_info_chunk(const std::string& path,
                               uint32_t clip_index,  uint32_t session_id,
                               uint64_t session_start_us, uint64_t clip_start_us,
                               uint32_t sample_rate, const std::string& node_a_url,
                               uint64_t pkts_rx, uint64_t pkts_dropped,
                               double mean_ms, double p95_ms)
{
    auto pack = [](const char fcc[4], const std::string& text) {
        std::vector<uint8_t> out(fcc, fcc + 4);
        uint32_t sz = static_cast<uint32_t>(text.size() + 1);
        for (int i = 0; i < 4; ++i) out.push_back((sz >> (8*i)) & 0xFF);
        out.insert(out.end(), text.begin(), text.end());
        out.push_back(0);
        if (sz & 1) out.push_back(0);
        return out;
    };
    auto dbl = [](double v) { std::ostringstream o; o << std::fixed << std::setprecision(3) << v; return o.str(); };
    auto hex = [](uint32_t v) { std::ostringstream o; o << "0x" << std::hex << std::uppercase << v; return o.str(); };

    std::vector<uint8_t> body;
    auto ap = [&](const char c[4], const std::string& v) { auto x=pack(c,v); body.insert(body.end(),x.begin(),x.end()); };

    ap("INAM", "Clip #" + std::to_string(clip_index) + "  session " + hex(session_id));
    ap("ICRD", timestamp_for_filename(clip_start_us));
    ap("ISFT", "AudioLinkV2 / node_B / miniaudio");
    ap("SESS", hex(session_id));
    ap("SIDX", std::to_string(clip_index));
    ap("SSTT", std::to_string(session_start_us));
    ap("CSTT", std::to_string(clip_start_us));
    ap("CSTH", timestamp_for_filename(clip_start_us));
    ap("SRTE", std::to_string(sample_rate));
    ap("BDEP", std::to_string(BITS_PER_SAMPLE));
    ap("CHAN", std::to_string(CHANNELS));
    ap("SRCA", node_a_url);
    ap("RXPK", std::to_string(pkts_rx));
    ap("DRPK", std::to_string(pkts_dropped));
    ap("MLAT", dbl(mean_ms) + " ms");
    ap("P95L", dbl(p95_ms)  + " ms");

    uint32_t list_sz = 4 + static_cast<uint32_t>(body.size());
    std::vector<uint8_t> chunk;
    chunk.reserve(8 + list_sz);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&list_sz);
    for (auto c : {'L','I','S','T'}) chunk.push_back(c);
    chunk.insert(chunk.end(), p, p+4);
    for (auto c : {'I','N','F','O'}) chunk.push_back(c);
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

// ─────────────────────────────────────────────────────────────────────────────
//  Adaptive Jitter Buffer
//
//  Problems fixed vs. the old passive ring buffer:
//
//  1. PRE-FILL: The playback callback used to start draining immediately,
//     causing underruns on the very first burst of packets while the jitter
//     buffer is still building up.  Now we wait until TARGET_FILL_PACKETS
//     packets are queued before releasing audio to the callback.
//
//  2. FILL-LEVEL MONITORING: If the buffer dips below LOW_WATER_MARK we
//     pause playback and re-accumulate to TARGET_FILL, preventing the
//     callback from constantly triggering PLC (last-frame repeat).
//
//  3. PACKET SIZE VALIDATION: Every packet is checked against the expected
//     payload size before being enqueued; corrupt/truncated frames are dropped.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int TARGET_FILL_PACKETS = 4;   // 4 × 10 ms = 40 ms pre-fill
static constexpr int LOW_WATER_MARK      = 1;   // re-fill if we drop this low
static constexpr int HIGH_WATER_MARK     = 180; // drop oldest if we exceed this

AudioQueue jitter_buffer(200);
static std::vector<uint8_t> residual_pcm;

// Playback is gated until the jitter buffer has accumulated enough packets.
static std::atomic<bool> playback_armed{false};

// ─────────────────────────────────────────────────────────────────────────────
//  WAV encoder state
// ─────────────────────────────────────────────────────────────────────────────
static ma_encoder  wav_encoder;
static bool        wav_encoder_ready = false;
static std::string current_wav_path;
static std::mutex  wav_mutex;

// ─────────────────────────────────────────────────────────────────────────────
//  Stats
// ─────────────────────────────────────────────────────────────────────────────
std::atomic<uint64_t> packets_received{0};
std::atomic<uint64_t> packets_dropped{0};
std::atomic<uint64_t> underruns{0};
std::atomic<double>   last_latency_ms{0.0};
std::atomic<uint32_t> last_seq{UINT32_MAX};

static std::array<double, 200> latency_window{};
static int  window_idx  = 0;
static bool window_full = false;
static std::mutex window_mutex;

static std::atomic<uint32_t> current_clip_index{0};
static uint64_t clip_start_us      = 0;
static uint64_t g_session_start_us = 0;
static uint32_t g_session_id       = 0;
static std::string node_a_ws_url;

// PLC: last good PCM frame (audio thread only, no mutex needed)
static std::vector<uint8_t> last_good_pcm;

// ─────────────────────────────────────────────────────────────────────────────
//  Open a new WAV file for a new clip
// ─────────────────────────────────────────────────────────────────────────────
static void open_new_wav(uint32_t clip_index, uint32_t session_id,
                          uint64_t session_start_us, uint32_t sr)
{
    std::lock_guard<std::mutex> lk(wav_mutex);

    if (wav_encoder_ready) {
        ma_encoder_uninit(&wav_encoder);
        wav_encoder_ready = false;
        std::cout << "[NODE B] Closed clip: " << current_wav_path << "\n";
    }

    clip_start_us    = get_current_time_us();
    current_wav_path = make_clip_path(clip_index, session_start_us, session_id, sr);

    ma_encoder_config cfg = ma_encoder_config_init(
        ma_encoding_format_wav, MA_FMT, CHANNELS, sr);

    if (ma_encoder_init_file(current_wav_path.c_str(), &cfg, &wav_encoder) == MA_SUCCESS) {
        wav_encoder_ready = true;
        std::cout << "[NODE B] New clip → " << current_wav_path << "\n";
    } else {
        std::cerr << "[NODE B] WARNING: cannot create WAV: " << current_wav_path << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Playback callback  (audio thread — never block)
// ─────────────────────────────────────────────────────────────────────────────
void playback_data_callback(ma_device* /*pDevice*/, void* pOutput,
                             const void* /*pInput*/, ma_uint32 frameCount)
{
    size_t expected_bytes = frameCount * BYTES_PER_FRAME;
    uint8_t* out = static_cast<uint8_t*>(pOutput);

    // ── Gate: wait for pre-fill before outputting audio ───────────────────────
    if (!playback_armed.load(std::memory_order_acquire)) {
        std::memset(out, 0, expected_bytes);
        return;
    }

    // ── Re-arm check: if buffer is critically low, pause and re-fill ──────────
    if (jitter_buffer.size() < static_cast<size_t>(LOW_WATER_MARK) && residual_pcm.empty()) {
        playback_armed.store(false, std::memory_order_release);
        underruns.fetch_add(1, std::memory_order_relaxed);
        std::memset(out, 0, expected_bytes);
        return;
    }

    size_t bytes_written = 0;

    // Step 1: Drain any leftover data from the previous callback
    if (!residual_pcm.empty()) {
        size_t copy = std::min(expected_bytes, residual_pcm.size());
        std::memcpy(out, residual_pcm.data(), copy);

        // Write to WAV
        {
            std::unique_lock<std::mutex> lk(wav_mutex, std::try_to_lock);
            if (lk.owns_lock() && wav_encoder_ready) {
                ma_encoder_write_pcm_frames(&wav_encoder, residual_pcm.data(), copy / BYTES_PER_FRAME, nullptr);
            }
        }

        bytes_written += copy;
        residual_pcm.erase(residual_pcm.begin(), residual_pcm.begin() + copy);
    }

    // Step 2: Pull from the jitter buffer until we satisfy 'expected_bytes'
    while (bytes_written < expected_bytes) {
        std::vector<uint8_t> packet_data;
        if (jitter_buffer.pop(packet_data)) {
            uint8_t* pcm  = packet_data.data() + sizeof(AudioPacket);
            size_t   plen = packet_data.size()  - sizeof(AudioPacket);

            size_t bytes_needed = expected_bytes - bytes_written;
            size_t copy = std::min(bytes_needed, plen);

            std::memcpy(out + bytes_written, pcm, copy);

            // Write to WAV
            {
                std::unique_lock<std::mutex> lk(wav_mutex, std::try_to_lock);
                if (lk.owns_lock() && wav_encoder_ready) {
                    ma_encoder_write_pcm_frames(&wav_encoder, pcm, copy / BYTES_PER_FRAME, nullptr);
                }
            }

            bytes_written += copy;

            // Save any unread bytes for the next callback
            if (copy < plen) {
                residual_pcm.assign(pcm + copy, pcm + plen);
            }

            // Update PLC buffer with the last known good packet
            last_good_pcm.assign(pcm, pcm + plen);

        } else {
            // Jitter buffer ran dry before we could fill the request
            break;
        }
    }

    // Step 3: Handle underruns if we couldn't get enough data
    if (bytes_written < expected_bytes) {
        size_t deficit = expected_bytes - bytes_written;

        // PLC: repeat last good chunk
        if (!last_good_pcm.empty()) {
            size_t plc_copy = std::min(deficit, last_good_pcm.size());
            std::memcpy(out + bytes_written, last_good_pcm.data(), plc_copy);
            if (plc_copy < deficit) {
                std::memset(out + bytes_written + plc_copy, 0, deficit - plc_copy);
            }
        } else {
            std::memset(out + bytes_written, 0, deficit);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Session summary
// ─────────────────────────────────────────────────────────────────────────────
static void print_session_summary() {
    double mean_ms = 0.0, p95_ms = 0.0, min_ms = 1e9, max_ms = 0.0;
    {
        std::lock_guard<std::mutex> lk(window_mutex);
        int n = window_full ? 200 : window_idx;
        if (n > 0) {
            std::array<double,200> sorted{};
            double sum = 0.0;
            for (int i = 0; i < n; ++i) {
                sum += latency_window[i]; sorted[i] = latency_window[i];
                if (latency_window[i] < min_ms) min_ms = latency_window[i];
                if (latency_window[i] > max_ms) max_ms = latency_window[i];
            }
            mean_ms = sum / n;
            std::sort(sorted.begin(), sorted.begin() + n);
            p95_ms = sorted[static_cast<int>(n * 0.95)];
        }
    }
    uint64_t rx   = packets_received.load();
    uint64_t drop = packets_dropped.load();
    uint64_t undr = underruns.load();
    double   loss = (rx + drop) > 0 ? 100.0 * drop / (rx + drop) : 0.0;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    SESSION SUMMARY                       ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "║  Packets received  : " << std::left << std::setw(34) << rx   << "║\n";
    std::cout << "║  Packets dropped   : " << std::left << std::setw(34) << drop << "║\n";
    std::cout << "║  Jitter underruns  : " << std::left << std::setw(34) << undr << "║\n";
    std::cout << "║  Packet loss       : " << std::left << std::setw(31) << loss << " % ║\n";
    std::cout << "║  Min latency       : " << std::left << std::setw(30) << min_ms  << " ms ║\n";
    std::cout << "║  Mean latency      : " << std::left << std::setw(30) << mean_ms << " ms ║\n";
    std::cout << "║  P95 latency       : " << std::left << std::setw(30) << p95_ms  << " ms ║\n";
    std::cout << "║  Max latency       : " << std::left << std::setw(30) << max_ms  << " ms ║\n";
    std::cout << "║  Jitter (p95-mean) : " << std::left << std::setw(30) << (p95_ms-mean_ms) << " ms ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Recordings folder : " << std::left << std::setw(34) << RECORDINGS_DIR << "║\n";
    std::cout << "║  Last clip         : " << std::left << std::setw(34) << current_wav_path << "║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::string node_a_host = "127.0.0.1";
    if (argc >= 2) node_a_host = argv[1];
    node_a_ws_url = "ws://" + node_a_host + ":9001";

    ensure_recordings_dir();
    ix::initNetSystem();

    // ── Playback device ───────────────────────────────────────────────────────
    //  We open at 48 kHz initially; if the first packet says a different rate
    //  we'll log a warning.  Reopening the device mid-session to change rate
    //  would cause a glitch — so we use miniaudio's resampler if needed.
    //  In practice Node A and Node B run on the same machine so the rate always
    //  matches.
    // ──────────────────────────────────────────────────────────────────────────
    uint32_t playback_sr = g_sample_rate.load();

    ma_device_config dcfg   = ma_device_config_init(ma_device_type_playback);
    dcfg.playback.format    = MA_FMT;
    dcfg.playback.channels  = CHANNELS;
    dcfg.sampleRate         = playback_sr;
    dcfg.dataCallback       = playback_data_callback;
    dcfg.periodSizeInFrames = playback_sr / 100; // 10 ms

    ma_device playback_device;
    if (ma_device_init(NULL, &dcfg, &playback_device) != MA_SUCCESS) {
        std::cerr << "[NODE B] Failed to init playback device\n"; return -1;
    }
    ma_device_start(&playback_device);
    std::cout << "[NODE B] Playback ready : "
              << playback_device.sampleRate << " Hz / 32-bit float / mono\n";

    // ── WebSocket client ──────────────────────────────────────────────────────
    ix::WebSocket webSocket;
    webSocket.setUrl(node_a_ws_url);
    webSocket.enableAutomaticReconnection();
    webSocket.setMaxWaitBetweenReconnectionRetries(2000);

    webSocket.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {

        if (msg->type == ix::WebSocketMessageType::Open) {
            std::cout << "\n[NODE B] Connected to Node A!\n";
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

        else if (msg->type == ix::WebSocketMessageType::Close) {
            std::cout << "\n[NODE B] Disconnected. Reconnecting...\n";
        }

        else if (msg->type == ix::WebSocketMessageType::Message) {
            if (msg->str.size() < sizeof(AudioPacket)) return;

            const AudioPacket* hdr =
                reinterpret_cast<const AudioPacket*>(msg->str.data());

            if (hdr->magic != AUDIO_PACKET_MAGIC) return;

            // ── Validate payload size ──────────────────────────────────────────
            // Reject packets whose declared payload doesn't match actual message size.
            // This catches partial WebSocket frames and corrupt packets.
            size_t expected_msg_size = sizeof(AudioPacket) + hdr->payload_bytes;
            if (msg->str.size() != expected_msg_size) return;

            // ── Update sample rate from packet (Node A is authoritative) ───────
            uint32_t pkt_sr = hdr->sample_rate;
            if (pkt_sr != g_sample_rate.load() && pkt_sr > 0) {
                g_sample_rate.store(pkt_sr);
                if (pkt_sr != playback_device.sampleRate) {
                    std::cout << "[NODE B] NOTE: Node A sample rate = " << pkt_sr
                              << " Hz, playback = " << playback_device.sampleRate
                              << " Hz. miniaudio will resample.\n";
                }
            }

            // ── New clip detection ─────────────────────────────────────────────
            uint32_t incoming_clip = hdr->clip_index;
            if (incoming_clip != current_clip_index.load()) {
                current_clip_index.store(incoming_clip);
                g_session_id       = hdr->session_id;
                g_session_start_us = hdr->session_start_us;

                // Reset per-clip stats
                packets_received.store(0);
                packets_dropped.store(0);
                underruns.store(0);
                last_seq.store(UINT32_MAX);
                {
                    std::lock_guard<std::mutex> lk(window_mutex);
                    window_idx = 0; window_full = false;
                }
                jitter_buffer.clear();
                last_good_pcm.clear();
                residual_pcm.clear(); // <--- ADD THIS LINE
                playback_armed.store(false); // require re-fill for new clip

                open_new_wav(incoming_clip, hdr->session_id,
                             hdr->session_start_us, pkt_sr);
            }

            // ── Sequence gap detection ─────────────────────────────────────────
            uint32_t prev = last_seq.load();
            if (prev != UINT32_MAX) {
                uint32_t gap = hdr->sequence_number - (prev + 1);
                if (gap > 0) packets_dropped += gap;
            }
            last_seq.store(hdr->sequence_number);

            // ── Latency ────────────────────────────────────────────────────────
            uint64_t now        = get_current_time_us();
            double   latency_ms = (now - hdr->timestamp_us) / 1000.0;
            last_latency_ms.store(latency_ms);
            packets_received++;

            double mean_ms = 0.0, p95_ms = 0.0;
            {
                std::lock_guard<std::mutex> lk(window_mutex);
                latency_window[window_idx] = latency_ms;
                window_idx = (window_idx + 1) % 200;
                if (window_idx == 0) window_full = true;
                int n = window_full ? 200 : window_idx;
                double sum = 0.0;
                std::array<double,200> sorted{};
                for (int i = 0; i < n; ++i) { sum += latency_window[i]; sorted[i] = latency_window[i]; }
                mean_ms = sum / n;
                std::sort(sorted.begin(), sorted.begin() + n);
                p95_ms = sorted[static_cast<int>(n * 0.95)];
            }

            // ── Push to jitter buffer ─────────────────────────────────────────
            // Drop oldest packet if buffer is overflowing (burst catch-up).
            if (jitter_buffer.size() >= static_cast<size_t>(HIGH_WATER_MARK)) {
                std::vector<uint8_t> discard;
                jitter_buffer.pop(discard);
            }
            std::vector<uint8_t> data(msg->str.begin(), msg->str.end());
            jitter_buffer.push(data);

            // ── Pre-fill gate: arm playback once we have enough packets ────────
            if (!playback_armed.load(std::memory_order_acquire)) {
                if (jitter_buffer.size() >= static_cast<size_t>(TARGET_FILL_PACKETS))
                    playback_armed.store(true, std::memory_order_release);
            }

            // ── Console row ────────────────────────────────────────────────────
            std::cout << std::left << std::fixed << std::setprecision(2)
                      << std::setw(6)  << incoming_clip
                      << std::setw(8)  << hdr->sequence_number
                      << std::setw(8)  << jitter_buffer.size()
                      << std::setw(14) << latency_ms
                      << std::setw(14) << mean_ms
                      << std::setw(14) << p95_ms
                      << std::setw(8)  << underruns.load()
                      << "\n";
        }
    });

    std::cout << "[NODE B] Connecting to " << node_a_ws_url << "...\n";
    webSocket.start();
    std::cout << "[NODE B] Press ENTER to quit.\n\n";
    std::cin.get();

    webSocket.stop();
    ma_device_uninit(&playback_device);

    // ── Close WAV + embed metadata ─────────────────────────────────────────────
    std::string final_path;
    uint64_t    final_clip_start = 0;
    {
        std::lock_guard<std::mutex> lk(wav_mutex);
        if (wav_encoder_ready) { ma_encoder_uninit(&wav_encoder); wav_encoder_ready = false; }
        final_path       = current_wav_path;
        final_clip_start = clip_start_us;
    }

    double final_mean = 0.0, final_p95 = 0.0;
    {
        std::lock_guard<std::mutex> lk(window_mutex);
        int n = window_full ? 200 : window_idx;
        if (n > 0) {
            std::array<double,200> sorted{};
            double sum = 0.0;
            for (int i = 0; i < n; ++i) { sum += latency_window[i]; sorted[i] = latency_window[i]; }
            final_mean = sum / n;
            std::sort(sorted.begin(), sorted.begin() + n);
            final_p95 = sorted[static_cast<int>(n * 0.95)];
        }
    }

    if (!final_path.empty()) {
        append_info_chunk(final_path,
                          current_clip_index.load(), g_session_id,
                          g_session_start_us, final_clip_start,
                          g_sample_rate.load(), node_a_ws_url,
                          packets_received.load(), packets_dropped.load(),
                          final_mean, final_p95);
        std::cout << "[NODE B] Metadata embedded → " << final_path << "\n";
    }

    print_session_summary();
    ix::uninitNetSystem();
    return 0;
}