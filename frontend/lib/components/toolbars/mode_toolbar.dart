import 'package:flutter/material.dart';
import 'package:hugeicons/hugeicons.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:layout_engine_plugin/layout_engine_plugin.dart';
import 'package:provider/provider.dart';

class ToolbarButton extends StatelessWidget {
  final String text;
  final String? shortcutKey;
  final dynamic icon;
  final VoidCallback onPressed;
  final bool selected;

  const ToolbarButton({
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
    final finalText = shortcutKey != null ? "$text\n($shortcutKey)" : "$text\n";

    return Container(
      decoration: selected
          ? BoxDecoration(
              border: .fromLTRB(
                top: BorderSide(color: forgroundColor, width: 4.0),
              ),
              gradient: LinearGradient(
                colors: [forgroundColor.withAlpha(25), Colors.transparent],
                begin: .topCenter,
                end: .bottomCenter,
              ),
            )
          : BoxDecoration(
              border: .fromLTRB(
                top: BorderSide(color: Colors.transparent, width: 4.0),
              ),
            ),
      child: Padding(
        padding: EdgeInsets.all(4),
        child: Tooltip(
          message: finalText,
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

class ModeToolbar extends StatelessWidget {
  const ModeToolbar({super.key});

  List<Widget> _getModeMenuItems(BuildContext context, LeMode mode) {
    final provider = context.read<LeProvider>();
    return switch (mode) {
      .LE_MODE_SELECT => [
        ToolbarButton(
          icon: HugeIcons.strokeRoundedBoundingBox,
          text: 'Select all',
          shortcutKey: "ctrl-a",
          onPressed: () => provider.selectAll(),
          selected: false,
        ),
        ToolbarButton(
          icon: HugeIcons.strokeRoundedCancel02,
          text: 'De-select all',
          shortcutKey: "ctrl-d",
          onPressed: () => provider.deselectAll(),
          selected: false,
        ),
      ],
      .LE_MODE_EDIT => [
        ToolbarButton(
          icon: HugeIcons.strokeRoundedArrowAllDirection,
          text: 'Move',
          shortcutKey: "ctrl-m",
          onPressed: () => provider.armMove(),
          selected: provider.moveArmed,
        ),
        ToolbarButton(
          icon: HugeIcons.strokeRoundedMaximizeScreen,
          text: 'Resize',
          shortcutKey: "ctrl-r",
          onPressed: () => {},
          selected: false,
        ),
        ToolbarButton(
          icon: HugeIcons.strokeRoundedRotateCrop,
          text: 'Rotate',
          shortcutKey: "ctrl-o",
          onPressed: () => {},
          selected: false,
        ),
        ToolbarButton(
          icon: HugeIcons.strokeRoundedAlignBottom,
          text: 'Align Bottom',
          shortcutKey: "ctrl-b",
          onPressed: () => {},
          selected: false,
        ),
        ToolbarButton(
          icon: HugeIcons.strokeRoundedAlignVerticalCenter,
          text: 'Align Vertcial Center',
          shortcutKey: "ctrl-v",
          onPressed: () => {},
          selected: false,
        ),
        ToolbarButton(
          icon: HugeIcons.strokeRoundedAlignTop,
          text: 'Align Top',
          shortcutKey: "ctrl-t",
          onPressed: () => {},
          selected: false,
        ),
        ToolbarButton(
          icon: HugeIcons.strokeRoundedAlignLeft,
          text: 'Align Left',
          shortcutKey: "ctrl-l",
          onPressed: () => {},
          selected: false,
        ),
        ToolbarButton(
          icon: HugeIcons.strokeRoundedAlignHorizontalCenter,
          text: 'Align Horizontal Center',
          shortcutKey: "ctrl-h",
          onPressed: () => {},
          selected: false,
        ),
        ToolbarButton(
          icon: HugeIcons.strokeRoundedAlignRight,
          text: 'Align Right',
          shortcutKey: "ctrl-w",
          onPressed: () => {},
          selected: false,
        ),
        ToolbarButton(
          icon: HugeIcons.strokeRoundedDelete04,
          shortcutKey: "ctrl-d",
          text: 'Delete',
          onPressed: () => {},
          selected: false,
        ),
        ToolbarButton(
          icon: HugeIcons.strokeRoundedUndo03,
          shortcutKey: "ctrl-z",
          text: 'Undo',
          onPressed: () => provider.undo(),
          selected: false,
        ),
        ToolbarButton(
          icon: HugeIcons.strokeRoundedRedo03,
          shortcutKey: "shift-ctrl-z",
          text: 'Redo',
          onPressed: () => provider.redo(),
          selected: false,
        ),
      ],
      .LE_MODE_RULER => [
        ToolbarButton(
          icon: HugeIcons.strokeRoundedCancel02,
          text: 'Clear rulers',
          onPressed: () => provider.clearRulers(),
          selected: false,
        ),
      ],
    };
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        final mode = provider.mode;
        return Padding(
          padding: const EdgeInsets.symmetric(vertical: 16.0),
          child: Wrap(
            spacing: 4,
            runSpacing: 4,
            direction: .horizontal,
            children: [..._getModeMenuItems(context, mode)],
          ),
        );
      },
    );
  }
}
