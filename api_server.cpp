#define CPPHTTPLIB_NO_EXCEPTIONS
#include "httplib.h"
#include "sqlite3.h"
#include "db_writer.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

static std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if      (c == '"')  o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else                o += c;
    }
    return o + "\"";
}

static std::string query_to_json(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK)
        return "[]";
    std::string out = "[";
    bool first = true;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) out += ",";
        first = false;
        int ncols = sqlite3_column_count(s);
        out += "{";
        for (int i = 0; i < ncols; i++) {
            if (i) out += ",";
            std::string name = sqlite3_column_name(s, i);
            out += "\"" + name + "\":";
            switch (sqlite3_column_type(s, i)) {
                case SQLITE_INTEGER:
                    out += std::to_string(sqlite3_column_int64(s, i)); break;
                case SQLITE_FLOAT:
                    out += std::to_string(sqlite3_column_double(s, i)); break;
                case SQLITE_NULL:
                    out += "null"; break;
                default: {
                    const char* v = reinterpret_cast<const char*>(
                        sqlite3_column_text(s, i));
                    out += jstr(v ? v : "");
                }
            }
        }
        out += "}";
    }
    sqlite3_finalize(s);
    return out + "]";
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return { std::istreambuf_iterator<char>(f), {} };
}

static void cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin",  "*");
    res.set_header("Access-Control-Allow-Methods", "GET, DELETE, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    int port = (argc >= 2) ? atoi(argv[1]) : 8080;

    sqlite3* db = nullptr;
    if (sqlite3_open(db_path().c_str(), &db) != SQLITE_OK) {
        std::cerr << "[API] Cannot open DB: " << sqlite3_errmsg(db) << "\n";
        return 1;
    }

    sqlite3_exec(db, R"sql(
        CREATE TABLE IF NOT EXISTS recordings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            clip_index INTEGER, session_id TEXT,
            session_start_us INTEGER, clip_start_us INTEGER,
            sample_rate INTEGER, file_path TEXT UNIQUE,
            file_size_bytes INTEGER DEFAULT 0,
            pkts_rx INTEGER DEFAULT 0, pkts_dropped INTEGER DEFAULT 0,
            mean_latency_ms REAL DEFAULT 0, p95_latency_ms REAL DEFAULT 0,
            created_at TEXT DEFAULT (datetime('now'))
        );)sql", nullptr, nullptr, nullptr);

    httplib::Server svr;

    svr.set_pre_routing_handler(
        [](const httplib::Request& req, httplib::Response& res) {
            cors(res);
            if (req.method == "OPTIONS") {
                res.status = 204;
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

    svr.Get("/recordings", [&](const httplib::Request& req, httplib::Response& res) {
        std::string where = "WHERE 1=1";
        if (req.has_param("session"))
            where += " AND session_id='" + req.get_param_value("session") + "'";
        std::string limit  = req.has_param("limit")  ? req.get_param_value("limit")  : "100";
        std::string offset = req.has_param("offset") ? req.get_param_value("offset") : "0";
        res.set_content(query_to_json(db,
            "SELECT id,clip_index,session_id,sample_rate,file_path,"
            "file_size_bytes,pkts_rx,pkts_dropped,mean_latency_ms,"
            "p95_latency_ms,created_at FROM recordings " + where +
            " ORDER BY id DESC LIMIT " + limit + " OFFSET " + offset + ";"),
            "application/json");
    });

    svr.Get(R"(/recordings/(\d+)$)", [&](const httplib::Request& req, httplib::Response& res) {
        std::string arr = query_to_json(db,
            "SELECT id,clip_index,session_id,sample_rate,file_path,"
            "file_size_bytes,pkts_rx,pkts_dropped,mean_latency_ms,"
            "p95_latency_ms,created_at FROM recordings WHERE id="
            + req.matches[1].str() + ";");
        std::string obj = (arr.size() > 2) ? arr.substr(1, arr.size()-2) : "null";
        res.set_content(obj, "application/json");
    });

    svr.Get(R"(/recordings/(\d+)/file)", [&](const httplib::Request& req, httplib::Response& res) {
        sqlite3_stmt* s = nullptr;
        std::string path;
        sqlite3_prepare_v2(db,
            ("SELECT file_path FROM recordings WHERE id=" + req.matches[1].str() + ";").c_str(),
            -1, &s, nullptr);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char* p = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
            if (p) path = p;
        }
        sqlite3_finalize(s);

        if (path.empty() || !std::filesystem::exists(path)) {
            res.status = 404;
            res.set_content("{\"error\":\"file not found\"}", "application/json");
            return;
        }
        std::string fname = std::filesystem::path(path).filename().string();
        res.set_header("Content-Disposition", "attachment; filename=\"" + fname + "\"");
        res.set_header("Accept-Ranges", "bytes");
        res.set_content(read_file(path), "audio/wav");
    });

    svr.Get("/sessions", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(query_to_json(db,
            "SELECT session_id, COUNT(*) AS clip_count, "
            "MIN(created_at) AS started, MAX(created_at) AS last_clip, "
            "SUM(file_size_bytes) AS total_bytes "
            "FROM recordings GROUP BY session_id ORDER BY started DESC;"),
            "application/json");
    });

    svr.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(query_to_json(db,
            "SELECT COUNT(*) AS total_clips, SUM(file_size_bytes) AS total_bytes,"
            "AVG(mean_latency_ms) AS avg_latency_ms, MAX(p95_latency_ms) AS worst_p95_ms "
            "FROM recordings;"),
            "application/json");
    });

    svr.Delete(R"(/recordings/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        sqlite3_stmt* s = nullptr;
        std::string path;
        sqlite3_prepare_v2(db,
            ("SELECT file_path FROM recordings WHERE id=" + req.matches[1].str() + ";").c_str(),
            -1, &s, nullptr);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char* p = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
            if (p) path = p;
        }
        sqlite3_finalize(s);

        sqlite3_stmt* d = nullptr;
        sqlite3_prepare_v2(db,
            ("DELETE FROM recordings WHERE id=" + req.matches[1].str() + ";").c_str(),
            -1, &d, nullptr);
        sqlite3_step(d);
        sqlite3_finalize(d);

        if (!path.empty()) { std::error_code ec; std::filesystem::remove(path, ec); }
        res.set_content("{\"deleted\":true}", "application/json");
    });

    std::thread server_thread([&]() {
    std::cout << "[API] Listening on http://localhost:" << port << "\n";
    std::cout << "[API] DB: " << db_path() << "\n";
    svr.listen("0.0.0.0", port);
});

    // Wait for user input
    std::cout << "Press ENTER to stop the server...\n";
    std::cin.get();

    // Stop server
    std::cout << "[API] Shutting down...\n";
    svr.stop();

    // Wait for thread to finish
    server_thread.join();

    sqlite3_close(db);
    return 0;
}