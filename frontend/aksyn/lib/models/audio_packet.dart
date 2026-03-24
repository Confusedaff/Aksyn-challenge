import 'dart:typed_data';

/// Mirrors the C++ AudioPacket struct (packed, no padding)
/// Layout:
///   uint32_t sequence_number   — 4 bytes  offset 0
///   uint64_t timestamp_us      — 8 bytes  offset 4
///   uint32_t sample_rate       — 4 bytes  offset 12
///   uint16_t payload_bytes     — 2 bytes  offset 16
///   [total header = 18 bytes]
///
/// NOTE: adjust offsets to match your actual protocol.h struct layout.
class AudioPacket {
  static const int headerSize = 18;

  final int sequenceNumber;
  final int timestampUs;
  final int sampleRate;
  final int payloadBytes;
  final Uint8List payload;

  const AudioPacket({
    required this.sequenceNumber,
    required this.timestampUs,
    required this.sampleRate,
    required this.payloadBytes,
    required this.payload,
  });

  /// Parse from raw binary WebSocket frame
  static AudioPacket? fromBytes(List<int> bytes) {
    if (bytes.length < headerSize) return null;
    final bd = ByteData.sublistView(Uint8List.fromList(bytes));
    final seq       = bd.getUint32(0,  Endian.little);
    final tsUs      = bd.getUint64(4,  Endian.little);  // int64 read as uint64
    final sr        = bd.getUint32(12, Endian.little);
    final pBytes    = bd.getUint16(16, Endian.little);
    final payload   = Uint8List.fromList(bytes.sublist(headerSize));
    return AudioPacket(
      sequenceNumber: seq,
      timestampUs: tsUs,
      sampleRate: sr,
      payloadBytes: pBytes,
      payload: payload,
    );
  }
}
