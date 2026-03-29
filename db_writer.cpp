#include "db_writer.h"
#include "sqlite3.h"
#include "protocol.h"
#include <iostream>
#include <filesystem>
#include <cstdio>

static const char* SCHEMA = R"sql(
CREATE TABLE IF NOT EXISTS recordings (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    clip_index       INTEGER NOT NULL,
    session_id       TEXT    NOT NULL,
    session_start_us INTEGER NOT NULL,
    clip_start_us    INTEGER NOT NULL,
    sample_rate      INTEGER NOT NULL,
    file_path        TEXT    NOT NULL UNIQUE,
    file_size_bytes  INTEGER NOT NULL DEFAULT 0,
    pkts_rx          INTEGER NOT NULL DEFAULT 0,
    pkts_dropped     INTEGER NOT NULL DEFAULT 0,
    mean_latency_ms  REAL    NOT NULL DEFAULT 0,
    p95_latency_ms   REAL    NOT NULL DEFAULT 0,
    created_at       TEXT    NOT NULL DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_session ON recordings(session_id);
CREATE INDEX IF NOT EXISTS idx_clip    ON recordings(clip_index);
)sql";

std::string db_path() {
    return (std::filesystem::current_path() / "recordings.db").string();
}

void db_record_clip(
    uint32_t clip_index,  uint32_t session_id,
    uint64_t session_start_us, uint64_t clip_start_us,
    uint32_t sample_rate, const std::string& file_path,
    uint64_t pkts_rx, uint64_t pkts_dropped,
    double mean_latency_ms, double p95_latency_ms)
{
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path().c_str(), &db) != SQLITE_OK) {
        std::cerr << "[DB] Open failed: " << sqlite3_errmsg(db) << "\n";
        return;
    }

    char* err = nullptr;
    sqlite3_exec(db, SCHEMA, nullptr, nullptr, &err);
    if (err) { sqlite3_free(err); }

    uintmax_t fsz = 0;
    try { fsz = std::filesystem::file_size(file_path); } catch (...) {}

    char sid[16];
    snprintf(sid, sizeof(sid), "0x%08X", session_id);

    const char* SQL = R"sql(
        INSERT OR IGNORE INTO recordings
            (clip_index, session_id, session_start_us, clip_start_us,
             sample_rate, file_path, file_size_bytes,
             pkts_rx, pkts_dropped, mean_latency_ms, p95_latency_ms)
        VALUES (?,?,?,?,?,?,?,?,?,?,?);
    )sql";

    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, SQL, -1, &s, nullptr);
    sqlite3_bind_int   (s,  1, (int)clip_index);
    sqlite3_bind_text  (s,  2, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (s,  3, (int64_t)session_start_us);
    sqlite3_bind_int64 (s,  4, (int64_t)clip_start_us);
    sqlite3_bind_int   (s,  5, (int)sample_rate);
    sqlite3_bind_text  (s,  6, file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (s,  7, (int64_t)fsz);
    sqlite3_bind_int64 (s,  8, (int64_t)pkts_rx);
    sqlite3_bind_int64 (s,  9, (int64_t)pkts_dropped);
    sqlite3_bind_double(s, 10, mean_latency_ms);
    sqlite3_bind_double(s, 11, p95_latency_ms);

    if (sqlite3_step(s) == SQLITE_DONE)
        std::cout << "[DB] Saved clip #" << clip_index << "\n";
    else
        std::cerr << "[DB] Insert error: " << sqlite3_errmsg(db) << "\n";

    sqlite3_finalize(s);
    sqlite3_close(db);
}