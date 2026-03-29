#include "transmitter.h"
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <vector>

static constexpr uint16_t CHANNELS        = 1;
static constexpr uint16_t BITS_PER_SAMPLE = 32; // 32-bit float

// ─────────────────────────────────────────────────────────────────────────────
Transmitter::~Transmitter() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
bool Transmitter::listen(const std::string& host, uint16_t port) {
    server_ = std::make_unique<ix::WebSocketServer>(port, host);

    server_->setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> /*state*/,
               ix::WebSocket& ws,
               const ix::WebSocketMessagePtr& msg)
        {
            if (msg->type == ix::WebSocketMessageType::Open)
                on_open(ws);
            else if (msg->type == ix::WebSocketMessageType::Close)
                on_close(ws);
            else if (msg->type == ix::WebSocketMessageType::Message && !msg->binary)
                on_text(ws, msg->str);
        });

    auto [ok, err] = server_->listen();
    if (!ok) {
        std::cerr << "[TRANSMITTER] Listen failed: " << err << "\n";
        return false;
    }
    server_->start();
    std::cout << "[TRANSMITTER] Listening on " << host << ":" << port << "\n";
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void Transmitter::stop() {
    if (server_) {
        server_->stop();
        server_.reset();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_audio  (called from miniaudio audio thread — must not block)
// ─────────────────────────────────────────────────────────────────────────────
void Transmitter::send_audio(const float* pcm, uint32_t frames, uint32_t sample_rate) {
    uint16_t payload_bytes =
        static_cast<uint16_t>(frames * CHANNELS * (BITS_PER_SAMPLE / 8));

    // Build packet header
    AudioPacket hdr{};
    hdr.magic            = AUDIO_PACKET_MAGIC;
    hdr.sequence_number  = seq_++;
    hdr.timestamp_us     = get_current_time_us();
    hdr.session_start_us = session_.start_us;
    hdr.session_id       = session_.id;
    hdr.sample_rate      = sample_rate;
    hdr.channels         = CHANNELS;
    hdr.bits_per_sample  = BITS_PER_SAMPLE;
    hdr.payload_bytes    = payload_bytes;
    hdr.reserved         = 0;

    // Serialise: header + PCM payload into a single buffer
    std::string packet;
    packet.resize(sizeof(AudioPacket) + payload_bytes);
    std::memcpy(packet.data(),                       &hdr, sizeof(AudioPacket));
    std::memcpy(packet.data() + sizeof(AudioPacket), pcm,  payload_bytes);

    // Broadcast to all connected clients (each gets its own clip_index stamp)
    std::lock_guard<std::mutex> lk(clients_mtx_);
    for (auto& cs : clients_) {
        reinterpret_cast<AudioPacket*>(packet.data())->clip_index = cs.clip_index;
        cs.ws->sendBinary(packet);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Private event handlers
// ─────────────────────────────────────────────────────────────────────────────
void Transmitter::on_open(ix::WebSocket& ws) {
    uint32_t clip = session_.clip_index.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        ClientState cs;
        cs.ws         = std::shared_ptr<ix::WebSocket>(&ws, [](ix::WebSocket*){});
        cs.clip_index = clip;
        clients_.push_back(std::move(cs));
    }
    std::cout << "[TRANSMITTER] Node B connected — clip #" << clip
              << "  session=0x" << std::hex << std::setw(8) << std::setfill('0')
              << session_.id << std::dec << "\n";
}

void Transmitter::on_close(ix::WebSocket& ws) {
    std::lock_guard<std::mutex> lk(clients_mtx_);
    for (auto it = clients_.begin(); it != clients_.end(); ++it) {
        if (it->ws.get() == &ws) { clients_.erase(it); break; }
    }
    std::cout << "[TRANSMITTER] Node B disconnected.\n";
}

void Transmitter::on_text(ix::WebSocket& ws, const std::string& msg) {
    // Simple PING/PONG RTT probe used by node_B
    if (msg.rfind("PING:", 0) == 0)
        ws.sendText("PONG:" + msg.substr(5));
}
