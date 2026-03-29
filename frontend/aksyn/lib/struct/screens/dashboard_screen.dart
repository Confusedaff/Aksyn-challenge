// Fix #1: No `hide ConnectionState` needed — we renamed our enum.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:provider/provider.dart';
import '../services/audio_stream_service.dart';
import '../widgets/latency_chart.dart';
import '../widgets/metric_card.dart';
import '../widgets/connection_indicator.dart';

class DashboardScreen extends StatefulWidget {
  const DashboardScreen({super.key});

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen> {
  final _hostController = TextEditingController(text: '100.95.213.57');
  final _portController = TextEditingController(text: '9001');

  @override
  void dispose() {
    _hostController.dispose();
    _portController.dispose();
    super.dispose();
  }

  // Fix #6 / #7: connect() is now async; we call it with unawaited() so the
  // `connecting` state persists visibly until the first packet arrives,
  // making the loading spinner actually visible to the user.
  void _handleConnect(AudioStreamService svc) {
    svc.updateHost(_hostController.text.trim());
    svc.updatePort(int.tryParse(_portController.text) ?? 9001);
    svc.connect(); // returns Future; errors surface via StreamConnectionState.error
  }

  @override
  Widget build(BuildContext context) {
    final svc = context.watch<AudioStreamService>();
    final m   = svc.metrics;
    final isConnected = svc.connectionState == StreamConnectionState.connected;

    // Compute dynamic chart Y max
    final chartMax = svc.latencyHistory.isEmpty
        ? 50.0
        : (svc.latencyHistory.reduce((a, b) => a > b ? a : b) * 1.3).clamp(5.0, 200.0);

    return Scaffold(
      backgroundColor: const Color(0xFF0A0C10),
      body: SafeArea(
        child: Column(
          children: [

            // ── Header ────────────────────────────────────────────────────────
            _Header(connectionState: svc.connectionState),

            Expanded(
              child: SingleChildScrollView(
                padding: const EdgeInsets.all(20),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [

                    // ── Connection panel ──────────────────────────────────────
                    _ConnectionPanel(
                      hostController: _hostController,
                      portController: _portController,
                      isConnected: isConnected,
                      connectionState: svc.connectionState,
                      onConnect: () => _handleConnect(svc),
                      onDisconnect: svc.disconnect,
                    ),

                    const SizedBox(height: 24),

                    // ── Primary metrics row ───────────────────────────────────
                    _SectionLabel('LATENCY'),
                    const SizedBox(height: 10),
                    _MetricRow(children: [
                      MetricCard(
                        label: 'Last Packet',
                        value: m.latencyMs.toStringAsFixed(2),
                        unit: 'ms',
                        accentColor: const Color(0xFF00D4FF),
                      ),
                      MetricCard(
                        label: 'Mean (100 pkt)',
                        value: m.meanLatencyMs.toStringAsFixed(2),
                        unit: 'ms',
                        accentColor: const Color(0xFF00D4FF),
                      ),
                      MetricCard(
                        label: 'P95',
                        value: m.p95LatencyMs.toStringAsFixed(2),
                        unit: 'ms',
                        accentColor: const Color(0xFF7FFF7F),
                        isAlert: m.p95LatencyMs > 80,
                      ),
                      MetricCard(
                        label: 'Jitter',
                        value: m.jitterMs.toStringAsFixed(2),
                        unit: 'ms',
                        accentColor: const Color(0xFFFFD080),
                        isAlert: m.jitterMs > 10,
                      ),
                    ]),

                    const SizedBox(height: 24),

                    // ── Latency chart ─────────────────────────────────────────
                    _SectionLabel('LATENCY HISTORY  ·  last ${svc.latencyHistory.length} packets'),
                    const SizedBox(height: 10),
                    Container(
                      height: 160,
                      padding: const EdgeInsets.fromLTRB(8, 12, 12, 8),
                      decoration: BoxDecoration(
                        color: const Color(0xFF111318),
                        border: Border.all(color: const Color(0xFF222733)),
                      ),
                      child: LatencyChart(
                        data: svc.latencyHistory,
                        maxY: chartMax,
                      ),
                    ),

                    const SizedBox(height: 24),

                    // ── Packet stats ──────────────────────────────────────────
                    _SectionLabel('PACKET STATISTICS'),
                    const SizedBox(height: 10),
                    _MetricRow(children: [
                      MetricCard(
                        label: 'Received',
                        value: '${m.packetsReceived}',
                        unit: 'pkts',
                        accentColor: const Color(0xFF7FFF7F),
                      ),
                      MetricCard(
                        label: 'Dropped',
                        value: '${m.packetsDropped}',
                        unit: 'pkts',
                        accentColor: const Color(0xFFFF6B35),
                        isAlert: m.packetsDropped > 0,
                      ),
                      MetricCard(
                        label: 'Loss Rate',
                        value: m.lossRatePct.toStringAsFixed(2),
                        unit: '%',
                        accentColor: const Color(0xFFFF6B35),
                        isAlert: m.lossRatePct > 1.0,
                      ),
                      MetricCard(
                        label: 'Last SEQ',
                        value: '${m.lastSeq}',
                        unit: '',
                        accentColor: const Color(0xFF3A4A5C),
                      ),
                    ]),

                    const SizedBox(height: 24),

                    // ── Status banner ─────────────────────────────────────────
                    if (svc.connectionState == StreamConnectionState.error)
                      _ErrorBanner(message: svc.errorMessage),

                    if (isConnected && m.packetsReceived > 0)
                      _StatusBanner(metrics: m),

                    const SizedBox(height: 40),
                  ],
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

// ── Sub-widgets ────────────────────────────────────────────────────────────────

class _Header extends StatelessWidget {
  // Fix #1: Use StreamConnectionState throughout — no clash with material.dart.
  final StreamConnectionState connectionState;
  const _Header({required this.connectionState});

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 16),
      decoration: const BoxDecoration(
        color: Color(0xFF111318),
        border: Border(bottom: BorderSide(color: Color(0xFF222733))),
      ),
      child: Row(
        children: [
          Container(
            width: 28, height: 28,
            decoration: BoxDecoration(
              border: Border.all(color: const Color(0xFF00D4FF), width: 1.5),
            ),
            child: const Center(
              child: Icon(Icons.graphic_eq, color: Color(0xFF00D4FF), size: 16),
            ),
          ),
          const SizedBox(width: 12),
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                'AKSYN',
                style: GoogleFonts.shareTechMono(
                  color: const Color(0xFFEEF2FF),
                  fontSize: 16,
                  fontWeight: FontWeight.w600,
                  letterSpacing: 4,
                ),
              ),
              Text(
                'STREAM MONITOR',
                style: GoogleFonts.shareTechMono(
                  color: const Color(0xFF3A4A5C),
                  fontSize: 8,
                  letterSpacing: 3,
                ),
              ),
            ],
          ),
          const Spacer(),
          ConnectionIndicator(state: connectionState),
        ],
      ),
    );
  }
}

class _ConnectionPanel extends StatelessWidget {
  final TextEditingController hostController;
  final TextEditingController portController;
  final bool isConnected;
  final StreamConnectionState connectionState;
  final VoidCallback onConnect;
  final VoidCallback onDisconnect;

  const _ConnectionPanel({
    required this.hostController,
    required this.portController,
    required this.isConnected,
    required this.connectionState,
    required this.onConnect,
    required this.onDisconnect,
  });

  @override
  Widget build(BuildContext context) {
    // Fix #7: isLoading now correctly spans both `connecting` states:
    // - After connect() is called (TCP handshake in progress)
    // - Until the first real packet arrives (WebSocket confirmed)
    // This means the spinner is visible for the full real connection duration.
    final isLoading = connectionState == StreamConnectionState.connecting;

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: const Color(0xFF111318),
        border: Border.all(color: const Color(0xFF222733)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            'NODE A CONNECTION',
            style: GoogleFonts.shareTechMono(
              color: const Color(0xFF3A4A5C),
              fontSize: 9,
              letterSpacing: 2,
            ),
          ),
          const SizedBox(height: 14),
          Row(
            children: [
              Expanded(
                flex: 3,
                child: _Field(
                  controller: hostController,
                  label: 'HOST',
                  hint: '127.0.0.1',
                  enabled: !isConnected && !isLoading,
                  keyboardType: TextInputType.text,
                ),
              ),
              const SizedBox(width: 10),
              Expanded(
                flex: 1,
                child: _Field(
                  controller: portController,
                  label: 'PORT',
                  hint: '9001',
                  enabled: !isConnected && !isLoading,
                  keyboardType: TextInputType.number,
                  inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                ),
              ),
              const SizedBox(width: 10),
              _ConnectButton(
                isConnected: isConnected,
                isLoading: isLoading,
                onConnect: onConnect,
                onDisconnect: onDisconnect,
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _Field extends StatelessWidget {
  final TextEditingController controller;
  final String label;
  final String hint;
  final bool enabled;
  final TextInputType keyboardType;
  final List<TextInputFormatter>? inputFormatters;

  const _Field({
    required this.controller,
    required this.label,
    required this.hint,
    required this.enabled,
    required this.keyboardType,
    this.inputFormatters,
  });

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          label,
          style: GoogleFonts.shareTechMono(
            color: const Color(0xFF3A4A5C),
            fontSize: 8,
            letterSpacing: 1.5,
          ),
        ),
        const SizedBox(height: 4),
        TextField(
          controller: controller,
          enabled: enabled,
          keyboardType: keyboardType,
          inputFormatters: inputFormatters,
          style: GoogleFonts.shareTechMono(
            color: const Color(0xFFEEF2FF),
            fontSize: 13,
          ),
          decoration: InputDecoration(
            hintText: hint,
            hintStyle: GoogleFonts.shareTechMono(
              color: const Color(0xFF2A3545),
              fontSize: 13,
            ),
            filled: true,
            fillColor: const Color(0xFF0A0C10),
            contentPadding: const EdgeInsets.symmetric(horizontal: 10, vertical: 10),
            enabledBorder: const OutlineInputBorder(
              borderRadius: BorderRadius.zero,
              borderSide: BorderSide(color: Color(0xFF222733)),
            ),
            focusedBorder: const OutlineInputBorder(
              borderRadius: BorderRadius.zero,
              borderSide: BorderSide(color: Color(0xFF00D4FF)),
            ),
            disabledBorder: const OutlineInputBorder(
              borderRadius: BorderRadius.zero,
              borderSide: BorderSide(color: Color(0xFF1A2535)),
            ),
          ),
        ),
      ],
    );
  }
}

class _ConnectButton extends StatelessWidget {
  final bool isConnected;
  final bool isLoading;
  final VoidCallback onConnect;
  final VoidCallback onDisconnect;

  const _ConnectButton({
    required this.isConnected,
    required this.isLoading,
    required this.onConnect,
    required this.onDisconnect,
  });

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: isLoading ? null : (isConnected ? onDisconnect : onConnect),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
        margin: const EdgeInsets.only(top: 16),
        decoration: BoxDecoration(
          color: isConnected
              ? const Color(0xFF1A1018)
              : const Color(0xFF001A24),
          border: Border.all(
            color: isConnected
                ? const Color(0xFFFF6B35)
                : const Color(0xFF00D4FF),
          ),
        ),
        child: isLoading
            ? const SizedBox(
                width: 14, height: 14,
                child: CircularProgressIndicator(
                  strokeWidth: 1.5,
                  color: Color(0xFF00D4FF),
                ),
              )
            : Text(
                isConnected ? 'DISCONNECT' : 'CONNECT',
                style: GoogleFonts.shareTechMono(
                  color: isConnected
                      ? const Color(0xFFFF6B35)
                      : const Color(0xFF00D4FF),
                  fontSize: 11,
                  letterSpacing: 1.5,
                ),
              ),
      ),
    );
  }
}

class _MetricRow extends StatelessWidget {
  final List<Widget> children;
  const _MetricRow({required this.children});

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final cols = constraints.maxWidth > 500 ? 4 : 2;
        return GridView.count(
          crossAxisCount: cols,
          shrinkWrap: true,
          physics: const NeverScrollableScrollPhysics(),
          crossAxisSpacing: 8,
          mainAxisSpacing: 8,
          childAspectRatio: 1.6,
          children: children,
        );
      },
    );
  }
}

class _SectionLabel extends StatelessWidget {
  final String text;
  const _SectionLabel(this.text);

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Container(width: 16, height: 1, color: const Color(0xFF00D4FF)),
        const SizedBox(width: 8),
        Text(
          text,
          style: GoogleFonts.shareTechMono(
            color: const Color(0xFF00D4FF),
            fontSize: 9,
            letterSpacing: 2.5,
          ),
        ),
      ],
    );
  }
}

class _ErrorBanner extends StatelessWidget {
  final String message;
  const _ErrorBanner({required this.message});

  @override
  Widget build(BuildContext context) {
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(14),
      margin: const EdgeInsets.only(bottom: 16),
      decoration: const BoxDecoration(
        color: Color(0xFF1A0A08),
        border: Border(left: BorderSide(color: Color(0xFFFF6B35), width: 2)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('CONNECTION ERROR',
            style: GoogleFonts.shareTechMono(
              color: const Color(0xFFFF6B35), fontSize: 9, letterSpacing: 2)),
          const SizedBox(height: 4),
          Text(message,
            style: GoogleFonts.shareTechMono(
              color: const Color(0xFFC8D0E0), fontSize: 12)),
        ],
      ),
    );
  }
}

class _StatusBanner extends StatelessWidget {
  final dynamic metrics;
  const _StatusBanner({required this.metrics});

  @override
  Widget build(BuildContext context) {
    final isHealthy = metrics.meanLatencyMs < 50 &&
                      metrics.lossRatePct < 1.0 &&
                      metrics.jitterMs < 10.0;

    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: isHealthy ? const Color(0xFF081408) : const Color(0xFF1A1008),
        border: Border(
          left: BorderSide(
            color: isHealthy ? const Color(0xFF7FFF7F) : const Color(0xFFFFD080),
            width: 2,
          ),
        ),
      ),
      child: Text(
        isHealthy
            ? '✓  Pipeline healthy — all metrics within target thresholds'
            : '⚠  Elevated latency or packet loss detected — check network',
        style: GoogleFonts.shareTechMono(
          color: isHealthy ? const Color(0xFF7FFF7F) : const Color(0xFFFFD080),
          fontSize: 11,
        ),
      ),
    );
  }
}