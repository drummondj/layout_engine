import 'package:flutter/material.dart';
import 'package:hugeicons/hugeicons.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:layout_engine_plugin/layout_engine_plugin.dart';
import 'package:provider/provider.dart';

class ModeButton extends StatelessWidget {
  final String text;
  final String? shortcutKey;
  final dynamic icon;
  final VoidCallback onPressed;
  final bool selected;

  const ModeButton({
    super.key,
    required this.text,
    this.shortcutKey,
    required this.icon,
    required this.onPressed,
    required this.selected,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    final forgroundColor = selected
        ? colorScheme.tertiary
        : colorScheme.primary;
    final strokeWidth = selected ? 1.5 : 1.0;
    final finalText = shortcutKey != null ? "$text ($shortcutKey)" : text;

    return Container(
      decoration: selected
          ? BoxDecoration(
              color: Theme.of(context).colorScheme.surfaceContainerLow,
              borderRadius: .all(Radius.circular(9)),
            )
          : null,
      child: Padding(
        padding: EdgeInsets.all(8.0),
        child: Tooltip(
          message: finalText,
          verticalOffset: 30,
          child: TextButton(
            onPressed: selected ? null : onPressed,
            child: Column(
              children: [
                HugeIcon(
                  icon: icon,
                  color: forgroundColor,
                  size: 32,
                  strokeWidth: strokeWidth,
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class ModeSelector extends StatefulWidget {
  const ModeSelector({super.key});

  @override
  State<ModeSelector> createState() => _ModeSelectorState();
}

class _ModeSelectorState extends State<ModeSelector> {
  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        final mode = provider.mode;
        return Padding(
          padding: const .symmetric(vertical: 78, horizontal: 8),
          child: Column(
            spacing: 16,
            children: [
              ModeButton(
                text: "Select",
                shortcutKey: "s",
                icon: HugeIcons.strokeRoundedCursorRectangleSelection01,
                onPressed: () => provider.setMode(LeMode.LE_MODE_SELECT),
                selected: mode == LeMode.LE_MODE_SELECT,
              ),
              ModeButton(
                text: "Edit",
                shortcutKey: "e",
                icon: HugeIcons.strokeRoundedCursorEdit01,
                onPressed: () => provider.setMode(LeMode.LE_MODE_EDIT),
                selected: mode == LeMode.LE_MODE_EDIT,
              ),
              ModeButton(
                text: "Ruler",
                shortcutKey: "r",
                icon: HugeIcons.strokeRoundedRuler,
                onPressed: () => provider.setMode(LeMode.LE_MODE_RULER),
                selected: mode == LeMode.LE_MODE_RULER,
              ),
            ],
          ),
        );
      },
    );
  }
}
