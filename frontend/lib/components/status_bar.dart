import 'package:flutter/material.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:layout_engine_plugin/layout_engine_plugin.dart';
import 'package:provider/provider.dart';

class StatusBar extends StatelessWidget {
  const StatusBar({super.key});

  // Below this width the tooltip no longer fits comfortably alongside Mode
  // and the coordinates, so it drops to its own row underneath them.
  static const double _narrowWidthCutoff = 1200;

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        final Widget mode = Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            SelectableText(
              "Mode: ${switch (provider.mode) {
                LeMode.LE_MODE_SELECT => 'Select',
                LeMode.LE_MODE_EDIT => 'Edit',
                LeMode.LE_MODE_RULER => 'Ruler',
              }}",
            ),
            // A small spinner while LeProvider is running any Tcl command
            // (BUGS_AND_ENHANCEMENTS.md item E3) - long-running commands no
            // longer block the UI, so there needs to be some indication
            // one is still in flight. Also shown while a frame is actually
            // rendering (BUGS_AND_ENHANCEMENTS.md item E17) - interactive
            // zoom/pan/etc. never go through a Tcl command at all, so
            // isRunning alone would miss them; isRendering is LeProvider's
            // own separate signal for that (see its own doc comment).
            if (provider.isRunning || provider.isRendering) ...[
              const SizedBox(width: 8),
              const SizedBox(
                width: 14,
                height: 14,
                child: CircularProgressIndicator(strokeWidth: 2),
              ),
            ],
          ],
        );
        final Widget tooltip = Expanded(
          child: SelectableText(
            provider.tooltipMessage,
            maxLines: 1,
            textAlign: TextAlign.center,
          ),
        );
        final Widget coordinates = SelectableText(
          "X: ${provider.snappedMousePosition.dx.toStringAsFixed(3)} Y: ${provider.snappedMousePosition.dy.toStringAsFixed(3)}",
        );
        final Widget selected = SelectableText(
          "Selected: ${provider.selectedCount}",
        );

        return Padding(
          padding: const EdgeInsets.all(8.0),
          child: LayoutBuilder(
            builder: (context, constraints) {
              final Widget modeAndCoordinates = Row(
                children: [
                  mode,
                  const Spacer(),
                  coordinates,
                  const SizedBox(width: 8),
                  selected,
                ],
              );

              if (constraints.maxWidth < _narrowWidthCutoff) {
                return Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    modeAndCoordinates,
                    const SizedBox(height: 4),
                    Row(children: [tooltip]),
                  ],
                );
              }

              return Row(
                children: [
                  mode,
                  const SizedBox(width: 8),
                  tooltip,
                  const SizedBox(width: 8),
                  coordinates,
                  const SizedBox(width: 8),
                  selected,
                ],
              );
            },
          ),
        );
      },
    );
  }
}
