import 'dart:async';

import 'package:file_selector/file_selector.dart';
import 'package:flutter/material.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:provider/provider.dart';

class MainMenu extends StatefulWidget {
  const MainMenu({super.key});

  @override
  State<MainMenu> createState() => _MainMenuState();
}

class _MainMenuState extends State<MainMenu> {
  Future<void> _openFilePicker() async {
    const typeGroup = XTypeGroup(label: 'LEF files', extensions: ['lef']);
    final files = await openFiles(acceptedTypeGroups: [typeGroup]);
    if (files.isEmpty || !mounted) return;
    final provider = context.read<LeProvider>();
    for (final file in files) {
      unawaited(provider.readLef(file.path));
    }
  }

  void _loadTestData() {
    var provider = context.read<LeProvider>();
    provider.readLef(
      "/Volumes/Docking/Projects/synthosilicon/layout_engine/test_data/Nangate45/Nangate45_tech.lef",
    );
    provider.readLef(
      "/Volumes/Docking/Projects/synthosilicon/layout_engine/test_data/Nangate45/Nangate45_stdcell.lef",
    );
    provider.readLef(
      "/Volumes/Docking/Projects/synthosilicon/layout_engine/test_data/Nangate45/fakeram45_1024x32.lef",
    );
  }

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(8.0),
      child: Column(
        crossAxisAlignment: .start,
        spacing: 8,
        children: [
          TextButton.icon(
            icon: Icon(Icons.folder_open),
            onPressed: _openFilePicker,
            label: Text("Import LEF ..."),
          ),
          TextButton.icon(
            icon: Icon(Icons.my_library_books_sharp),
            onPressed: _loadTestData,
            label: Text("Load test data"),
          ),
        ],
      ),
    );
  }
}
