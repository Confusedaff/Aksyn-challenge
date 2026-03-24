import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';

class MetricCard extends StatelessWidget {
  final String label;
  final String value;
  final String unit;
  final Color accentColor;
  final bool isAlert; // turns value red if true

  const MetricCard({
    super.key,
    required this.label,
    required this.value,
    required this.unit,
    this.accentColor = const Color(0xFF00D4FF),
    this.isAlert = false,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: const Color(0xFF111318),
        border: Border(
          left: BorderSide(color: accentColor, width: 2),
        ),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisSize: MainAxisSize.min,
        children: [
          Text(
            label.toUpperCase(),
            style: GoogleFonts.shareTechMono(
              color: const Color(0xFF3A4A5C),
              fontSize: 9,
              letterSpacing: 2,
            ),
          ),
          const SizedBox(height: 8),
          Row(
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: [
              Text(
                value,
                style: GoogleFonts.shareTechMono(
                  color: isAlert
                      ? const Color(0xFFFF6B35)
                      : const Color(0xFFEEF2FF),
                  fontSize: 26,
                  fontWeight: FontWeight.w600,
                  height: 1,
                ),
              ),
              const SizedBox(width: 4),
              Text(
                unit,
                style: GoogleFonts.shareTechMono(
                  color: const Color(0xFF3A4A5C),
                  fontSize: 11,
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}
