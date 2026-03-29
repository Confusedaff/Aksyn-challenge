#include "protocol.h"
#include "wav_saver.h"
#include "playback.h"
#include "receiver.h"

#include <ixwebsocket/IXNetSystem.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <array>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
static void print_session_summary(const Receiver& rx, const std::string& rec_dir,
                                   const std::string& last_path)
{
    ClipStats s = rx.current_clip_stats();

    uint64_t rx_ct  = rx.packets_received();
    uint64_t drop   = rx.packets_dropped();
    uint64_t undr   = rx.underruns();
    double   loss   = (rx_ct + drop) > 0 ? 100.0 * drop / (rx_ct + drop) : 0.0;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    SESSION SUMMARY                       ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "║  Packets received  : " << std::left << std::setw(34) << rx_ct << "║\n";
    std::cout << "║  Packets dropped   : " << std::left << std::setw(34) << drop  << "║\n";
    std::cout << "║  Jitter underruns  : " << std::left << std::setw(34) << undr  << "║\n";
    std::cout << "║  Packet loss       : " << std::left << std::setw(31) << loss  << " % ║\n";
    std::cout << "║  Mean latency      : " << std::left << std::setw(30) << s.mean_latency_ms << " ms ║\n";
    std::cout << "║  P95 latency       : " << std::left << std::setw(30) << s.p95_latency_ms  << " ms ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Recordings folder : " << std::left << std::setw(34) << rec_dir   << "║\n";
    std::cout << "║  Last clip         : " << std::left << std::setw(34) << last_path << "║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // ── Parse args ────────────────────────────────────────────────────────────
    std::string node_a_host = "127.0.0.1";
    if (argc >= 2) node_a_host = argv[1];
    std::string ws_url = "ws://" + node_a_host + ":9001";

    ix::initNetSystem();

    // ── 1. WavSaver — start background writer thread ──────────────────────────
    WavSaver saver;
    saver.start();

    // ── 2. Receiver — owns jitter buffer and stat tracking ────────────────────
    Receiver receiver(saver);

    // ── 3. Playback device — pulls audio from Receiver on the audio thread ────
    //  Open at 48 kHz initially; miniaudio will resample if node_A sends a
    //  different rate (in practice they're on the same machine so rates match).
    Playback playback;
    if (!playback.start(48000,
            [&](float* out, uint32_t frames) {
                receiver.pull_audio(out, frames);
            }))
    {
        std::cerr << "[NODE B] Failed to start playback device\n";
        saver.stop();
        ix::uninitNetSystem();
        return 1;
    }

    std::cout << "[NODE B] Recordings → " << WavSaver::recordings_dir() << "\n";
    std::cout << "[NODE B] Press ENTER to quit.\n\n";

    // ── 4. Receiver::run() blocks here until ENTER ────────────────────────────
    receiver.run(ws_url);

    // ── Teardown ──────────────────────────────────────────────────────────────
    playback.stop();

    // Stop writer thread (drains queue) then close encoder + embed metadata
    saver.stop();
    ClipStats final_stats = receiver.current_clip_stats();
    std::string final_path = saver.current_path();
    saver.close_and_embed_metadata(final_stats);

    print_session_summary(receiver, WavSaver::recordings_dir(), final_path);

    ix::uninitNetSystem();
    return 0;
}
