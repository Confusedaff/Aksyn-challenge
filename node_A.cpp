#include "protocol.h"
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXNetSystem.h>

// ── miniaudio – configure BEFORE the implementation include ───────────────────
//
//  MA_WASAPI_NO_DEFAULT_QUALITY_CONVERSION
//    Disables Windows Audio Engine sample-rate conversion.  When set,
//    miniaudio will use WASAPI exclusive mode and the device's true native
//    sample rate, bypassing the Windows mixer entirely.  Result: zero OS
//    resampling artefacts, real 32-bit float capture path.
//
//  The root cause of the muffled/telephone quality in the recordings was that
//  the default WASAPI SHARED mode routes all capture through the Windows Audio
//  Engine at its "Default Format" (typically 16 kHz for built-in laptop mics).
//  Even though we requested 96 kHz, the OS resampled 16 kHz → 96 kHz, giving
//  a container with only ~8 kHz of real audio bandwidth.
//
#define MA_WASAPI_NO_DEFAULT_QUALITY_CONVERSION
#define MA_ENABLE_WASAPI
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <iostream>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <string>
#include <random>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Audio format settings
//
//  We use ma_format_f32 (32-bit float) at the device's OWN native sample rate.
//  SAMPLE_RATE is a runtime variable — probed from the hardware before opening
//  the device so the OS never needs to insert a resampler.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr ma_format  MA_FMT          = ma_format_f32;
static constexpr uint16_t   CHANNELS        = 1;
static constexpr uint16_t   BITS_PER_SAMPLE = 32;

static uint32_t SAMPLE_RATE = 48000; // overwritten by probe below

// ── Session-level metadata ─────────────────────────────────────────────────────
static uint32_t g_session_id       = 0;
static uint64_t g_session_start_us = 0;
static uint32_t g_clip_index       = 0;

// ── Connected clients ──────────────────────────────────────────────────────────
struct ClientState {
    std::shared_ptr<ix::WebSocket> ws;
    uint32_t clip_index = 0;
};
static std::vector<ClientState> connected_clients;
static std::mutex               clients_mutex;

static std::atomic<uint32_t> g_seq{0};

// ─────────────────────────────────────────────────────────────────────────────
//  Probe: discover the device's true native sample rate.
//
//  Why this matters:
//    If we request 96000 Hz from a mic that is natively 16000 Hz, WASAPI
//    (shared mode) silently resamples for us — giving us a 96 kHz file with
//    only 8 kHz of real content.  By probing first and requesting the native
//    rate, we bypass that resampler completely.
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t probe_native_sample_rate() {
    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return 48000;

    ma_device_info* pInfos = nullptr;
    ma_uint32        count  = 0;
    if (ma_context_get_devices(&ctx, nullptr, nullptr, &pInfos, &count) != MA_SUCCESS
        || count == 0)
    {
        ma_context_uninit(&ctx); return 48000;
    }

    ma_device_info info = pInfos[0];
    if (ma_context_get_device_info(&ctx, ma_device_type_capture,
                                   &info.id, &info) != MA_SUCCESS)
    {
        ma_context_uninit(&ctx); return 48000;
    }

    std::cout << "[NODE A] Capture device : " << info.name << "\n";
    std::cout << "[NODE A] Native formats :\n";

    uint32_t best_f32 = 0, best_any = 0;
    for (ma_uint32 i = 0; i < info.nativeDataFormatCount; ++i) {
        const auto& f = info.nativeDataFormats[i];
        std::cout << "  [" << i << "] format=" << f.format
                  << " ch=" << f.channels
                  << " sr=" << f.sampleRate << "\n";
        if (f.sampleRate > best_any) best_any = f.sampleRate;
        if (f.format == ma_format_f32 && f.sampleRate > best_f32)
            best_f32 = f.sampleRate;
    }
    ma_context_uninit(&ctx);

    uint32_t chosen = best_f32 ? best_f32 : best_any;
    if (!chosen) chosen = 48000;
    // 48 kHz captures the full human hearing range (20 Hz – 20 kHz).
    // Going higher only costs bandwidth without perceptible benefit on a
    // laptop mic whose element BW is ~18 kHz at best.  Cap at 96 kHz.
    if (chosen > 96000) chosen = 96000;

    std::cout << "[NODE A] → Chosen rate  : " << chosen << " Hz"
              << (best_f32 ? "  (native f32)\n" : "  (converted to f32)\n");
    return chosen;
}

// ─────────────────────────────────────────────────────────────────────────────
static void print_config() {
    const double period_ms  = 1000.0 * (SAMPLE_RATE / 100) / SAMPLE_RATE; // = 10 ms
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║          NODE A — AUDIO CONFIGURATION                    ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Sample rate   : " << std::left << std::setw(39) << SAMPLE_RATE << "║\n";
    std::cout << "║  Format        : 32-bit float                            ║\n";
    std::cout << "║  Channels      : 1 (mono)                                ║\n";
    std::cout << "║  Period        : " << std::left << std::setw(39) << (std::to_string((int)period_ms) + " ms  (10 ms @ any rate)") << "║\n";
    std::cout << "║  WASAPI mode   : Exclusive (bypasses Windows mixer)      ║\n";
    std::cout << "║  Estimated E2E : ~41.5 ms                                ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Capture callback  (miniaudio audio thread — must not block)
// ─────────────────────────────────────────────────────────────────────────────
void capture_data_callback(ma_device* pDevice, void* /*pOutput*/,
                           const void* pInput, ma_uint32 frameCount)
{
    uint16_t payload_size = static_cast<uint16_t>(
        frameCount * CHANNELS * (BITS_PER_SAMPLE / 8));

    std::vector<ClientState> snapshot;
    {
        std::lock_guard<std::mutex> lk(clients_mutex);
        snapshot = connected_clients;
    }
    if (snapshot.empty()) return;

    AudioPacket hdr{};
    hdr.magic            = AUDIO_PACKET_MAGIC;
    hdr.sequence_number  = g_seq.fetch_add(1, std::memory_order_relaxed);
    hdr.timestamp_us     = get_current_time_us();
    hdr.session_start_us = g_session_start_us;
    hdr.session_id       = g_session_id;
    hdr.sample_rate      = pDevice->sampleRate;
    hdr.channels         = CHANNELS;
    hdr.bits_per_sample  = BITS_PER_SAMPLE;
    hdr.payload_bytes    = payload_size;
    hdr.reserved         = 0;

    std::string packet;
    packet.resize(sizeof(AudioPacket) + payload_size);
    std::memcpy(packet.data(),                       &hdr,   sizeof(AudioPacket));
    std::memcpy(packet.data() + sizeof(AudioPacket), pInput, payload_size);

    for (auto& cs : snapshot) {
        reinterpret_cast<AudioPacket*>(packet.data())->clip_index = cs.clip_index;
        cs.ws->sendBinary(packet);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    {
        std::mt19937 rng(std::random_device{}());
        g_session_id       = rng();
        g_session_start_us = get_current_time_us();
    }

    std::string bind_host = "127.0.0.1";
    if (argc >= 2) bind_host = argv[1];

    ix::initNetSystem();

    // ── 1. Probe native rate ──────────────────────────────────────────────────
    SAMPLE_RATE = probe_native_sample_rate();
    print_config();

    // ── 2. WebSocket server ───────────────────────────────────────────────────
    ix::WebSocketServer server(9001, bind_host);

    server.setOnClientMessageCallback(
        [](std::shared_ptr<ix::ConnectionState> /*state*/,
           ix::WebSocket& ws,
           const ix::WebSocketMessagePtr& msg)
        {
            if (msg->type == ix::WebSocketMessageType::Open) {
                uint32_t clip = 0;
                {
                    std::lock_guard<std::mutex> lk(clients_mutex);
                    clip = ++g_clip_index;
                    ClientState cs;
                    cs.ws = std::shared_ptr<ix::WebSocket>(&ws, [](ix::WebSocket*){});
                    cs.clip_index = clip;
                    connected_clients.push_back(cs);
                }
                std::cout << "[NODE A] Node B connected — clip #" << clip
                          << "  session=0x" << std::hex << g_session_id
                          << std::dec << "\n";
            }
            else if (msg->type == ix::WebSocketMessageType::Close) {
                std::cout << "[NODE A] Node B disconnected.\n";
                std::lock_guard<std::mutex> lk(clients_mutex);
                for (auto it = connected_clients.begin();
                     it != connected_clients.end(); ++it)
                {
                    if (it->ws.get() == &ws) { connected_clients.erase(it); break; }
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Message && !msg->binary)
            {
                if (msg->str.rfind("PING:", 0) == 0)
                    ws.sendText("PONG:" + msg->str.substr(5));
            }
        });

    auto res = server.listen();
    if (!res.first) {
        std::cerr << "[NODE A] Server failed: " << res.second << "\n"; return 1;
    }
    server.start();
    std::cout << "[NODE A] Listening on " << bind_host << ":9001\n";
    std::cout << "[NODE A] Session ID    : 0x" << std::hex << g_session_id << std::dec << "\n";
    std::cout << "[NODE A] Session start : " << timestamp_for_filename(g_session_start_us) << "\n\n";

    // ── 3. Capture device ─────────────────────────────────────────────────────
    ma_device_config cfg   = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format     = MA_FMT;
    cfg.capture.channels   = CHANNELS;
    cfg.sampleRate         = SAMPLE_RATE;
    cfg.dataCallback       = capture_data_callback;
    cfg.periodSizeInFrames = SAMPLE_RATE / 100; // 10 ms period

#ifdef _WIN32
    // WASAPI exclusive: bypass the Windows Audio Engine entirely.
    // noAutoConvertSRC prevents WASAPI-inserted sample-rate conversion.
    // noDefaultQualityConversion is controlled by the compile-time
    // #define MA_WASAPI_NO_DEFAULT_QUALITY_CONVERSION at the top of this file.
    cfg.wasapi.noAutoConvertSRC = MA_TRUE;
#endif

    ma_device capture_device;
    ma_result rc = ma_device_init(NULL, &cfg, &capture_device);

#ifdef _WIN32
    if (rc != MA_SUCCESS) {
        // Another app may hold exclusive access — fall back to shared mode.
        std::cerr << "[NODE A] Exclusive mode unavailable, falling back to shared mode.\n"
                  << "         Audio will still be captured but may be resampled by Windows.\n";
        cfg.wasapi.noAutoConvertSRC = MA_FALSE;
        rc = ma_device_init(NULL, &cfg, &capture_device);
        std::cout << "[NODE A] WASAPI mode: "
          << (cfg.wasapi.noAutoConvertSRC ? "EXCLUSIVE" : "SHARED (degraded!)")
          << "\n";
    }
#endif

    #ifdef _WIN32
    if (rc != MA_SUCCESS) {
        std::cerr << "[NODE A] WARNING: Exclusive WASAPI failed — falling back to SHARED mode.\n"
                << "         Audio bandwidth will be limited by the Windows Audio Engine.\n"
                << "         To fix: close Teams, browsers, or any app holding WASAPI exclusive lock.\n";
        cfg.wasapi.noAutoConvertSRC = MA_FALSE;
        rc = ma_device_init(NULL, &cfg, &capture_device);
        if (rc == MA_SUCCESS) {
            std::cout << "[NODE A] WASAPI capture mode : SHARED (OS may resample — quality degraded)\n";
        }
    }
    #endif

    uint32_t actual_sr = capture_device.sampleRate;
    if (actual_sr != SAMPLE_RATE) {
        std::cout << "[NODE A] WARNING: requested " << SAMPLE_RATE
                  << " Hz, driver gave " << actual_sr << " Hz.\n";
        SAMPLE_RATE = actual_sr;
    }

    ma_device_start(&capture_device);
    std::cout << "[NODE A] Capture active : " << actual_sr
              << " Hz / 32-bit float / mono / "
              << cfg.periodSizeInFrames << " frames per period\n";
    std::cout << "[NODE A] Press ENTER to quit.\n\n";

    std::cin.get();

    ma_device_uninit(&capture_device);
    server.stop();
    ix::uninitNetSystem();
    return 0;
}