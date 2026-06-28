import 'package:flutter/material.dart';
import 'package:frontend/components/layout_engine_widget.dart';

class HomePage extends StatelessWidget {
  const HomePage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text("LAYOUT ENGINE"), primary: false),
      body: Padding(
        padding: const EdgeInsets.all(8.0),
        child: LayoutEngineWidget(),
      ),
    );
  }
}
