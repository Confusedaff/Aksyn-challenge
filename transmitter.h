#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>
#include "protocol.h"
#include "session.h"
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Transmitter — owns the IXWebSocket server and the list of connected clients.
//
//  Responsibilities:
//    • Accept incoming WebSocket connections from node_B instances.
//    • Assign each connection a unique clip_index (from the Session).
//    • Expose send_audio() which is called from the audio-capture callback.
//      send_audio() serialises an AudioPacket header + PCM payload and sends
//      it as a binary WebSocket message to every connected client.
//    • Reply to PING: text messages with PONG: (used by node_B for RTT probes).
//
//  Thread safety:
//    send_audio() is called from the miniaudio audio thread.  It takes a
//    short-lived shared_lock on the client list; client connect/disconnect
//    takes an exclusive lock.  IXWebSocket's sendBinary() is itself thread-safe.
// ─────────────────────────────────────────────────────────────────────────────
class Transmitter {
public:
    explicit Transmitter(Session& session) : session_(session) {}
    ~Transmitter();

    // Bind and start accepting connections.  Returns false on failure.
    bool listen(const std::string& host, uint16_t port);

    // Stop the server and disconnect all clients.
    void stop();

    // Called from the audio-capture callback (real-time thread).
    // Builds and broadcasts an AudioPacket for every connected client.
    void send_audio(const float* pcm, uint32_t frames, uint32_t sample_rate);

private:
    struct ClientState {
        std::shared_ptr<ix::WebSocket> ws;
        uint32_t clip_index = 0;
    };

    Session&                              session_;
    std::unique_ptr<ix::WebSocketServer>  server_;
    std::vector<ClientState>              clients_;
    std::mutex                            clients_mtx_;
    uint32_t                              seq_ = 0;

    void on_open (ix::WebSocket& ws);
    void on_close(ix::WebSocket& ws);
    void on_text (ix::WebSocket& ws, const std::string& msg);
};
