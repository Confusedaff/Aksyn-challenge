import 'dart:async';
import 'package:aksyn_monitor/models/audio_packet.dart';
import 'package:aksyn_monitor/models/stream_metrics.dart';
import 'package:flutter/foundation.dart';
import 'package:web_socket_channel/web_socket_channel.dart';

enum StreamConnectionState { disconnected, connecting, connected, error }

class AudioStreamService extends ChangeNotifier {
  // ── Config ──────────────────────────────────────────────────────────────────
  String host = '127.0.0.1';
  int    port = 9001;

  // ── Connection state ────────────────────────────────────────────────────────
  StreamConnectionState connectionState = StreamConnectionState.disconnected;
  String errorMessage = '';
  bool _handshakeConfirmed = false;

  // ── Metrics ─────────────────────────────────────────────────────────────────
  StreamMetrics metrics = StreamMetrics.zero;

  // ── Latency history for chart (last 60 data points) ─────────────────────────
  final List<double> latencyHistory = [];
  static const int _historyMax = 60;

  // ── Internal state ───────────────────────────────────────────────────────────
  WebSocketChannel? _channel;
  StreamSubscription? _sub;
  Timer? _pingTimer;

  int _packetsReceived = 0;
  int _packetsDropped  = 0;
  int _lastSeq         = -1;
  double _lastArrivalUs = 0;
  double _lastSendUs    = 0;
  double _jitterMs      = 0;

  // ── Clock offset (phone_us - pc_us), measured via RTT ping ──────────────────
  // We use this to correct the one-way latency calculation so it works across
  // devices with unsynchronised clocks (Tailscale, LAN, etc.)
  // Formula:  offset = ((t4 - t1) - (t3 - t2)) / 2  (NTP simplified)
  // Where t1=ping sent (phone), t2=ping received (PC, echoed as t3), t4=pong received (phone)
  // Simplified here: offset = rtt/2 - one_way_raw
  // We just store the smoothed offset and add it to every latency reading.
  double _clockOffsetMs = 0.0;
  double? _pingSentUs;          // phone timestamp when last ping was sent
  bool   _offsetCalibrated = false;

  // Rolling window (100 packets) for mean + p95
  final List<double> _window = [];

  // ── Public API ───────────────────────────────────────────────────────────────
  Future<void> connect() async {
    if (connectionState == StreamConnectionState.connected ||
        connectionState == StreamConnectionState.connecting) return;
    _reset();

    connectionState = StreamConnectionState.connecting;
    notifyListeners();

    try {
      final uri = Uri.parse('ws://$host:$port');
      _channel = WebSocketChannel.connect(uri);

      // Do NOT await _channel!.ready — it can hang indefinitely on mobile
      // networks (Tailscale, LTE) and keep the UI stuck in `connecting`.
      // Instead, attach the listener immediately and let the stream's onError
      // / onDone callbacks handle connection failures naturally.
      _sub = _channel!.stream.listen(
        _onMessage,
        onError: _onError,
        onDone: _onDone,
        cancelOnError: false,
      );

      // Start pinging right away — the PONG response confirms the connection
      // is alive and also calibrates the clock offset.
      _pingTimer = Timer.periodic(const Duration(seconds: 2), (_) => _sendPing());
      _sendPing();

      // Safety net: if no message arrives within 5 seconds, the server is
      // unreachable — flip to error so the user knows to retry.
      Future.delayed(const Duration(seconds: 5), () {
        if (connectionState == StreamConnectionState.connecting) {
          connectionState = StreamConnectionState.error;
          errorMessage = 'Connection timed out — is Node A running?';
          _pingTimer?.cancel();
          _pingTimer = null;
          _sub?.cancel();
          _channel?.sink.close();
          notifyListeners();
        }
      });

    } catch (e) {
      connectionState = StreamConnectionState.error;
      errorMessage = e.toString();
      notifyListeners();
    }
  }

  void disconnect() {
    _pingTimer?.cancel();
    _pingTimer = null;
    _sub?.cancel();
    _channel?.sink.close();
    _handshakeConfirmed = false;
    connectionState = StreamConnectionState.disconnected;
    notifyListeners();
  }

  void updateHost(String h) { host = h; notifyListeners(); }
  void updatePort(int p)    { port = p; notifyListeners(); }

  // ── Ping: send phone's current timestamp as a text frame ────────────────────
  // Node A will echo it back as the same text frame.
  // Format: "PING:<phone_timestamp_us>"
  void _sendPing() {
    if (_channel == null) return;
    _pingSentUs = DateTime.now().microsecondsSinceEpoch.toDouble();
    _channel!.sink.add('PING:${_pingSentUs!.toInt()}');
  }

  // ── Message handler ──────────────────────────────────────────────────────────
  void _onMessage(dynamic raw) {

    // Confirm handshake on the very first message of any type (text or binary).
    // The first message is almost always a PONG text frame because we send a
    // ping immediately on connect — so checking only binary frames caused the
    // app to stay stuck in `connecting` indefinitely.
    if (!_handshakeConfirmed) {
      _handshakeConfirmed = true;
      connectionState = StreamConnectionState.connected;
      notifyListeners();
    }

    // ── Text frame = PONG from Node A ────────────────────────────────────────
    if (raw is String) {
      if (raw.startsWith('PONG:') && _pingSentUs != null) {
        final t4 = DateTime.now().microsecondsSinceEpoch.toDouble();
        final t1 = _pingSentUs!;
        final rttMs = (t4 - t1) / 1000.0;
        final newOffset = rttMs / 2.0;
        if (!_offsetCalibrated) {
          _clockOffsetMs    = newOffset;
          _offsetCalibrated = true;
        } else {
          // EMA alpha=0.1 — stable, spike-resistant
          _clockOffsetMs = _clockOffsetMs * 0.9 + newOffset * 0.1;
        }
        _pingSentUs = null;
      }
      return;
    }

    // ── Binary frame = audio packet ──────────────────────────────────────────
    final packet = AudioPacket.fromBytes(raw);
    if (packet == null) return;

    final nowUs = DateTime.now().microsecondsSinceEpoch.toDouble();

    // ── Sequence gap detection ────────────────────────────────────────────────
    if (_lastSeq >= 0) {
      final expected = _lastSeq + 1;
      if (packet.sequenceNumber != expected) {
        _packetsDropped += (packet.sequenceNumber - expected).abs();
      }
    }
    _lastSeq = packet.sequenceNumber;
    _packetsReceived++;

    // ── Clock-corrected one-way latency ───────────────────────────────────────
    // raw_one_way = phone_now - pc_sent  (wrong if clocks differ)
    // corrected   = _clockOffsetMs       (rtt/2, measured by ping)
    // We use the corrected value once calibrated, raw otherwise.
    final rawLatencyMs  = (nowUs - packet.timestampUs) / 1000.0;
    final latencyMs     = _offsetCalibrated ? _clockOffsetMs : rawLatencyMs.abs();

    // ── RFC 3550 Jitter ───────────────────────────────────────────────────────
    // Jitter uses inter-packet spacing — it is clock-independent and always correct.
    if (_lastArrivalUs > 0 && _lastSendUs > 0) {
      final d = ((nowUs - _lastArrivalUs) - (packet.timestampUs - _lastSendUs)).abs() / 1000.0;
      _jitterMs += (d - _jitterMs) / 16.0;
    }
    _lastArrivalUs = nowUs;
    _lastSendUs    = packet.timestampUs.toDouble();

    // ── Rolling window ────────────────────────────────────────────────────────
    _window.add(latencyMs);
    if (_window.length > 100) _window.removeAt(0);

    final sorted = List<double>.from(_window)..sort();
    final meanMs = sorted.reduce((a, b) => a + b) / sorted.length;
    final p95Ms  = sorted[(sorted.length * 0.95).toInt().clamp(0, sorted.length - 1)];

    // ── History for chart ─────────────────────────────────────────────────────
    latencyHistory.add(latencyMs.clamp(0, 200));
    if (latencyHistory.length > _historyMax) latencyHistory.removeAt(0);

    // ── Loss rate ─────────────────────────────────────────────────────────────
    final totalExpected = _packetsReceived + _packetsDropped;
    final lossRate = totalExpected > 0 ? (_packetsDropped / totalExpected * 100) : 0.0;

    metrics = StreamMetrics(
      latencyMs:         latencyMs.clamp(0, 9999),
      meanLatencyMs:     meanMs.clamp(0, 9999),
      p95LatencyMs:      p95Ms.clamp(0, 9999),
      jitterMs:          _jitterMs.clamp(0, 9999),
      packetsReceived:   _packetsReceived,
      packetsDropped:    _packetsDropped,
      lossRatePct:       lossRate,
      lastSeq:           packet.sequenceNumber,
      jitterBufferDepth: 0,
    );

    notifyListeners();
  }

  void _onError(Object e) {
    connectionState = StreamConnectionState.error;
    errorMessage    = e.toString();
    notifyListeners();
  }

  void _onDone() {
    _pingTimer?.cancel();
    _pingTimer = null;
    _handshakeConfirmed = false;
    connectionState = StreamConnectionState.disconnected;
    notifyListeners();
  }

  void _reset() {
    _handshakeConfirmed  = false;
    _offsetCalibrated    = false;
    _clockOffsetMs       = 0.0;
    _pingSentUs          = null;
    _packetsReceived     = 0;
    _packetsDropped      = 0;
    _lastSeq             = -1;
    _lastArrivalUs       = 0;
    _lastSendUs          = 0;
    _jitterMs            = 0;
    _window.clear();
    latencyHistory.clear();
    metrics = StreamMetrics.zero;
  }

  @override
  void dispose() {
    disconnect();
    super.dispose();
  }
}