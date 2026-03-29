import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:provider/provider.dart';
import 'package:audioplayers/audioplayers.dart';
import '../services/recordings_service.dart';

class RecordingsScreen extends StatefulWidget {
  const RecordingsScreen({super.key});

  @override
  State<RecordingsScreen> createState() => _RecordingsScreenState();
}

class _RecordingsScreenState extends State<RecordingsScreen> {
  final _hostCtrl = TextEditingController(text: '100.95.213.57');
  final _portCtrl = TextEditingController(text: '8080');

  // One shared player for the whole screen
  final _player = AudioPlayer();
  int? _playingId;
  PlayerState _playerState = PlayerState.stopped;
  Duration _position = Duration.zero;
  Duration _duration = Duration.zero;

  @override
  void initState() {
    super.initState();
    _player.onPlayerStateChanged.listen((s) {
      if (mounted) setState(() => _playerState = s);
    });
    _player.onPositionChanged.listen((p) {
      if (mounted) setState(() => _position = p);
    });
    _player.onDurationChanged.listen((d) {
      if (mounted) setState(() => _duration = d);
    });
    _player.onPlayerComplete.listen((_) {
      if (mounted) setState(() { _playingId = null; _position = Duration.zero; });
    });

    WidgetsBinding.instance.addPostFrameCallback((_) {
      context.read<RecordingsService>().refresh();
    });
  }

  @override
  void dispose() {
    _player.dispose();
    _hostCtrl.dispose();
    _portCtrl.dispose();
    super.dispose();
  }

  Future<void> _togglePlay(Recording rec) async {
    final svc = context.read<RecordingsService>();
    if (_playingId == rec.id && _playerState == PlayerState.playing) {
      await _player.pause();
    } else if (_playingId == rec.id && _playerState == PlayerState.paused) {
      await _player.resume();
    } else {
      setState(() { _playingId = rec.id; _position = Duration.zero; });
      await _player.play(UrlSource(svc.wavUrl(rec.id)));
    }
  }

  Future<void> _stopPlay() async {
    await _player.stop();
    setState(() { _playingId = null; _position = Duration.zero; });
  }

  @override
  Widget build(BuildContext context) {
    final svc = context.watch<RecordingsService>();

    return Scaffold(
      backgroundColor: const Color(0xFF0A0C10),
      body: SafeArea(
        child: Column(children: [
          _buildHeader(svc),
          if (_playingId != null) _buildNowPlaying(svc),
          _buildApiPanel(svc),
          _buildStatsBar(svc),
          Expanded(child: _buildList(svc)),
        ]),
      ),
    );
  }

  Widget _buildHeader(RecordingsService svc) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 16),
      decoration: const BoxDecoration(
        color: Color(0xFF111318),
        border: Border(bottom: BorderSide(color: Color(0xFF222733))),
      ),
      child: Row(children: [
        Container(
          width: 28, height: 28,
          decoration: BoxDecoration(
            border: Border.all(color: const Color(0xFF7FFF7F), width: 1.5),
          ),
          child: const Center(
            child: Icon(Icons.album, color: Color(0xFF7FFF7F), size: 16),
          ),
        ),
        const SizedBox(width: 12),
        Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text('RECORDINGS',
            style: GoogleFonts.shareTechMono(
              color: const Color(0xFFEEF2FF), fontSize: 16,
              fontWeight: FontWeight.w600, letterSpacing: 4)),
          Text('WAV ARCHIVE',
            style: GoogleFonts.shareTechMono(
              color: const Color(0xFF3A4A5C), fontSize: 8, letterSpacing: 3)),
        ]),
        const Spacer(),
        GestureDetector(
          onTap: svc.isLoading ? null : svc.refresh,
          child: Container(
            padding: const EdgeInsets.all(8),
            decoration: BoxDecoration(
              border: Border.all(color: const Color(0xFF222733)),
            ),
            child: svc.isLoading
                ? const SizedBox(width: 14, height: 14,
                    child: CircularProgressIndicator(strokeWidth: 1.5,
                      color: Color(0xFF00D4FF)))
                : const Icon(Icons.refresh, color: Color(0xFF00D4FF), size: 16),
          ),
        ),
      ]),
    );
  }

  Widget _buildApiPanel(RecordingsService svc) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: const BoxDecoration(
        color: Color(0xFF0D0F14),
        border: Border(bottom: BorderSide(color: Color(0xFF222733))),
      ),
      child: Row(children: [
        Expanded(
          flex: 3,
          child: _miniField('API HOST', _hostCtrl, '127.0.0.1'),
        ),
        const SizedBox(width: 8),
        Expanded(
          flex: 2,
          child: _miniField('PORT', _portCtrl, '8080',
            keyboardType: TextInputType.number),
        ),
        const SizedBox(width: 8),
        GestureDetector(
          onTap: () {
            svc.updateHost(_hostCtrl.text.trim());
            svc.updateApiPort(int.tryParse(_portCtrl.text) ?? 8080);
            svc.refresh();
          },
          child: Container(
            padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
            margin: const EdgeInsets.only(top: 16),
            decoration: BoxDecoration(
              color: const Color(0xFF001A24),
              border: Border.all(color: const Color(0xFF00D4FF)),
            ),
            child: Text('LOAD',
              style: GoogleFonts.shareTechMono(
                color: const Color(0xFF00D4FF), fontSize: 11, letterSpacing: 1.5)),
          ),
        ),
      ]),
    );
  }

  Widget _miniField(String label, TextEditingController ctrl, String hint,
      {TextInputType keyboardType = TextInputType.text}) {
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Text(label,
        style: GoogleFonts.shareTechMono(
          color: const Color(0xFF3A4A5C), fontSize: 8, letterSpacing: 1.5)),
      const SizedBox(height: 4),
      TextField(
        controller: ctrl,
        keyboardType: keyboardType,
        style: GoogleFonts.shareTechMono(
          color: const Color(0xFFEEF2FF), fontSize: 12),
        decoration: InputDecoration(
          hintText: hint,
          hintStyle: GoogleFonts.shareTechMono(
            color: const Color(0xFF2A3545), fontSize: 12),
          filled: true, fillColor: const Color(0xFF0A0C10),
          contentPadding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
          enabledBorder: const OutlineInputBorder(borderRadius: BorderRadius.zero,
            borderSide: BorderSide(color: Color(0xFF222733))),
          focusedBorder: const OutlineInputBorder(borderRadius: BorderRadius.zero,
            borderSide: BorderSide(color: Color(0xFF00D4FF))),
        ),
      ),
    ]);
  }

  Widget _buildStatsBar(RecordingsService svc) {
    if (svc.error != null) {
      return Container(
        width: double.infinity,
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
        color: const Color(0xFF1A0A08),
        child: Text('⚠  ${svc.error}',
          style: GoogleFonts.shareTechMono(
            color: const Color(0xFFFF6B35), fontSize: 11)),
      );
    }

    final st = svc.stats;
    final totalMb = st.totalBytes / (1024 * 1024);

    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
      color: const Color(0xFF0D0F14),
      child: Row(children: [
        _statChip('CLIPS', '${st.totalClips}'),
        _divider(),
        _statChip('STORAGE', '${totalMb.toStringAsFixed(1)} MB'),
        _divider(),
        _statChip('AVG LAT', '${st.avgLatencyMs.toStringAsFixed(2)} ms'),
        _divider(),
        _statChip('WORST P95', '${st.worstP95Ms.toStringAsFixed(2)} ms',
          alert: st.worstP95Ms > 80),
      ]),
    );
  }

  Widget _statChip(String label, String value, {bool alert = false}) {
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Text(label,
        style: GoogleFonts.shareTechMono(
          color: const Color(0xFF3A4A5C), fontSize: 7, letterSpacing: 1.5)),
      Text(value,
        style: GoogleFonts.shareTechMono(
          color: alert ? const Color(0xFFFF6B35) : const Color(0xFFEEF2FF),
          fontSize: 12, fontWeight: FontWeight.w600)),
    ]);
  }

  Widget _divider() => Container(
    width: 1, height: 28, margin: const EdgeInsets.symmetric(horizontal: 12),
    color: const Color(0xFF222733));

  Widget _buildNowPlaying(RecordingsService svc) {
    final rec = svc.recordings.firstWhere(
      (r) => r.id == _playingId,
      orElse: () => svc.recordings.first,
    );
    final isPlaying = _playerState == PlayerState.playing;
    final progress = _duration.inMilliseconds > 0
        ? _position.inMilliseconds / _duration.inMilliseconds
        : 0.0;

    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
      decoration: const BoxDecoration(
        color: Color(0xFF0A1A12),
        border: Border(
          bottom: BorderSide(color: Color(0xFF7FFF7F), width: 1),
          left: BorderSide(color: Color(0xFF7FFF7F), width: 2),
        ),
      ),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          const Icon(Icons.graphic_eq, color: Color(0xFF7FFF7F), size: 12),
          const SizedBox(width: 6),
          Text('NOW PLAYING',
            style: GoogleFonts.shareTechMono(
              color: const Color(0xFF7FFF7F), fontSize: 8, letterSpacing: 2)),
          const Spacer(),
          GestureDetector(
            onTap: _stopPlay,
            child: const Icon(Icons.close, color: Color(0xFF3A4A5C), size: 16),
          ),
        ]),
        const SizedBox(height: 6),
        Text(rec.filename,
          style: GoogleFonts.shareTechMono(
            color: const Color(0xFFEEF2FF), fontSize: 11),
          maxLines: 1, overflow: TextOverflow.ellipsis),
        const SizedBox(height: 8),
        Row(children: [
          GestureDetector(
            onTap: () => _togglePlay(rec),
            child: Container(
              width: 28, height: 28,
              decoration: BoxDecoration(
                border: Border.all(color: const Color(0xFF7FFF7F)),
              ),
              child: Icon(
                isPlaying ? Icons.pause : Icons.play_arrow,
                color: const Color(0xFF7FFF7F), size: 16),
            ),
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              SliderTheme(
                data: SliderThemeData(
                  trackHeight: 2,
                  thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 5),
                  overlayShape: const RoundSliderOverlayShape(overlayRadius: 10),
                  activeTrackColor: const Color(0xFF7FFF7F),
                  inactiveTrackColor: const Color(0xFF1A2535),
                  thumbColor: const Color(0xFF7FFF7F),
                  overlayColor: const Color(0x207FFF7F),
                ),
                child: Slider(
                  value: progress.clamp(0.0, 1.0),
                  onChanged: (v) async {
                    final pos = Duration(
                      milliseconds: (v * _duration.inMilliseconds).round());
                    await _player.seek(pos);
                  },
                ),
              ),
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  Text(_formatDuration(_position),
                    style: GoogleFonts.shareTechMono(
                      color: const Color(0xFF3A4A5C), fontSize: 9)),
                  Text(_formatDuration(_duration),
                    style: GoogleFonts.shareTechMono(
                      color: const Color(0xFF3A4A5C), fontSize: 9)),
                ],
              ),
            ]),
          ),
        ]),
      ]),
    );
  }

  String _formatDuration(Duration d) {
    final m = d.inMinutes.remainder(60).toString().padLeft(2, '0');
    final s = d.inSeconds.remainder(60).toString().padLeft(2, '0');
    return '$m:$s';
  }

  Widget _buildList(RecordingsService svc) {
    if (svc.isLoading && svc.recordings.isEmpty) {
      return const Center(
        child: CircularProgressIndicator(color: Color(0xFF00D4FF), strokeWidth: 1.5));
    }
    if (svc.recordings.isEmpty) {
      return Center(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          const Icon(Icons.folder_open, color: Color(0xFF222733), size: 48),
          const SizedBox(height: 12),
          Text('No recordings yet',
            style: GoogleFonts.shareTechMono(
              color: const Color(0xFF3A4A5C), fontSize: 13)),
          const SizedBox(height: 4),
          Text('Run node_B to start capturing',
            style: GoogleFonts.shareTechMono(
              color: const Color(0xFF222733), fontSize: 11)),
        ]),
      );
    }

    return ListView.separated(
      padding: const EdgeInsets.all(16),
      itemCount: svc.recordings.length,
      separatorBuilder: (_, __) => const SizedBox(height: 8),
      itemBuilder: (_, i) => _RecordingTile(
        rec: svc.recordings[i],
        isPlaying: _playingId == svc.recordings[i].id &&
                   _playerState == PlayerState.playing,
        isPaused: _playingId == svc.recordings[i].id &&
                  _playerState == PlayerState.paused,
        onPlay: () => _togglePlay(svc.recordings[i]),
        onDelete: () => _confirmDelete(svc, svc.recordings[i]),
      ),
    );
  }

  void _confirmDelete(RecordingsService svc, Recording rec) {
    showDialog(
      context: context,
      builder: (_) => AlertDialog(
        backgroundColor: const Color(0xFF111318),
        shape: const RoundedRectangleBorder(borderRadius: BorderRadius.zero),
        title: Text('DELETE RECORDING',
          style: GoogleFonts.shareTechMono(
            color: const Color(0xFFFF6B35), fontSize: 13, letterSpacing: 2)),
        content: Text('Clip #${rec.clipIndex} — ${rec.filename}\n\nThis will delete the WAV file permanently.',
          style: GoogleFonts.shareTechMono(
            color: const Color(0xFFC8D0E0), fontSize: 12)),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: Text('CANCEL',
              style: GoogleFonts.shareTechMono(
                color: const Color(0xFF3A4A5C), fontSize: 11))),
          TextButton(
            onPressed: () async {
              Navigator.pop(context);
              if (_playingId == rec.id) await _stopPlay();
              await svc.deleteRecording(rec.id);
            },
            child: Text('DELETE',
              style: GoogleFonts.shareTechMono(
                color: const Color(0xFFFF6B35), fontSize: 11))),
        ],
      ),
    );
  }
}

class _RecordingTile extends StatelessWidget {
  final Recording rec;
  final bool isPlaying;
  final bool isPaused;
  final VoidCallback onPlay;
  final VoidCallback onDelete;

  const _RecordingTile({
    required this.rec,
    required this.isPlaying,
    required this.isPaused,
    required this.onPlay,
    required this.onDelete,
  });

  @override
  Widget build(BuildContext context) {
    final active = isPlaying || isPaused;
    final accentColor = active ? const Color(0xFF7FFF7F) : const Color(0xFF222733);
    final lossAlert = rec.lossRatePct > 1.0;

    return Container(
      decoration: BoxDecoration(
        color: const Color(0xFF111318),
        border: Border(left: BorderSide(color: accentColor, width: 2)),
      ),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        // Top row — clip info + controls
        Padding(
          padding: const EdgeInsets.fromLTRB(12, 12, 12, 8),
          child: Row(children: [
            // Play/pause button
            GestureDetector(
              onTap: onPlay,
              child: Container(
                width: 36, height: 36,
                decoration: BoxDecoration(
                  color: active ? const Color(0xFF0A1A12) : const Color(0xFF0A0C10),
                  border: Border.all(color: accentColor),
                ),
                child: Icon(
                  isPlaying ? Icons.pause : Icons.play_arrow,
                  color: active ? const Color(0xFF7FFF7F) : const Color(0xFF3A4A5C),
                  size: 18),
              ),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text('CLIP #${rec.clipIndex.toString().padLeft(4, '0')}',
                  style: GoogleFonts.shareTechMono(
                    color: active ? const Color(0xFF7FFF7F) : const Color(0xFF00D4FF),
                    fontSize: 11, letterSpacing: 1.5, fontWeight: FontWeight.w600)),
                const SizedBox(height: 2),
                Text(rec.filename,
                  style: GoogleFonts.shareTechMono(
                    color: const Color(0xFF3A4A5C), fontSize: 9),
                  maxLines: 1, overflow: TextOverflow.ellipsis),
              ]),
            ),
            // Delete button
            GestureDetector(
              onTap: onDelete,
              child: Container(
                padding: const EdgeInsets.all(6),
                child: const Icon(Icons.delete_outline,
                  color: Color(0xFF2A3545), size: 16)),
            ),
          ]),
        ),

        // Stats grid — two rows so nothing overflows on narrow screens
        Container(
          padding: const EdgeInsets.fromLTRB(12, 8, 12, 12),
          decoration: const BoxDecoration(
            border: Border(top: BorderSide(color: Color(0xFF1A2535))),
          ),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            // Row 1: session · sample rate · size
            IntrinsicHeight(
              child: Row(children: [
                Expanded(child: _chip(rec.sessionId, 'SESSION')),
                _vdiv(),
                Expanded(child: _chip('${rec.sampleRate ~/ 1000}kHz', 'SAMPLE RATE')),
                _vdiv(),
                Expanded(child: _chip(rec.fileSizeFormatted, 'SIZE')),
              ]),
            ),
            const SizedBox(height: 8),
            // Row 2: mean latency · loss · recorded time
            IntrinsicHeight(
              child: Row(children: [
                Expanded(child: _chip('${rec.meanLatencyMs.toStringAsFixed(2)}ms', 'MEAN LAT')),
                _vdiv(),
                Expanded(child: _chip('${rec.lossRatePct.toStringAsFixed(1)}%', 'LOSS',
                  alert: lossAlert)),
                _vdiv(),
                Expanded(child: _chip(
                  rec.createdAt.length >= 16
                      ? rec.createdAt.replaceFirst('T', ' ').substring(5, 16)
                      : rec.createdAt,
                  'RECORDED')),
              ]),
            ),
          ]),
        ),
      ]),
    );
  }

  Widget _chip(String top, String bottom, {bool alert = false}) {
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Text(top,
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
        style: GoogleFonts.shareTechMono(
          color: alert ? const Color(0xFFFF6B35) : const Color(0xFFEEF2FF),
          fontSize: 10, fontWeight: FontWeight.w600)),
      Text(bottom,
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
        style: GoogleFonts.shareTechMono(
          color: const Color(0xFF3A4A5C), fontSize: 8, letterSpacing: 1)),
    ]);
  }

  Widget _vdiv() => Container(
    width: 1, height: 24, margin: const EdgeInsets.symmetric(horizontal: 8),
    color: const Color(0xFF1A2535));
}