import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';

class LatencyChart extends StatelessWidget {
  final List<double> data;
  final double maxY;

  const LatencyChart({super.key, required this.data, this.maxY = 50});

  @override
  Widget build(BuildContext context) {
    if (data.isEmpty) {
      return Center(
        child: Text(
          'Awaiting packets...',
          style: GoogleFonts.shareTechMono(
            color: const Color(0xFF3A4A5C),
            fontSize: 13,
          ),
        ),
      );
    }

    final spots = List.generate(
      data.length,
      (i) => FlSpot(i.toDouble(), data[i]),
    );

    return LineChart(
      LineChartData(
        minX: 0,
        maxX: (data.length - 1).toDouble().clamp(1, 60),
        minY: 0,
        maxY: maxY,
        clipData: const FlClipData.all(),
        backgroundColor: Colors.transparent,
        gridData: FlGridData(
          show: true,
          drawVerticalLine: false,
          horizontalInterval: maxY / 5,
          getDrawingHorizontalLine: (_) => FlLine(
            color: const Color(0xFF1A2535),
            strokeWidth: 1,
          ),
        ),
        borderData: FlBorderData(show: false),
        titlesData: FlTitlesData(
          leftTitles: AxisTitles(
            sideTitles: SideTitles(
              showTitles: true,
              reservedSize: 36,
              interval: maxY / 5,
              getTitlesWidget: (v, _) => Text(
                '${v.toInt()}',
                style: GoogleFonts.shareTechMono(
                  color: const Color(0xFF3A4A5C),
                  fontSize: 10,
                ),
              ),
            ),
          ),
          bottomTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
          topTitles:    const AxisTitles(sideTitles: SideTitles(showTitles: false)),
          rightTitles:  const AxisTitles(sideTitles: SideTitles(showTitles: false)),
        ),
        lineBarsData: [
          // Fill area
          LineChartBarData(
            spots: spots,
            isCurved: true,
            curveSmoothness: 0.3,
            color: Colors.transparent,
            belowBarData: BarAreaData(
              show: true,
              gradient: LinearGradient(
                begin: Alignment.topCenter,
                end: Alignment.bottomCenter,
                colors: [
                  const Color(0xFF00D4FF).withOpacity(0.15),
                  const Color(0xFF00D4FF).withOpacity(0.0),
                ],
              ),
            ),
            dotData: const FlDotData(show: false),
            barWidth: 0,
          ),
          // Line
          LineChartBarData(
            spots: spots,
            isCurved: true,
            curveSmoothness: 0.3,
            color: const Color(0xFF00D4FF),
            barWidth: 1.5,
            dotData: const FlDotData(show: false),
            belowBarData: BarAreaData(show: false),
          ),
        ],
        lineTouchData: LineTouchData(
          touchTooltipData: LineTouchTooltipData(
            getTooltipItems: (spots) => spots.map((s) => LineTooltipItem(
              '${s.y.toStringAsFixed(2)}ms',
              GoogleFonts.shareTechMono(
                color: const Color(0xFF00D4FF),
                fontSize: 11,
              ),
            )).toList(),
          ),
        ),
      ),
      duration: const Duration(milliseconds: 0),
    );
  }
}
