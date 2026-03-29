# AudioLink v2

A high-fidelity, low-latency audio streaming application written in C++17. AudioLink captures raw microphone audio on one machine (**Node A**) — bypassing OS mixer degradation via WASAPI Exclusive mode on Windows — and streams it over WebSockets to a second machine (**Node B**) for real-time playback and lossless WAV recording.

---

## Table of Contents

1. [How It Works](#how-it-works)
2. [Architecture Overview](#architecture-overview)
3. [Wire Protocol](#wire-protocol)
4. [Latency Budget](#latency-budget)
5. [Prerequisites](#prerequisites)
6. [Building from Source](#building-from-source)
   - [Windows (MinGW / MSYS2 — Recommended)](#windows-mingw--msys2--recommended)
   - [Windows (MSVC + vcpkg)](#windows-msvc--vcpkg)
   - [Linux](#linux)
   - [macOS](#macos)
7. [Running the Application](#running-the-application)
   - [Same Machine (Loopback)](#same-machine-loopback)
   - [Two Machines on a LAN](#two-machines-on-a-lan)
   - [Across the Internet](#across-the-internet)
8. [WAV Recordings](#wav-recordings)
9. [Tuning Latency](#tuning-latency)
10. [Do You Need Docker?](#do-you-need-docker)
11. [Project File Reference](#project-file-reference)
12. [Troubleshooting](#troubleshooting)

---

## How It Works

```
  ┌─────────────────────────────────┐          ┌─────────────────────────────────┐
  │           Node A (sender)       │          │          Node B (receiver)      │
  │                                 │          │                                 │
  │  Microphone                     │          │                                 │
  │      ↓                          │          │                                 │
  │  AudioCapture (miniaudio)       │          │  Receiver (IXWebSocket client)  │
  │  WASAPI Exclusive / 32-bit f32  │ ──ws──▶ │  Adaptive jitter buffer (200pk)  │ 
  │  Native sample rate (≤96 kHz)   │          │  Packet Loss Concealment (PLC)  │
  │      ↓                          │          │      ↓               ↓          │
  │  Transmitter (IXWebSocket srv)  │          │  Playback (miniaudio) WavSaver  │
  │  Port 9001 / binary frames      │          │  Real-time audio out  WAV file  │
  └─────────────────────────────────┘          └─────────────────────────────────┘
```

- **Node A** probes the microphone for its true native sample rate (avoiding Windows resampling), opens the device in WASAPI Exclusive mode, and broadcasts 10 ms PCM packets over WebSocket.
- **Node B** connects to Node A, buffers incoming packets in a jitter buffer, plays audio in real time, and simultaneously writes a lossless 32-bit float WAV file with embedded session metadata.

Both programs stop cleanly when you press **Enter**.

---

## Architecture Overview

### Shared

| File | Role |
|---|---|
| `protocol.h` | `AudioPacket` (44-byte wire header), `AudioQueue` thread-safe ring buffer, `get_current_time_us()`, `timestamp_for_filename()` |

### Node A

| Module | Role |
|---|---|
| `session.cpp/.h` | Generates a random 32-bit session ID and records wall-clock start time; both are stamped into every outgoing packet |
| `audio_capture.cpp/.h` | Probes the capture device's native sample rate; opens WASAPI Exclusive (Windows) or default device; fires `OnFramesCallback` with raw f32 PCM every ~10 ms |
| `transmitter.cpp/.h` | Runs an `IXWebSocketServer` on port 9001; maintains a connected-client list; serialises `AudioPacket` + PCM and broadcasts to all clients |
| `node_A.cpp` | `main()`: wires Session → AudioCapture → Transmitter; waits for Enter; tears down in reverse order |

### Node B

| Module | Role |
|---|---|
| `playback.cpp/.h` | Opens the miniaudio playback device at 48 kHz (miniaudio resamples if Node A uses a different rate); invokes `PullCallback` every ~10 ms |
| `wav_saver.cpp/.h` | Background writer thread (keeps `fwrite` off the audio thread); `open_clip()`, `write_pcm()`, `stop()`, `close_and_embed_metadata()` |
| `receiver.cpp/.h` | IXWebSocket client with auto-reconnect; validates headers; manages adaptive jitter buffer (200-packet capacity); implements PLC (last-frame repeat); tracks rolling 200-sample latency window (mean + P95) |
| `node_B.cpp` | `main()`: wires WavSaver → Playback → Receiver; waits for Enter; tears down: stop playback → drain WavSaver → embed metadata → print summary |

---

## Wire Protocol

Every WebSocket message sent by Node A is a binary frame with the following layout:

```
Offset  Size  Type      Field
──────  ────  ────────  ────────────────────────────────────────────────
 0       4    uint32    magic           = 0x41554431 ("AUD1")
 4       4    uint32    sequence_number   monotonic per-session counter
 8       8    uint64    timestamp_us      sender wall-clock (µs, Unix epoch)
16       8    uint64    session_start_us  wall-clock when Node A started
24       4    uint32    session_id        random 32-bit ID
28       4    uint32    clip_index        increments on each Node B reconnect
32       4    uint32    sample_rate       Hz — authoritative
36       2    uint16    channels          1 = mono
38       2    uint16    bits_per_sample   32
40       2    uint16    payload_bytes     byte length of following PCM data
42       2    uint16    reserved / zero
──────  ────
44 bytes total header, followed immediately by raw 32-bit float PCM samples
```

Node B uses `session_id` + `clip_index` to group recordings, and `timestamp_us` to compute end-to-end latency.

---

## Latency Budget

| Stage | Typical |
|---|---|
| miniaudio capture period | 10 ms |
| WebSocket frame serialisation | < 0.1 ms |
| Network (loopback / LAN) | 0.1 – 1 ms |
| Jitter buffer pre-fill (6 packets) | 60 ms |
| miniaudio playback period | 10 ms |
| **End-to-end** | **~80 ms** |

The jitter buffer dominates. See [Tuning Latency](#tuning-latency) to reduce it.

---

## Prerequisites

### All Platforms

| Tool | Minimum Version | Notes |
|---|---|---|
| C++ compiler | C++17 support | GCC 9+, Clang 9+, or MSVC 2019+ |
| CMake | 3.16 | [cmake.org/download](https://cmake.org/download/) |
| Git | Any recent | Needed to clone the repo and IXWebSocket |
| IXWebSocket | Any recent | See platform sections below |
| miniaudio.h | Latest | Single header — download separately |

### Getting miniaudio

`miniaudio.h` is a single-header library not included in the repo. Download it and place it in the project root before building:

```
https://github.com/mackron/miniaudio/releases
```

Place `miniaudio.h` here:
```
Aksyn challenge/
└── miniaudio.h     ← right here
```

---

## Building from Source

### Windows (MinGW / MSYS2 — Recommended)

This is the path used in development. It uses GCC from MSYS2's UCRT64 environment, which is what VS Code's CMake Tools detects by default if MSYS2 is installed.

**Step 1 — Install MSYS2** (if not already installed)

Download from [msys2.org](https://www.msys2.org/) and run the installer. Open the **MSYS2 UCRT64** terminal.

**Step 2 — Install the toolchain and IXWebSocket**

```bash
pacman -Syu
pacman -S mingw-w64-ucrt-x86_64-gcc \
          mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-ixwebsocket
```

**Step 3 — Clone the repository**

```bash
git clone <your-repo-url> "Aksyn challenge"
cd "Aksyn challenge"
```

**Step 4 — Drop in miniaudio.h**

Download `miniaudio.h` from the link above and copy it into the project root.

**Step 5 — Configure CMake in VS Code**

Add this to your `.vscode/settings.json`:

```json
{
  "cmake.configureArgs": [
    "-DCMAKE_PREFIX_PATH=C:/msys64/ucrt64"
  ]
}
```

Then open VS Code in the project folder and run **CMake: Configure**, then **CMake: Build**.

**Step 6 — Build from terminal (alternative)**

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
```

---

### Windows (MSVC + vcpkg)

Use this path if you prefer the MSVC compiler (Visual Studio 2022).

**Step 1 — Install vcpkg**

```bat
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

**Step 2 — Install IXWebSocket**

```bat
C:\vcpkg\vcpkg install ixwebsocket:x64-windows
```

**Step 3 — Clone the repo and drop in miniaudio.h** (same as above)

**Step 4 — Configure and build**

```bat
cmake -B build -S . ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows

cmake --build build --config Release
```

Executables will be at:
```
build\bin\Release\node_A.exe
build\bin\Release\node_B.exe
```

---

### Linux

**Step 1 — Install dependencies**

Ubuntu / Debian:
```bash
sudo apt update
sudo apt install build-essential cmake git libixwebsocket-dev
```

Arch Linux:
```bash
sudo pacman -S base-devel cmake git ixwebsocket
```

Fedora:
```bash
sudo dnf install gcc-c++ cmake git
# IXWebSocket may need to be built from source — see below
```

If your distro does not package IXWebSocket:
```bash
git clone https://github.com/machinezone/IXWebSocket third_party/IXWebSocket
# CMakeLists.txt will find it automatically at this path
```

**Step 2 — Clone the repo and drop in miniaudio.h**

```bash
git clone <your-repo-url> aksyn-challenge
cd aksyn-challenge
# copy miniaudio.h here
```

**Step 3 — Build**

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Executables:
```
build/bin/node_A
build/bin/node_B
```

> **Note:** On Linux, miniaudio uses ALSA by default. CMakeLists.txt detects `libasound` automatically. If you want PulseAudio or PipeWire, you may need to edit the CMakeLists.txt `PLATFORM_LIBS` section.

---

### macOS

**Step 1 — Install dependencies**

```bash
xcode-select --install
brew install cmake ixwebsocket
```

**Step 2 — Clone the repo and drop in miniaudio.h**

**Step 3 — Build**

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
```

---

## Running the Application

### Same Machine (Loopback)

Open two terminal windows in the project folder (or `build/bin/`).

**Terminal 1 — start Node A first:**
```bash
./node_A.exe          # Windows
./node_A              # Linux / macOS
```

Node A will print the detected audio device, sample rate, and confirm it is listening on port 9001.

**Terminal 2 — start Node B:**
```bash
./node_B.exe          # Windows
./node_B              # Linux / macOS
```

Node B will connect, print a live stats table (clip, sequence, jitter buffer depth, latency), and begin writing a WAV file.

Press **Enter** in either terminal to stop that node cleanly.

---

### Two Machines on a LAN

Both machines must have the executables built (see above). No additional software is needed.

**Machine A (sender):**
```bash
# Find your local IP first
ipconfig          # Windows
ip addr           # Linux

./node_A.exe 0.0.0.0     # bind to all interfaces so Machine B can connect
```

> Binding to `0.0.0.0` makes Node A accept connections from the network, not just localhost.

**Machine B (receiver):**
```bash
./node_B.exe 192.168.1.42    # replace with Machine A's actual LAN IP
```

**Firewall:** Make sure port **9001 TCP** is open on Machine A. On Windows:
```bat
netsh advfirewall firewall add rule name="AudioLink NodeA" dir=in action=allow protocol=TCP localport=9001
```

On Linux:
```bash
sudo ufw allow 9001/tcp
```

---

### Across the Internet

Node A needs to be reachable from Node B. Options:

**Option 1 — Port forward on your router**

Forward port 9001 TCP from your router to Machine A's LAN IP. Node B connects to your public IP.

**Option 2 — Tailscale (easiest, no port forwarding)**

Install [Tailscale](https://tailscale.com) on both machines. Each gets a stable private IP (e.g. `100.x.x.x`). Run Node A normally; Node B connects to the Tailscale IP.

```bash
./node_A.exe 0.0.0.0
./node_B.exe 100.x.x.x     # Tailscale IP of Machine A
```

**Option 3 — SSH tunnel**

If you have SSH access to a server or to Machine A:
```bash
# On Machine B, tunnel port 9001 from Machine A through SSH
ssh -L 9001:localhost:9001 user@machine-a-ip -N &
./node_B.exe 127.0.0.1
```

---

## WAV Recordings

Node B saves recordings to a `recordings/` folder next to the executable.

**Filename format:**
```
clip_0001_2025-01-15T10-30-00Z_sid1A2B3C4D_48k_f32_mono.wav
```

Each file is 32-bit float mono WAV. After the session ends, Node B embeds a RIFF `LIST/INFO` chunk with these fields:

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

These fields are readable in **MediaInfo**, **Audacity** (via File → Import → Raw Data metadata view), or any RIFF-aware tool.

---

## Tuning Latency

Edit `receiver.h` and adjust these three constants, then rebuild:

```cpp
static constexpr int TARGET_FILL_PACKETS = 6;   // 60 ms pre-fill before playback starts
static constexpr int LOW_WATER_MARK      = 3;   // 30 ms — re-arms pre-fill if buffer drops here
static constexpr int HIGH_WATER_MARK     = 180; // drops oldest packet if buffer exceeds this
```

| Goal | Change |
|---|---|
| Lower latency (more dropout risk) | Reduce `TARGET_FILL_PACKETS` and `LOW_WATER_MARK` |
| More resilience to network jitter | Increase `TARGET_FILL_PACKETS` |
| Cap memory use | Reduce `HIGH_WATER_MARK` |

A setting of `TARGET_FILL_PACKETS = 2` / `LOW_WATER_MARK = 1` gives ~20 ms pre-fill — useful on a loopback or a very stable LAN.

---

## Project File Reference

```
Aksyn challenge/
│
├── CMakeLists.txt          Single CMake build file for both executables
├── protocol.h              Shared wire format: AudioPacket, AudioQueue, helpers
├── miniaudio.h             ← Download and place here (not in repo)
│
├── node_A.cpp              main() for the sender: Session → AudioCapture → Transmitter
├── session.h / .cpp        Random session ID + wall-clock start time
├── audio_capture.h / .cpp  WASAPI probe, device open/start/stop, MINIAUDIO_IMPLEMENTATION
├── transmitter.h / .cpp    IXWebSocket server, client list, packet broadcast
│
├── node_B.cpp              main() for the receiver: WavSaver → Playback → Receiver
├── playback.h / .cpp       miniaudio playback device, MINIAUDIO_IMPLEMENTATION
├── wav_saver.h / .cpp      Background writer thread, ma_encoder, INFO chunk
└── receiver.h / .cpp       WebSocket client, jitter buffer, PLC, latency stats
```
---

## Troubleshooting

**CMake cannot find IXWebSocket**

On MSYS2: run `pacman -S mingw-w64-ucrt-x86_64-ixwebsocket` and add `-DCMAKE_PREFIX_PATH=C:/msys64/ucrt64` to your CMake configure args.

On Linux: run `sudo apt install libixwebsocket-dev` or clone into `third_party/IXWebSocket` — CMakeLists.txt checks that path automatically.

---

**CMake cannot find miniaudio.h**

Make sure `miniaudio.h` is in the project root (same folder as `CMakeLists.txt`). The build will print a clear error with the expected path if it is missing.

---

**Node A: "Failed to start audio capture"**

- Another application may have the microphone open in exclusive mode. Close other audio software (DAWs, Discord, etc.) and try again.
- Try running as Administrator on Windows — WASAPI Exclusive sometimes requires elevated privileges.
- Node A will automatically fall back to WASAPI Shared mode and log a warning if Exclusive mode fails.

---

**Node B connects but no audio plays / WAV file is silent**

- Check that Node A is actually capturing — it prints `[CAPTURE] Active: <rate> Hz` on success.
- The jitter buffer requires 6 packets (~60 ms) before playback starts. If Node A stops quickly, not enough packets may have arrived.
- Verify the playback device is not muted in Windows Volume Mixer.

---

**Node B: "Failed to start playback device"**

Another application may have the audio output in exclusive mode. Close other audio apps. Node B uses WASAPI Shared mode for playback, so this is uncommon but possible.

---

**High packet loss or underruns on LAN**

Increase `TARGET_FILL_PACKETS` in `receiver.h` to give the jitter buffer more headroom. A value of 10–15 (100–150 ms) is typically enough to absorb LAN jitter completely.

---

**Port 9001 refused / Node B cannot connect**

- Confirm Node A is running and printed `[TRANSMITTER] Listening on ...`
- Check the firewall on Machine A allows inbound TCP on port 9001.
- Confirm Node B is using the correct IP address for Machine A.
- If testing across the internet without a VPN, ensure port 9001 is forwarded on Machine A's router.

## 🔗 Project Links
* **Team Head Google Drive:** [https://drive.google.com/file/d/1E6YTVGmwlKECK_-p275U_oivKa62uYBn/view?usp=drive_link]
* **Client Google Drive:** [https://drive.google.com/file/d/1Uk_PyAYJCmhTXOVQ_Kwpr_Q53wCz46Ee/view?usp=drive_link]

---