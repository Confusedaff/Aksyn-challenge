# Aksyn Monitor — Flutter Frontend

Real-time audio stream monitor for the Aksyn Challenge C++ backend.

## What it does

- Connects to Node A's WebSocket server (`ws://host:9001`)
- Receives binary audio packets and parses the `AudioPacket` header
- Computes and displays:
  - **One-way latency** (per packet, ms)
  - **Mean latency** (rolling 100-packet window)
  - **P95 latency**
  - **RFC 3550 jitter**
  - **Packets received / dropped**
  - **Packet loss rate (%)**
- Live latency history chart (last 60 packets)
- Health status banner (green if all metrics within target thresholds)

## Prerequisites

- Flutter SDK 3.x
- Dart SDK 3.x
- Node A running on accessible host:port

## Run

```bash
cd aksyn_monitor
flutter pub get
flutter run                          # Android/iOS device or emulator
flutter run -d windows               # Windows desktop
flutter run -d macos                 # macOS desktop
flutter run -d chrome                # Web (limited WebSocket support)
```

## Connecting to Node A

1. Launch the app
2. Enter the IP address of the machine running Node A (use `127.0.0.1` for localhost)
3. Enter port `9001` (or whatever port Node A is bound to)
4. Tap **CONNECT**

On Android connecting to a desktop on the same LAN: use the desktop's LAN IP
(e.g. `192.168.1.x`), not `127.0.0.1`.

## Packet header layout

The app expects this struct layout (matches `protocol.h`):

```
Offset  Size  Field
0       4     sequence_number  (uint32, little-endian)
4       8     timestamp_us     (int64,  little-endian)
12      4     sample_rate      (uint32, little-endian)
16      2     payload_bytes    (uint16, little-endian)
18+     N     PCM payload
```

If your `protocol.h` has a different layout, update `lib/models/audio_packet.dart`.

## Project structure

```
lib/
  main.dart                        # App entry point
  models/
    audio_packet.dart              # Binary packet parser
    stream_metrics.dart            # Metrics data class
  services/
    audio_stream_service.dart      # WebSocket + metrics computation
  widgets/
    latency_chart.dart             # fl_chart line chart
    metric_card.dart               # Individual metric display
    connection_indicator.dart      # Animated status dot
  screens/
    dashboard_screen.dart          # Main UI
```
