import 'package:flutter/material.dart';
import 'package:frontend/config/dependencies.dart';
import 'package:frontend/pages/home_page.dart';

void main() => runApp(const MyApp());

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return wrapWithProviders(
      MaterialApp(
        home: HomePage(),
        themeMode: ThemeMode.dark,
        darkTheme: ThemeData.dark(),
      ),
    );
  }
}
