#pragma once
#include <cstdint>
#include <string>
#include <atomic>
#include "protocol.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Session — owns the random session-ID, start timestamp, and clip counter
//  that are stamped into every outgoing AudioPacket header.
//
//  A new Session object is created once at node_A startup.  Its fields are
//  read from the audio-capture callback (real-time thread) so they are stored
//  as plain POD values guarded by the fact that they are written once before
//  any audio thread starts.
// ─────────────────────────────────────────────────────────────────────────────
struct Session {
    uint32_t id            = 0;   // random 32-bit ID generated at startup
    uint64_t start_us      = 0;   // wall-clock µs when node_A started
    std::atomic<uint32_t> clip_index{0}; // incremented per node_B connection

    // Generate a new random session
    void init();

    // Human-readable ISO-8601 start time (used in log output)
    std::string start_timestamp() const {
        return timestamp_for_filename(start_us);
    }
};
