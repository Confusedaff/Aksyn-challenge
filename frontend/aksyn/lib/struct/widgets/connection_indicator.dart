import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';
// Fix #1: Import the renamed enum directly — no `hide` needed anymore.
import '../services/audio_stream_service.dart' show StreamConnectionState;

class ConnectionIndicator extends StatefulWidget {
  final StreamConnectionState state;
  const ConnectionIndicator({super.key, required this.state});

  @override
  State<ConnectionIndicator> createState() => _ConnectionIndicatorState();
}

class _ConnectionIndicatorState extends State<ConnectionIndicator>
    with SingleTickerProviderStateMixin {
  late AnimationController _pulse;

  @override
  void initState() {
    super.initState();
    _pulse = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1200),
    )..repeat(reverse: true);
  }

  @override
  void dispose() {
    _pulse.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final (color, label) = switch (widget.state) {
      StreamConnectionState.connected    => (const Color(0xFF7FFF7F), 'LIVE'),
      StreamConnectionState.connecting   => (const Color(0xFFFFD080), 'CONNECTING'),
      StreamConnectionState.error        => (const Color(0xFFFF6B35), 'ERROR'),
      StreamConnectionState.disconnected => (const Color(0xFF3A4A5C), 'OFFLINE'),
    };

    final animate = widget.state == StreamConnectionState.connected ||
                    widget.state == StreamConnectionState.connecting;

    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        AnimatedBuilder(
          animation: _pulse,
          builder: (_, __) => Container(
            width: 8, height: 8,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: animate
                  ? color.withOpacity(0.4 + 0.6 * _pulse.value)
                  : color,
              boxShadow: animate ? [
                BoxShadow(color: color.withOpacity(0.5 * _pulse.value), blurRadius: 8),
              ] : null,
            ),
          ),
        ),
        const SizedBox(width: 8),
        Text(
          label,
          style: GoogleFonts.shareTechMono(
            color: color,
            fontSize: 10,
            letterSpacing: 2,
          ),
        ),
      ],
    );
  }
}