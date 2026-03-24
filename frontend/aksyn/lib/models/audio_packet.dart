import 'dart:typed_data';

/// Mirrors the C++ AudioPacket struct from protocol.h (packed, 44-byte header)
/// Layout:
///   offset  0 : magic            uint32  (4 bytes) — must == 0x41554431
///   offset  4 : sequence_number  uint32  (4 bytes)
///   offset  8 : timestamp_us     uint64  (8 bytes)
///   offset 16 : session_start_us uint64  (8 bytes)
///   offset 24 : session_id       uint32  (4 bytes)
///   offset 28 : clip_index       uint32  (4 bytes)
///   offset 32 : sample_rate      uint32  (4 bytes)
///   offset 36 : channels         uint16  (2 bytes)
///   offset 38 : bits_per_sample  uint16  (2 bytes)
///   offset 40 : payload_bytes    uint16  (2 bytes)
///   offset 42 : reserved         uint16  (2 bytes)
///   [total header = 44 bytes]
class AudioPacket {
  static const int headerSize = 44;
  static const int magic      = 0x41554431; // "AUD1"

  final int sequenceNumber;
  final int timestampUs;
  final int sessionStartUs;
  final int sessionId;
  final int clipIndex;
  final int sampleRate;
  final int channels;
  final int bitsPerSample;
  final int payloadBytes;
  final Uint8List payload;

  const AudioPacket({
    required this.sequenceNumber,
    required this.timestampUs,
    required this.sessionStartUs,
    required this.sessionId,
    required this.clipIndex,
    required this.sampleRate,
    required this.channels,
    required this.bitsPerSample,
    required this.payloadBytes,
    required this.payload,
  });

  /// Parse from raw binary WebSocket frame.
  /// Accepts both Uint8List and List<int> (web_socket_channel can return either).
  static AudioPacket? fromBytes(dynamic raw) {
    final Uint8List bytes;
    if (raw is Uint8List) {
      bytes = raw;
    } else if (raw is List<int>) {
      bytes = Uint8List.fromList(raw);
    } else {
      return null;
    }

    if (bytes.length < headerSize) return null;

    final bd = ByteData.sublistView(bytes);

    // Validate magic number before trusting any other field
    final magicVal = bd.getUint32(0, Endian.little);
    if (magicVal != magic) return null;

    final seq            = bd.getUint32(4,  Endian.little);
    final tsUs           = bd.getInt64 (8,  Endian.little); // signed to avoid overflow
    final sessionStartUs = bd.getInt64 (16, Endian.little);
    final sessionId      = bd.getUint32(24, Endian.little);
    final clipIndex      = bd.getUint32(28, Endian.little);
    final sr             = bd.getUint32(32, Endian.little);
    final channels       = bd.getUint16(36, Endian.little);
    final bps            = bd.getUint16(38, Endian.little);
    final pBytes         = bd.getUint16(40, Endian.little);
    // offset 42 = reserved, skip

    final payload = bytes.sublist(headerSize);

    return AudioPacket(
      sequenceNumber: seq,
      timestampUs:    tsUs,
      sessionStartUs: sessionStartUs,
      sessionId:      sessionId,
      clipIndex:      clipIndex,
      sampleRate:     sr,
      channels:       channels,
      bitsPerSample:  bps,
      payloadBytes:   pBytes,
      payload:        payload,
    );
  }
}