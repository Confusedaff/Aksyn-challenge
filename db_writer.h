#pragma once
#include <string>
#include <cstdint>

void db_record_clip(
    uint32_t    clip_index,
    uint32_t    session_id,
    uint64_t    session_start_us,
    uint64_t    clip_start_us,
    uint32_t    sample_rate,
    const std::string& file_path,
    uint64_t    pkts_rx,
    uint64_t    pkts_dropped,
    double      mean_latency_ms,
    double      p95_latency_ms
);

std::string db_path();