import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;

class Recording {
  final int id;
  final int clipIndex;
  final String sessionId;
  final int sampleRate;
  final String filePath;
  final int fileSizeBytes;
  final int pktsRx;
  final int pktsDropped;
  final double meanLatencyMs;
  final double p95LatencyMs;
  final String createdAt;

  const Recording({
    required this.id,
    required this.clipIndex,
    required this.sessionId,
    required this.sampleRate,
    required this.filePath,
    required this.fileSizeBytes,
    required this.pktsRx,
    required this.pktsDropped,
    required this.meanLatencyMs,
    required this.p95LatencyMs,
    required this.createdAt,
  });

  factory Recording.fromJson(Map<String, dynamic> j) => Recording(
        id: j['id'] as int? ?? 0,
        clipIndex: j['clip_index'] as int? ?? 0,
        sessionId: j['session_id'] as String? ?? '',
        sampleRate: j['sample_rate'] as int? ?? 0,
        filePath: j['file_path'] as String? ?? '',
        fileSizeBytes: j['file_size_bytes'] as int? ?? 0,
        pktsRx: j['pkts_rx'] as int? ?? 0,
        pktsDropped: j['pkts_dropped'] as int? ?? 0,
        meanLatencyMs: (j['mean_latency_ms'] as num?)?.toDouble() ?? 0.0,
        p95LatencyMs: (j['p95_latency_ms'] as num?)?.toDouble() ?? 0.0,
        createdAt: j['created_at'] as String? ?? '',
      );

  String get filename => filePath.split(RegExp(r'[/\\]')).last;
  String get fileSizeFormatted {
    if (fileSizeBytes < 1024) return '$fileSizeBytes B';
    if (fileSizeBytes < 1024 * 1024) return '${(fileSizeBytes / 1024).toStringAsFixed(1)} KB';
    return '${(fileSizeBytes / (1024 * 1024)).toStringAsFixed(1)} MB';
  }

  double get lossRatePct {
    final total = pktsRx + pktsDropped;
    return total > 0 ? pktsDropped / total * 100 : 0.0;
  }
}

class ApiStats {
  final int totalClips;
  final int totalBytes;
  final double avgLatencyMs;
  final double worstP95Ms;

  const ApiStats({
    required this.totalClips,
    required this.totalBytes,
    required this.avgLatencyMs,
    required this.worstP95Ms,
  });

  factory ApiStats.fromJson(Map<String, dynamic> j) => ApiStats(
        totalClips: j['total_clips'] as int? ?? 0,
        totalBytes: j['total_bytes'] as int? ?? 0,
        avgLatencyMs: (j['avg_latency_ms'] as num?)?.toDouble() ?? 0.0,
        worstP95Ms: (j['worst_p95_ms'] as num?)?.toDouble() ?? 0.0,
      );

  static const ApiStats zero =
      ApiStats(totalClips: 0, totalBytes: 0, avgLatencyMs: 0, worstP95Ms: 0);
}

class RecordingsService extends ChangeNotifier {
  String host = '192.168.1.37';
  int apiPort = 8080;

  List<Recording> recordings = [];
  ApiStats stats = ApiStats.zero;
  bool isLoading = false;
  String? error;

  String get baseUrl => 'http://$host:$apiPort';

  void updateHost(String h) {
    host = h;
    notifyListeners();
  }

  void updateApiPort(int p) {
    apiPort = p;
    notifyListeners();
  }

  Future<void> refresh() async {
    isLoading = true;
    error = null;
    notifyListeners();
    try {
      await Future.wait([_fetchRecordings(), _fetchStats()]);
    } catch (e) {
      error = e.toString();
    }
    isLoading = false;
    notifyListeners();
  }

  Future<void> _fetchRecordings() async {
    final res = await http
        .get(Uri.parse('$baseUrl/recordings?limit=100'))
        .timeout(const Duration(seconds: 5));
    if (res.statusCode == 200) {
      final list = jsonDecode(res.body) as List;
      recordings = list.map((e) => Recording.fromJson(e as Map<String, dynamic>)).toList();
    } else {
      throw Exception('HTTP ${res.statusCode}');
    }
  }

  Future<void> _fetchStats() async {
    final res = await http
        .get(Uri.parse('$baseUrl/stats'))
        .timeout(const Duration(seconds: 5));
    if (res.statusCode == 200) {
      final list = jsonDecode(res.body) as List;
      if (list.isNotEmpty) {
        stats = ApiStats.fromJson(list.first as Map<String, dynamic>);
      }
    }
  }

  Future<bool> deleteRecording(int id) async {
    try {
      final res = await http
          .delete(Uri.parse('$baseUrl/recordings/$id'))
          .timeout(const Duration(seconds: 5));
      if (res.statusCode == 200) {
        recordings.removeWhere((r) => r.id == id);
        notifyListeners();
        return true;
      }
    } catch (_) {}
    return false;
  }

  /// Returns the streaming URL for a recording (used by audio_players)
  String wavUrl(int id) => '$baseUrl/recordings/$id/file';
}