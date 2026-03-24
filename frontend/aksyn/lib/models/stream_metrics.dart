/// Aggregated metrics computed from received packets
class StreamMetrics {
  final double latencyMs;        // one-way latency of last packet
  final double meanLatencyMs;    // rolling mean over last 100 packets
  final double p95LatencyMs;     // p95 over last 100 packets
  final double jitterMs;         // RFC 3550 jitter estimate
  final int    packetsReceived;
  final int    packetsDropped;
  final double lossRatePct;
  final int    lastSeq;
  final int    jitterBufferDepth; // how many packets are buffered

  const StreamMetrics({
    required this.latencyMs,
    required this.meanLatencyMs,
    required this.p95LatencyMs,
    required this.jitterMs,
    required this.packetsReceived,
    required this.packetsDropped,
    required this.lossRatePct,
    required this.lastSeq,
    required this.jitterBufferDepth,
  });

  static const StreamMetrics zero = StreamMetrics(
    latencyMs: 0, meanLatencyMs: 0, p95LatencyMs: 0,
    jitterMs: 0, packetsReceived: 0, packetsDropped: 0,
    lossRatePct: 0, lastSeq: 0, jitterBufferDepth: 0,
  );
}
