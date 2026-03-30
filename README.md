# Aksyn AudioLink v2

A high-fidelity, low-latency audio streaming system written in C++17 with a Flutter monitoring frontend. AudioLink captures raw microphone audio on one machine (**Node A**), streams it over WebSockets to a second machine (**Node B**) for real-time playback and lossless WAV recording, and exposes a REST API so a Flutter mobile app can monitor live metrics and browse/play recordings remotely.

---

## Table of Contents

1. [System Overview](#system-overview)
2. [Architecture](#architecture)
3. [Wire Protocol](#wire-protocol)
4. [Repository Structure](#repository-structure)
5. [Prerequisites](#prerequisites)
6. [Building from Source — Windows (MinGW/MSYS2)](#building-from-source--windows-mingwmsys2-recommended)
7. [Building from Source — Windows (MSVC + vcpkg)](#building-from-source--windows-msvc--vcpkg)
8. [Building from Source — Linux](#building-from-source--linux)
9. [Building from Source — macOS](#building-from-source--macos)
10. [Running on the Same Machine (Loopback)](#running-on-the-same-machine-loopback)
11. [Running Across a LAN (Real Device)](#running-across-a-lan-real-device)
12. [Running Across the Internet](#running-across-the-internet)
13. [Flutter App Setup](#flutter-app-setup)
14. [API Server Endpoints](#api-server-endpoints)
15. [WAV Recordings](#wav-recordings)
16. [Latency Tuning](#latency-tuning)
17. [Firewall Setup](#firewall-setup)
18. [Troubleshooting](#troubleshooting)

---

## System Overview

```
┌──────────────────────────────────┐        ┌──────────────────────────────────────┐
│         Node A  (sender PC)      │        │        Node B  (receiver PC)         │
│                                  │        │                                      │
│  Microphone                      │        │  Receiver (IXWebSocket client)       │
│      ↓                           │        │  Adaptive jitter buffer (200 pkts)   │
│  AudioCapture (miniaudio)        │ws:9001▶│  Packet Loss Concealment (PLC)       │ 
│  WASAPI Exclusive / 32-bit f32   │        │      ↓                  ↓            │
│  Native sample rate (≤96 kHz)    │        │  Playback (miniaudio)  WavSaver      │
│      ↓                           │        │  Real-time audio out   WAV file      │
│  Transmitter (IXWebSocket srv)   │        │      ↓                               │
│  Port 9001 / binary frames       │        │  db_writer → recordings.db           │
│                                  │        │      ↓                               │
│                                  │        │  api_server (http-cpp)               │
│                                  │        │  Port 8080 / REST JSON               │
└──────────────────────────────────┘        └──────────────────────────────────────┘
                                                         ↑
                                            ┌────────────────────────┐
                                            │  Flutter App (Android) │
                                            │  Monitor tab: WS:9001  │
                                            │  Recordings tab: :8080 │
                                            └────────────────────────┘
```

- **Node A** probes the microphone's true native sample rate, opens it in WASAPI Exclusive mode (bypassing the Windows mixer), and broadcasts 10 ms PCM frames over WebSocket on port **9001**.
- **Node B** connects to Node A, buffers packets in an adaptive jitter buffer, plays audio in real time via miniaudio, and simultaneously writes a lossless 32-bit float WAV file. On disconnect it embeds session statistics into the WAV as a RIFF INFO chunk and writes the clip record to **SQLite**.
- **api_server** serves the SQLite database over HTTP on port **8080**, letting the Flutter app list, stream, and delete recordings.
- **Flutter app** has two tabs: a live **Monitor** tab (WebSocket to Node A port 9001) and a **Recordings** tab (HTTP to api_server port 8080).

---

## Architecture

### Shared

| File | Role |
|---|---|
| `protocol.h` | `AudioPacket` 44-byte wire header, `AudioQueue` thread-safe ring buffer, `get_current_time_us()`, `timestamp_for_filename()` |

### Node A

| Module | Role |
|---|---|
| `session.cpp/.h` | Generates a random 32-bit session ID and records wall-clock start time stamped into every packet |
| `audio_capture.cpp/.h` | Probes native sample rate; opens WASAPI Exclusive on Windows; fires `OnFramesCallback` with raw f32 PCM every ~10 ms; optional RNNoise denoising |
| `transmitter.cpp/.h` | `IXWebSocketServer` on port 9001; maintains connected-client list; serialises `AudioPacket` + PCM and broadcasts to all clients; replies to PING with PONG for RTT probing |
| `node_A.cpp` | `main()`: wires Session → AudioCapture → Transmitter; binds to `0.0.0.0` by default so both local and LAN clients can connect |

### Node B

| Module | Role |
|---|---|
| `playback.cpp/.h` | miniaudio playback device; invokes `PullCallback` every ~10 ms from the audio thread |
| `wav_saver.cpp/.h` | Background writer thread (keeps disk I/O off the audio thread); `open_clip()`, `write_pcm()`, `close_and_embed_metadata()` |
| `receiver.cpp/.h` | IXWebSocket client with auto-reconnect; header validation; adaptive jitter buffer; PLC (last-frame repeat on underrun); rolling 200-sample latency window (mean + P95) |
| `db_writer.cpp/.h` | Writes one SQLite row per clip with all session statistics |
| `api_server.cpp` | cpp-httplib HTTP server on port 8080; serves recordings JSON, streams WAV files, handles DELETE; binds to `0.0.0.0` for LAN access |
| `node_B.cpp` | `main()`: wires WavSaver → Playback → Receiver; teardown: stop playback → drain WavSaver → embed metadata → print summary |

### Flutter App (`frontend/aksyn/`)

| File | Role |
|---|---|
| `lib/main.dart` | `MultiProvider` setup + `_AppShell` with bottom navigation (Monitor / Recordings tabs) |
| `lib/struct/services/audio_stream_service.dart` | WebSocket client to Node A port 9001; PING/PONG clock-offset calibration; live latency + packet loss metrics |
| `lib/struct/services/recordings_service.dart` | HTTP client to api_server port 8080; fetches recordings list + stats; streams WAV for playback |
| `lib/struct/screens/dashboard_screen.dart` | Live monitor: latency chart, packet statistics, connection panel |
| `lib/struct/screens/recordings_screen.dart` | Recordings browser: per-clip playback with scrubber, delete, stats grid |

---

## Wire Protocol

Every WebSocket binary frame from Node A has this layout:

```
Offset  Size  Type     Field
──────  ────  ───────  ──────────────────────────────────────────
 0       4    uint32   magic           = 0x41554431 ("AUD1")
 4       4    uint32   sequence_number   monotonic per-session
 8       8    uint64   timestamp_us      sender wall-clock (µs)
16       8    uint64   session_start_us  wall-clock Node A started
24       4    uint32   session_id        random 32-bit ID
28       4    uint32   clip_index        increments on reconnect
32       4    uint32   sample_rate       Hz — authoritative
36       2    uint16   channels          1 = mono
38       2    uint16   bits_per_sample   32
40       2    uint16   payload_bytes     byte length of PCM data
42       2    uint16   reserved / zero
──────  ────
44 bytes header, followed by raw 32-bit float PCM samples
```

Node B uses `session_id` + `clip_index` to group recordings. `timestamp_us` drives end-to-end latency calculations. Text frames starting with `PING:` are echoed as `PONG:` for RTT probing by the Flutter app.

---

## Repository Structure

```
Aksyn challenge/
│
├── CMakeLists.txt            Single CMake build file — all three targets
├── protocol.h                Shared wire format and ring buffer
├── miniaudio.h               ← Download separately (see Prerequisites)
├── httplib.h                 cpp-httplib single header (included in repo)
├── sqlite3.c / sqlite3.h     SQLite amalgamation (included in repo)
│
├── node_A.cpp                Sender main()
├── session.h / session.cpp   Session ID + timestamps
├── audio_capture.h / .cpp    WASAPI capture, RNNoise optional denoising
├── transmitter.h / .cpp      IXWebSocket server, packet broadcast
│
├── node_B.cpp                Receiver main()
├── playback.h / .cpp         miniaudio playback device
├── wav_saver.h / .cpp        Background WAV writer + RIFF INFO metadata
├── receiver.h / .cpp         WebSocket client, jitter buffer, PLC, stats
├── db_writer.h / .cpp        SQLite recording metadata writer
├── api_server.cpp            HTTP REST API for recordings
│
├── third_party/
│   └── IXWebSocket/          Clone here if not using MSYS2/vcpkg
│
└── frontend/aksyn/           Flutter application
    └── lib/
        ├── main.dart
        ├── struct/
        │   ├── screens/
        │   │   ├── dashboard_screen.dart
        │   │   └── recordings_screen.dart
        │   ├── services/
        │   │   ├── audio_stream_service.dart
        │   │   └── recordings_service.dart
        │   └── widgets/
        │       ├── connection_indicator.dart
        │       ├── latency_chart.dart
        │       └── metric_card.dart
        └── models/
            ├── audio_packet.dart
            └── stream_metrics.dart
```

---

## Prerequisites

### C++ Backend

| Tool | Version | Notes |
|---|---|---|
| C++ compiler | GCC 9+ / Clang 9+ / MSVC 2019+ | C++17 required |
| CMake | 3.16+ | [cmake.org](https://cmake.org/download/) |
| IXWebSocket | Any recent | Install via MSYS2, vcpkg, or clone to `third_party/` |
| miniaudio.h | Latest | **Not in repo** — download separately (see below) |

### Getting miniaudio.h

This is a single-header library not committed to the repo. Download it and place it in the project root:

```
https://github.com/mackron/miniaudio/releases
```

Place it here:
```
Aksyn challenge/
└── miniaudio.h     ← right here, same folder as CMakeLists.txt
```

### Flutter App

| Tool | Version |
|---|---|
| Flutter SDK | 3.x+ |
| Dart | 3.x+ |
| Android SDK | API 21+ |

Flutter dependencies (`pubspec.yaml`):
```yaml
dependencies:
  flutter:
    sdk: flutter
  provider: ^6.1.0
  web_socket_channel: ^3.0.0
  http: ^1.2.0
  audioplayers: ^6.0.0
  fl_chart: ^0.69.0
  google_fonts: ^6.0.0
```

---

## Building from Source — Windows (MinGW/MSYS2, Recommended)

This is the path used in development. All three executables (`node_A.exe`, `node_B.exe`, `api_server.exe`) are built together.

### Step 1 — Install MSYS2

Download from [msys2.org](https://www.msys2.org/). Open the **MSYS2 UCRT64** terminal after install.

### Step 2 — Install the toolchain and IXWebSocket

```bash
pacman -Syu
pacman -S mingw-w64-ucrt-x86_64-gcc \
          mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-ixwebsocket
```

### Step 3 — Clone the repository

```bash
git clone https://github.com/Confusedaff/Aksyn-challenge "Aksyn challenge"
cd "Aksyn challenge"
```

### Step 4 — Download and place miniaudio.h

Download `miniaudio.h` from [github.com/mackron/miniaudio/releases](https://github.com/mackron/miniaudio/releases) and copy it into the project root.

### Step 5 — Configure VS Code (if using VS Code)

Create or update `.vscode/settings.json`:

```json
{
  "cmake.generator": "MinGW Makefiles",
  "cmake.cmakePath": "C:/msys64/ucrt64/bin/cmake.exe",
  "cmake.configureArgs": [
    "-DCMAKE_PREFIX_PATH=C:/msys64/ucrt64"
  ]
}
```

Create `.vscode/cmake-kits.json` to lock the toolchain:

```json
[
  {
    "name": "MinGW UCRT64 (MSYS2)",
    "compilers": {
      "C":   "C:/msys64/ucrt64/bin/gcc.exe",
      "CXX": "C:/msys64/ucrt64/bin/g++.exe"
    },
    "preferredGenerator": {
      "name": "MinGW Makefiles"
    },
    "environmentVariables": {
      "PATH": "C:/msys64/ucrt64/bin;${env:PATH}"
    }
  }
]
```

Then in VS Code: **Ctrl+Shift+P** → `CMake: Select a Kit` → pick **MinGW UCRT64 (MSYS2)**, then `CMake: Configure`, then `CMake: Build All`.

### Step 6 — Build from terminal (alternative)

```bash
cmake -B build -S . \
  -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64

cmake --build build -j$(nproc)
```

Executables will be at:
```
build/bin/node_A.exe
build/bin/node_B.exe
build/bin/api_server.exe
```

---

## Building from Source — Windows (MSVC + vcpkg)

### Step 1 — Install vcpkg

```bat
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

### Step 2 — Install IXWebSocket

```bat
C:\vcpkg\vcpkg install ixwebsocket:x64-windows
```

### Step 3 — Clone the repo and place miniaudio.h (same as above)

### Step 4 — Configure and build

```bat
cmake -B build -S . ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows

cmake --build build --config Release
```

---

## Building from Source — Linux

### Step 1 — Install dependencies

Ubuntu / Debian:
```bash
sudo apt update
sudo apt install build-essential cmake git libixwebsocket-dev libasound2-dev
```

Arch:
```bash
sudo pacman -S base-devel cmake git ixwebsocket alsa-lib
```

If IXWebSocket is not packaged for your distro:
```bash
git clone https://github.com/machinezone/IXWebSocket third_party/IXWebSocket
# CMakeLists.txt detects this path automatically
```

### Step 2 — Clone the repo and place miniaudio.h

```bash
git clone https://github.com/Confusedaff/Aksyn-challenge aksyn-challenge
cd aksyn-challenge
# copy miniaudio.h here
```

### Step 3 — Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Executables:
```
build/bin/node_A
build/bin/node_B
build/bin/api_server
```

---

## Building from Source — macOS

```bash
xcode-select --install
brew install cmake ixwebsocket

git clone https://github.com/Confusedaff/Aksyn-challenge aksyn-challenge
cd aksyn-challenge
# copy miniaudio.h here

cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
```

---

## Running on the Same Machine (Loopback)

All three processes must run from the **same directory** so they share `recordings.db`.

Open three terminal windows inside `build/bin/`:

**Terminal 1 — Node A:**
```bat
cd "C:\path\to\Aksyn challenge\build\bin"
.\node_A.exe
```
Expected output:
```
[NODE A] Session ID    : 0x...
[TRANSMITTER] Listening on 0.0.0.0:9001
[CAPTURE] Active  : 48000 Hz / 32-bit float / mono / 480 frames/period
[NODE A] Streaming on 0.0.0.0:9001
[NODE A] Press ENTER to quit.
```

**Terminal 2 — Node B:**
```bat
cd "C:\path\to\Aksyn challenge\build\bin"
.\node_B.exe
```
Node B will connect, print a live stats table, and start writing a WAV file.

**Terminal 3 — API Server:**
```bat
cd "C:\path\to\Aksyn challenge\build\bin"
.\api_server.exe 8080
```
Expected output:
```
[API] Listening on http://localhost:8080
[API] DB: C:\...\build\bin\recordings.db
```

Press **Enter** in any terminal to stop that process cleanly.

---

## Running Across a LAN (Real Device)

Both machines must be on the **same WiFi network**.

### Step 1 — Find your PC's LAN IP

```bat
ipconfig
```
Look for **IPv4 Address** under your WiFi adapter — e.g. `192.168.1.42`.

### Step 2 — Start Node A bound to all interfaces

```bat
.\node_A.exe 0.0.0.0
```

> **Critical:** Without `0.0.0.0`, Node A only accepts connections from the same machine. Real devices on the LAN cannot connect.

### Step 3 — Start Node B and API Server

```bat
.\node_B.exe
.\api_server.exe 8080
```

Both bind to `0.0.0.0` by default.

### Step 4 — Open the firewall

```bat
netsh advfirewall firewall add rule name="Aksyn NodeA WS"  dir=in action=allow protocol=TCP localport=9001
netsh advfirewall firewall add rule name="Aksyn API"       dir=in action=allow protocol=TCP localport=8080
```

### Step 5 — Configure the Flutter app

In the **Monitor** tab, set host to `192.168.1.42` (your PC's IP), port `9001`.
In the **Recordings** tab, set API Host to `192.168.1.42`, port `8080`, tap **LOAD**.

### Verify connectivity

Run on your PC and confirm both ports show `0.0.0.0:9001` and `0.0.0.0:8080` — not `127.0.0.1`:

```bat
netstat -an | findstr "9001\|8080"
```

---

## Running Across the Internet

### Option 1 — Tailscale (Easiest, No Port Forwarding)

Install [Tailscale](https://tailscale.com) on both machines. Each gets a stable `100.x.x.x` private IP.

```bat
# Machine A
.\node_A.exe 0.0.0.0

# Machine B (or Flutter app) — use Tailscale IP of Machine A
.\node_B.exe 100.x.x.x
# Flutter: set host to 100.x.x.x
```

### Option 2 — Port Forwarding

Forward port **9001 TCP** and **8080 TCP** on your router to Machine A's LAN IP. Node B and the Flutter app connect to your public IP.

### Option 3 — SSH Tunnel

```bash
# On Machine B, tunnel through SSH
ssh -L 9001:localhost:9001 -L 8080:localhost:8080 user@machine-a-ip -N &
.\node_B.exe 127.0.0.1
# Flutter: set host to 127.0.0.1
```

---

## Flutter App Setup

### Step 1 — Install dependencies

```bash
cd frontend/aksyn
flutter pub get
```

### Step 2 — Set default IP addresses

In `lib/struct/services/audio_stream_service.dart`:
```dart
String host = '192.168.1.42';  // your PC's LAN IP
int    port = 9001;
```

In `lib/struct/screens/dashboard_screen.dart`:
```dart
final _hostController = TextEditingController(text: '192.168.1.42');
```

In `lib/struct/screens/recordings_screen.dart`:
```dart
final _hostCtrl = TextEditingController(text: '192.168.1.42');
final _portCtrl = TextEditingController(text: '8080');
```

### Step 3 — Run

```bash
flutter run
```

Or build a release APK:
```bash
flutter build apk --release
```

### Monitor Tab

Connects to Node A's WebSocket on port **9001**. Shows live latency, P95, jitter, packet loss, and a rolling latency chart. The connection uses PING/PONG to calibrate clock offset between the phone and PC, correcting for unsynchronised clocks.

### Recordings Tab

Connects to the API server on port **8080**. Lists all saved clips with session ID, sample rate, file size, mean latency, and packet loss. Tap the play button on any clip to stream and play it directly from the server. Tap the delete icon to remove the clip and its WAV file.

---

## API Server Endpoints

Base URL: `http://<host>:8080`

| Method | Path | Description |
|---|---|---|
| GET | `/recordings` | List all recordings. Supports `?session=`, `?limit=`, `?offset=` |
| GET | `/recordings/:id` | Get a single recording by ID |
| GET | `/recordings/:id/file` | Stream the WAV file as `audio/wav` |
| GET | `/sessions` | List all sessions with clip count and total bytes |
| GET | `/stats` | Aggregate stats: total clips, storage, avg latency, worst P95 |
| DELETE | `/recordings/:id` | Delete recording row and WAV file |

All responses are JSON. CORS headers are set on all responses so the Flutter app can access from any origin.

---

## WAV Recordings

Node B saves recordings to a `recordings/` folder next to the executable.

**Filename format:**
```
clip_0001_2026-03-29T11-55-00Z_sidBB69083F_48k_f32_mono.wav
```

Each file is **32-bit float mono WAV** at the microphone's native sample rate. After each session, Node B embeds a RIFF `LIST/INFO` chunk:

| Tag | Contents |
|---|---|
| `SESS` | Session ID (hex) |
| `SIDX` | Clip index |
| `SRTE` | Sample rate (Hz) |
| `RXPK` | Packets received |
| `DRPK` | Packets dropped |
| `MLAT` | Mean latency (ms) |
| `P95L` | P95 latency (ms) |
| `SRCA` | Source WebSocket URL |
| `INAM` | Human-readable clip name |
| `ICRD` | ISO-8601 clip start time |

These fields are readable in MediaInfo, Audacity, or any RIFF-aware tool.

---

## Latency Tuning

Edit `receiver.h` and rebuild:

```cpp
static constexpr int TARGET_FILL_PACKETS = 6;    // packets before playback starts (~60 ms)
static constexpr int LOW_WATER_MARK      = 3;    // re-arm threshold (~30 ms)
static constexpr int HIGH_WATER_MARK     = 180;  // drop oldest above this
```

| Goal | Change |
|---|---|
| Minimum latency (loopback / stable LAN) | Set `TARGET_FILL_PACKETS=2`, `LOW_WATER_MARK=1` (~20 ms) |
| Tolerate WiFi jitter | Increase `TARGET_FILL_PACKETS` to 10–15 |
| Reduce memory use | Lower `HIGH_WATER_MARK` |

**Typical end-to-end budget:**

| Stage | Time |
|---|---|
| Capture period | 10 ms |
| Serialisation | < 0.1 ms |
| Network (LAN) | 0.1–2 ms |
| Jitter buffer (6 packets) | 60 ms |
| Playback period | 10 ms |
| **Total** | **~80 ms** |

---

## Firewall Setup

### Windows — open both ports

```bat
netsh advfirewall firewall add rule name="Aksyn NodeA WS"  dir=in action=allow protocol=TCP localport=9001
netsh advfirewall firewall add rule name="Aksyn NodeA OUT" dir=out action=allow protocol=TCP localport=9001
netsh advfirewall firewall add rule name="Aksyn API"       dir=in action=allow protocol=TCP localport=8080
netsh advfirewall firewall add rule name="Aksyn API OUT"   dir=out action=allow protocol=TCP localport=8080
```

### Linux

```bash
sudo ufw allow 9001/tcp
sudo ufw allow 8080/tcp
```

### Verify both ports are bound to 0.0.0.0 (not 127.0.0.1)

```bat
netstat -an | findstr "9001\|8080"
```

Both must show `0.0.0.0:9001` and `0.0.0.0:8080` in the LISTENING state. If either shows `127.0.0.1`, the old binary is still running — kill it and re-run the freshly built exe.

---

## Troubleshooting

### "CMake_C_COMPILE_OBJECT not set" / build fails with sqlite3

The Visual Studio CMake binary is running but trying to compile with MinGW GCC. These two toolchains are incompatible. Fix: add `LANGUAGES C CXX` to the `project()` line in CMakeLists.txt, delete the `build/` folder, select the MinGW kit in VS Code, and reconfigure.

### Node A prints "Listening on 127.0.0.1:9001" even after changing the source

The old binary is still running. Kill it first:
```bat
taskkill /f /im node_A.exe
```
Delete the stale object file to force recompile:
```bat
del "build\CMakeFiles\node_A.dir\node_A.cpp.obj"
```
Rebuild, then verify with `netstat -an | findstr 9001` — must show `0.0.0.0`.

### Flutter Monitor tab: "Connection timed out — is Node A running?"

The phone cannot reach Node A. Check in order:

1. Both devices on the same WiFi network.
2. Node A is running and `netstat` shows `0.0.0.0:9001 LISTENING`.
3. Windows Firewall allows inbound TCP on port 9001 (see Firewall Setup).
4. The host field in the Monitor tab shows the PC's LAN IP (from `ipconfig`), not `127.0.0.1`.

### Flutter Recordings tab works but Monitor tab does not

The Recordings tab uses HTTP port 8080 (which was opened in the firewall). The Monitor tab uses WebSocket port 9001 which may still be blocked. Add the firewall rule for port 9001 separately.

### Node A: "Failed to start audio capture"

Another application has the microphone in exclusive mode (Discord, DAWs, Teams). Close other audio apps. Node A will automatically fall back to WASAPI Shared mode and log a warning.

### Node B: WAV file is silent / very short

The jitter buffer requires 6 packets (~60 ms) to pre-fill before playback and recording begin. If Node A stops immediately after starting, not enough packets accumulate. Keep Node A running for at least a few seconds.

### High packet loss on WiFi

Increase `TARGET_FILL_PACKETS` in `receiver.h` to 10–15 and rebuild. The extra buffer absorbs WiFi retransmission bursts. On a wired LAN the default value of 6 is appropriate.

### api_server shows empty recordings (`[]`)

Node B and api_server are running from different directories and writing separate `recordings.db` files. Always run all three executables from the same folder:
```bat
cd "C:\...\build\bin"
.\node_A.exe
.\node_B.exe
.\api_server.exe 8080
```

### CMake cannot find IXWebSocket

On MSYS2: `pacman -S mingw-w64-ucrt-x86_64-ixwebsocket` and add `-DCMAKE_PREFIX_PATH=C:/msys64/ucrt64`.
On Linux: `sudo apt install libixwebsocket-dev`.
Without a package manager: `git clone https://github.com/machinezone/IXWebSocket third_party/IXWebSocket` — CMakeLists.txt checks this path automatically.

### CMake cannot find miniaudio.h

Place `miniaudio.h` in the project root (same folder as `CMakeLists.txt`). The build will print the expected path if it is missing.
