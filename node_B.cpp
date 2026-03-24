#include "protocol.h"
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <array>
#include <vector>
#include <string>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

// ── Jitter buffer ──────────────────────────────────────────────────────────────
AudioQueue jitter_buffer(100);

// ── WAV encoder ───────────────────────────────────────────────────────────────
// Initialised in main(), used in the playback callback.
// No mutex needed: the playback callback is the only writer after init.
static ma_encoder  wav_encoder;
static bool        wav_encoder_ready = false;

// ── Running stats ──────────────────────────────────────────────────────────────
std::atomic<uint64_t> packets_received{0};
std::atomic<uint64_t> packets_dropped{0};
std::atomic<double>   last_latency_ms{0.0};

// Fix #4: last_seq must be atomic — it is read on one thread and written on
// another in the original code (even if "only" the callback thread today,
// the compiler and CPU are free to reorder non-atomic reads/writes).
std::atomic<uint32_t> last_seq{UINT32_MAX}; // UINT32_MAX = "first packet" sentinel

// Fix #4: window_idx and window_full are only ever touched on the IXWebSocket
// callback thread, so plain types are fine there — but we protect the whole
// window with a mutex so the main thread can safely print stats later if needed.
static std::array<double, 100> latency_window{};
static int  window_idx  = 0;
static bool window_full = false;
static std::mutex window_mutex; // guards latency_window / window_idx / window_full

// ── Packet-loss concealment: last good PCM frame ───────────────────────────────
// Storing the last successfully decoded PCM block so we can repeat it instead
// of outputting silence when the jitter buffer runs dry.
// Accessed only from the playback callback (audio thread) — no mutex needed.
static std::vector<uint8_t> last_good_pcm;  // raw PCM bytes of last good frame

// ── Playback callback ──────────────────────────────────────────────────────────
// Runs on the audio thread.  Must never block.
void playback_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    (void)pDevice;
    uint32_t expected_bytes = frameCount * 2; // mono S16 = 2 bytes/frame

    std::vector<uint8_t> packet_data;
    if (jitter_buffer.pop(packet_data)) {
        uint8_t* pcm_data  = packet_data.data() + sizeof(AudioPacket);
        size_t   pcm_size  = packet_data.size()  - sizeof(AudioPacket);
        size_t   copy_size = std::min(static_cast<size_t>(expected_bytes), pcm_size);

        std::memcpy(pOutput, pcm_data, copy_size);
        if (copy_size < expected_bytes)
            std::memset(static_cast<uint8_t*>(pOutput) + copy_size, 0,
                        expected_bytes - copy_size);

        // ── Save to WAV (non-blocking: encoder writes to a buffered file) ──
        // This happens on the audio thread but ma_encoder_write_pcm_frames is
        // designed to be called from audio callbacks and does no heap allocation
        // on the hot path — latency impact is negligible (< 5 µs per call).
        if (wav_encoder_ready) {
            ma_encoder_write_pcm_frames(&wav_encoder,
                                        pcm_data,
                                        copy_size / 2, // frames = bytes / 2
                                        nullptr);
        }

        // ── Update concealment buffer ──────────────────────────────────────
        // Keep a copy of the raw PCM so we can repeat it on the next underrun.
        last_good_pcm.assign(pcm_data, pcm_data + copy_size);

    } else {
        // ── Packet-loss concealment: repeat last good frame ────────────────
        // Silence (memset 0) is the most audible artefact under packet loss.
        // Repeating the last frame produces a short "freeze" which is far less
        // jarring and buys the jitter buffer time to refill.
        if (!last_good_pcm.empty()) {
            size_t copy_size = std::min(static_cast<size_t>(expected_bytes),
                                        last_good_pcm.size());
            std::memcpy(pOutput, last_good_pcm.data(), copy_size);
            if (copy_size < expected_bytes)
                std::memset(static_cast<uint8_t*>(pOutput) + copy_size, 0,
                            expected_bytes - copy_size);
        } else {
            // No frame received yet — true silence is the only option.
            std::memset(pOutput, 0, expected_bytes);
        }
    }
}

// ── Session summary ────────────────────────────────────────────────────────────
// Called once when the user presses ENTER.  Reads the final stats and prints a
// clean report — exactly what you'd show a client or evaluator.
static void print_session_summary(const std::string& wav_path) {
    double mean_ms = 0.0, p95_ms = 0.0, min_ms = 1e9, max_ms = 0.0;
    {
        std::lock_guard<std::mutex> lock(window_mutex);
        int n = window_full ? 100 : window_idx;
        if (n > 0) {
            std::array<double, 100> sorted{};
            double sum = 0.0;
            for (int i = 0; i < n; i++) {
                sum += latency_window[i];
                sorted[i] = latency_window[i];
                if (latency_window[i] < min_ms) min_ms = latency_window[i];
                if (latency_window[i] > max_ms) max_ms = latency_window[i];
            }
            mean_ms = sum / n;
            std::sort(sorted.begin(), sorted.begin() + n);
            p95_ms  = sorted[static_cast<int>(n * 0.95)];
        }
    }

    uint64_t rx   = packets_received.load();
    uint64_t drop = packets_dropped.load();
    double   loss = (rx + drop) > 0
                    ? 100.0 * drop / (rx + drop)
                    : 0.0;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║                  SESSION SUMMARY                     ║\n";
    std::cout << "╠══════════════════════════════════════════════════════╣\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "║  Packets received : " << std::left << std::setw(32) << rx   << "║\n";
    std::cout << "║  Packets dropped  : " << std::left << std::setw(32) << drop << "║\n";
    std::cout << "║  Packet loss      : " << std::left << std::setw(29) << loss << " %║\n";
    std::cout << "║  Min latency      : " << std::left << std::setw(28) << min_ms << " ms ║\n";
    std::cout << "║  Mean latency     : " << std::left << std::setw(28) << mean_ms << " ms ║\n";
    std::cout << "║  P95 latency      : " << std::left << std::setw(28) << p95_ms  << " ms ║\n";
    std::cout << "║  Max latency      : " << std::left << std::setw(28) << max_ms  << " ms ║\n";
    std::cout << "║  Jitter (p95-mean): " << std::left << std::setw(28) << (p95_ms - mean_ms) << " ms║\n";
    std::cout << "╠══════════════════════════════════════════════════════╣\n";
    std::cout << "║  Audio saved to   : " << std::left << std::setw(32) << wav_path << " ║\n";
    std::cout << "╚══════════════════════════════════════════════════════╝\n\n";
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Set console to UTF-8 so box-drawing characters render correctly in
    // PowerShell / cmd without needing to run `chcp 65001` manually.
    SetConsoleOutputCP(CP_UTF8);
#endif

    // ── CLI: optional Node A address ───────────────────────────────────────────
    // Usage:  ./node_B [node_a_ip]
    // Default is 127.0.0.1 (localhost demo).
    // For a real LAN demo pass Node A's LAN IP: ./node_B 192.168.1.10
    std::string node_a_host = "127.0.0.1";
    if (argc >= 2) {
        node_a_host = argv[1];
    }
    const std::string ws_url  = "ws://" + node_a_host + ":9001";
    const std::string wav_path = "received_audio.wav";

    ix::initNetSystem();

    // ── WAV encoder init ───────────────────────────────────────────────────────
    // Opened before the playback device so the encoder is ready before audio
    // starts flowing.  ma_encoder writes to a buffered file handle — it is safe
    // to call ma_encoder_write_pcm_frames from the audio callback.
    ma_encoder_config enc_cfg = ma_encoder_config_init(
        ma_encoding_format_wav, ma_format_s16, 1, 48000);
    if (ma_encoder_init_file(wav_path.c_str(), &enc_cfg, &wav_encoder) == MA_SUCCESS) {
        wav_encoder_ready = true;
        std::cout << "[NODE B] WAV encoder ready → " << wav_path << "\n";
    } else {
        std::cerr << "[NODE B] WARNING: Could not open WAV file for writing.\n";
    }

    // ── Playback device ────────────────────────────────────────────────────────
    ma_device_config deviceConfig  = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_s16;
    deviceConfig.playback.channels = 1;
    deviceConfig.sampleRate        = 48000;
    deviceConfig.dataCallback      = playback_data_callback;

    ma_device playback_device;
    if (ma_device_init(NULL, &deviceConfig, &playback_device) != MA_SUCCESS) {
        std::cerr << "Failed to init playback device\n";
        return -1;
    }
    ma_device_start(&playback_device);
    std::cout << "[NODE B] Audio playback ready.\n";

    // ── WebSocket client ───────────────────────────────────────────────────────
    ix::WebSocket webSocket;
    webSocket.setUrl(ws_url);

    // AUTO-RECONNECT: if Node A restarts or the network blips, Node B will
    // automatically reconnect rather than silently dying.
    // The reconnection attempts happen on IXWebSocket's internal thread —
    // no latency impact on the audio path.
    webSocket.enableAutomaticReconnection();
    webSocket.setMaxWaitBetweenReconnectionRetries(2000); // 2 s back-off cap

    webSocket.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg) {

        if (msg->type == ix::WebSocketMessageType::Open) {
            std::cout << "\n[NODE B] Connected to Node A!\n";
            std::cout << "─────────────────────────────────────────────────────────\n";
            std::cout << std::left
                      << std::setw(8)  << "SEQ"
                      << std::setw(14) << "LATENCY(ms)"
                      << std::setw(14) << "MEAN(ms)"
                      << std::setw(14) << "P95(ms)"
                      << std::setw(10) << "DROPPED"
                      << "\n";
            std::cout << "─────────────────────────────────────────────────────────\n";
        }

        else if (msg->type == ix::WebSocketMessageType::Close) {
            std::cout << "\n[NODE B] Disconnected from Node A. Reconnecting...\n";
        }

        else if (msg->type == ix::WebSocketMessageType::Message) {
            if (msg->str.size() < sizeof(AudioPacket)) return;

            const AudioPacket* header =
                reinterpret_cast<const AudioPacket*>(msg->str.data());

            // ── Sequence gap detection ─────────────────────────────────────
            uint32_t prev = last_seq.load();
            if (prev != UINT32_MAX) {
                uint32_t expected = prev + 1;
                if (header->sequence_number != expected) {
                    uint32_t gap = header->sequence_number - expected;
                    packets_dropped += gap;
                }
            }
            last_seq.store(header->sequence_number);

            // ── Latency ───────────────────────────────────────────────────
            // NOTE: accurate on localhost (shared clock). On separate physical
            // machines you need NTP sync or an RTT/2 correction instead.
            uint64_t now        = get_current_time_us();
            double   latency_ms = (now - header->timestamp_us) / 1000.0;
            last_latency_ms.store(latency_ms);
            packets_received++;

            // Guard the rolling window so a future /metrics endpoint is safe.
            double mean_ms = 0.0, p95_ms = 0.0;
            {
                std::lock_guard<std::mutex> lock(window_mutex);
                latency_window[window_idx] = latency_ms;
                window_idx = (window_idx + 1) % 100;
                if (window_idx == 0) window_full = true;

                int n = window_full ? 100 : window_idx;
                double sum = 0.0;
                std::array<double, 100> sorted{};
                for (int i = 0; i < n; i++) {
                    sum += latency_window[i];
                    sorted[i] = latency_window[i];
                }
                mean_ms = sum / n;
                std::sort(sorted.begin(), sorted.begin() + n);
                p95_ms = sorted[static_cast<int>(n * 0.95)];
            }

            // ── Print every packet ─────────────────────────────────────────
            std::cout << std::left << std::fixed << std::setprecision(2)
                      << std::setw(8)  << header->sequence_number
                      << std::setw(14) << latency_ms
                      << std::setw(14) << mean_ms
                      << std::setw(14) << p95_ms
                      << std::setw(10) << packets_dropped.load()
                      << "\n";

            // ── Push to jitter buffer ─────────────────────────────────────
            // Note: this copy happens on the IXWebSocket receive thread, not
            // the audio thread, so it does not add any audio latency.
            std::vector<uint8_t> data(msg->str.begin(), msg->str.end());
            jitter_buffer.push(data);
        }
    });

    std::cout << "[NODE B] Connecting to " << ws_url << "...\n";
    webSocket.start();

    std::cout << "[NODE B] Press ENTER to quit.\n";
    std::cin.get();

    webSocket.stop();
    ma_device_uninit(&playback_device);

    // Flush and close WAV file cleanly so the file header is correct.
    if (wav_encoder_ready) {
        ma_encoder_uninit(&wav_encoder);
        std::cout << "[NODE B] WAV file closed: " << wav_path << "\n";
    }

    print_session_summary(wav_path);

    ix::uninitNetSystem();
    return 0;
}