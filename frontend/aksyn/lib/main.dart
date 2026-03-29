import 'package:aksyn_monitor/struct/screens/dashboard_screen.dart';
import 'package:aksyn_monitor/struct/screens/recordings_screen.dart';
import 'package:aksyn_monitor/struct/services/audio_stream_service.dart';
import 'package:aksyn_monitor/struct/services/recordings_service.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:provider/provider.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  SystemChrome.setPreferredOrientations([
    DeviceOrientation.portraitUp,
    DeviceOrientation.landscapeLeft,
    DeviceOrientation.landscapeRight,
  ]);
  SystemChrome.setSystemUIOverlayStyle(const SystemUiOverlayStyle(
    statusBarColor: Colors.transparent,
    statusBarIconBrightness: Brightness.light,
  ));
  runApp(const AksynMonitorApp());
}

class AksynMonitorApp extends StatelessWidget {
  const AksynMonitorApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MultiProvider(
      providers: [
        ChangeNotifierProvider(create: (_) => AudioStreamService()),
        ChangeNotifierProvider(create: (_) => RecordingsService()),
      ],
      child: MaterialApp(
        title: 'Aksyn Monitor',
        debugShowCheckedModeBanner: false,
        theme: ThemeData(
          brightness: Brightness.dark,
          scaffoldBackgroundColor: const Color(0xFF0A0C10),
          colorScheme: const ColorScheme.dark(
            primary: Color(0xFF00D4FF),
            surface: Color(0xFF111318),
          ),
        ),
        home: const _AppShell(),
      ),
    );
  }
}

class _AppShell extends StatefulWidget {
  const _AppShell();

  @override
  State<_AppShell> createState() => _AppShellState();
}

class _AppShellState extends State<_AppShell> {
  int _tab = 0;

  static const _screens = [
    DashboardScreen(),
    RecordingsScreen(),
  ];

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF0A0C10),
      body: IndexedStack(index: _tab, children: _screens),
      bottomNavigationBar: Container(
        decoration: const BoxDecoration(
          color: Color(0xFF111318),
          border: Border(top: BorderSide(color: Color(0xFF222733))),
        ),
        child: Row(children: [
          _NavItem(
            icon: Icons.graphic_eq,
            label: 'MONITOR',
            active: _tab == 0,
            onTap: () => setState(() => _tab = 0),
          ),
          _NavItem(
            icon: Icons.album,
            label: 'RECORDINGS',
            active: _tab == 1,
            onTap: () => setState(() => _tab = 1),
          ),
        ]),
      ),
    );
  }
}

class _NavItem extends StatelessWidget {
  final IconData icon;
  final String label;
  final bool active;
  final VoidCallback onTap;

  const _NavItem({
    required this.icon,
    required this.label,
    required this.active,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    final color = active ? const Color(0xFF00D4FF) : const Color(0xFF3A4A5C);
    return Expanded(
      child: GestureDetector(
        onTap: onTap,
        behavior: HitTestBehavior.opaque,
        child: Container(
          padding: const EdgeInsets.symmetric(vertical: 12),
          decoration: BoxDecoration(
            border: Border(
              top: BorderSide(
                color: active ? const Color(0xFF00D4FF) : Colors.transparent,
                width: 1.5,
              ),
            ),
          ),
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            Icon(icon, color: color, size: 18),
            const SizedBox(height: 4),
            Text(label,
              style: GoogleFonts.shareTechMono(
                color: color, fontSize: 8, letterSpacing: 1.5)),
          ]),
        ),
      ),
    );
  }
}