import 'package:aksyn_monitor/struct/screens/dashboard_screen.dart';
import 'package:aksyn_monitor/struct/services/audio_stream_service.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  // Lock to portrait on mobile; allow all on desktop/tablet
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
    return ChangeNotifierProvider(
      create: (_) => AudioStreamService(),
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
        home: const DashboardScreen(),
      ),
    );
  }
}
