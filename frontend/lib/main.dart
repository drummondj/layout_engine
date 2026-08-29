import 'package:flutter/material.dart';
import 'package:layout_engine/pages/home.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:provider/provider.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  /// Overrides the real (production) [LeProvider] this widget would
  /// otherwise construct itself - for tests only, so a widget test can
  /// inject one built with a fake [LeEditorBase] (see
  /// test/fakes/fake_le_editor.dart) instead of the real [LeEditor],
  /// whose constructor isn't safe to call outside a real built app (see
  /// LeEditorBase's own doc comment).
  const MyApp({super.key, this.provider, this.initiallyMaximizedItemId});

  final LeProvider? provider;

  /// Forwarded to [Home] - see its own doc comment. `null` (the real
  /// app's own default, via `runApp(const MyApp())` in this file's
  /// `main()`) leaves Home's own default docking layout untouched.
  final String? initiallyMaximizedItemId;

  @override
  Widget build(BuildContext context) {
    return MultiProvider(
      providers: [
        provider != null
            ? ChangeNotifierProvider<LeProvider>.value(value: provider!)
            : ChangeNotifierProvider(create: (context) => LeProvider()),
      ],
      child: MaterialApp(
        title: 'Layout Engine',
        home: Home(initiallyMaximizedItemId: initiallyMaximizedItemId),
        debugShowCheckedModeBanner: false,
        themeMode: .dark,
        darkTheme: ThemeData(
          fontFamily: 'JetBrains Mono',
          colorScheme: ColorScheme(
            brightness: .dark,
            primary: const Color.fromARGB(255, 220, 220, 220),
            onPrimary: Colors.black,
            secondary: Colors.orange,
            onSecondary: Colors.black,
            tertiary: Colors.lightBlueAccent,
            onTertiary: Colors.black,
            error: Colors.red,
            onError: Colors.black,
            surface: Color.fromARGB(255, 30, 30, 30),
            onSurface: Color.fromARGB(255, 220, 220, 220),
            surfaceDim: Color.fromARGB(255, 0, 0, 0),
            surfaceContainer: Color.fromARGB(255, 10, 10, 10),
            surfaceContainerHighest: Color.fromARGB(255, 50, 50, 50),
          ),
        ),
        theme: ThemeData(brightness: .light, fontFamily: 'JetBrains Mono'),
      ),
    );
  }
}
