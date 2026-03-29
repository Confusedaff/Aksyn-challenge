#include "protocol.h"
#include "session.h"
#include "audio_capture.h"
#include "transmitter.h"

#include <ixwebsocket/IXNetSystem.h>
#include <iostream>
#include <iomanip>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
static void print_config(uint32_t sample_rate) {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║          NODE A — AUDIO CONFIGURATION                    ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Sample rate   : " << std::left << std::setw(39) << sample_rate       << "║\n";
    std::cout << "║  Format        : 32-bit float                            ║\n";
    std::cout << "║  Channels      : 1 (mono)                                ║\n";
    std::cout << "║  Period        : 10 ms                                   ║\n";
    std::cout << "║  WASAPI mode   : Exclusive (bypasses Windows mixer)      ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // ── Parse args ────────────────────────────────────────────────────────────
    std::string bind_host = "0.0.0.0";
    if (argc >= 2) bind_host = argv[1];

    // ── Init ──────────────────────────────────────────────────────────────────
    ix::initNetSystem();

    Session session;
    session.init();

    std::cout << "[NODE A] Session ID    : 0x" << std::hex << session.id << std::dec << "\n";
    std::cout << "[NODE A] Session start : " << session.start_timestamp() << "\n\n";

    // ── 1. Probe native audio rate ────────────────────────────────────────────
    uint32_t sample_rate = AudioCapture::probe_native_sample_rate();
    print_config(sample_rate);

    // ── 2. Start WebSocket server (Transmitter) ───────────────────────────────
    Transmitter transmitter(session);
    if (!transmitter.listen(bind_host, 9001))
        return 1;

    // ── 3. Start audio capture — callback forwards PCM to Transmitter ─────────
    AudioCapture capture;
    bool ok = capture.start(sample_rate,
        [&](const float* pcm, uint32_t frames, uint32_t sr) {
            transmitter.send_audio(pcm, frames, sr);
        });

    if (!ok) {
        std::cerr << "[NODE A] Failed to start audio capture\n";
        transmitter.stop();
        ix::uninitNetSystem();
        return 1;
    }

    std::cout << "[NODE A] Streaming on " << bind_host << ":9001\n";
    std::cout << "[NODE A] Press ENTER to quit.\n\n";
    std::cin.get();

    // ── Teardown (reverse order) ──────────────────────────────────────────────
    capture.stop();
    transmitter.stop();
    ix::uninitNetSystem();
    return 0;
}
