import 'package:flutter/material.dart';
import 'package:animated_visibility/animated_visibility.dart';
import 'package:hugeicons/hugeicons.dart';

enum ToolbarDesintation { zoomIn, zoomOut, zoomFit }

class Toolbar extends StatefulWidget {
  final Function(ToolbarDesintation) onTap;
  final double iconSize;
  final Color iconColor;

  const Toolbar({
    super.key,
    required this.onTap,
    this.iconSize = 30.0,
    this.iconColor = Colors.white,
  });

  @override
  State<Toolbar> createState() => _ToolbarState();
}

class _ToolbarState extends State<Toolbar> {
  bool hidden = false;
  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(8.0),
      child: Container(
        decoration: BoxDecoration(
          color: Colors.transparent,
          border: BoxBorder.all(color: Colors.white38),
          borderRadius: BorderRadius.only(
            topLeft: Radius.circular(32),
            topRight: Radius.circular(32),
            bottomLeft: Radius.circular(32),
            bottomRight: Radius.circular(32),
          ),
          boxShadow: [BoxShadow(color: Colors.black54, blurRadius: 10)],
        ),
        child: Padding(
          padding: const EdgeInsets.all(8.0),
          child: Column(
            mainAxisSize: .min,
            children: [
              Tooltip(
                message: hidden ? "Show menu" : "Hide menu",
                child: IconButton(
                  onPressed: () {
                    setState(() {
                      hidden = !hidden;
                    });
                  },
                  icon: AnimatedRotation(
                    turns: hidden ? 0.5 : 0.0,
                    duration: const Duration(milliseconds: 200),
                    child: HugeIcon(
                      icon: HugeIcons.strokeRoundedCircleArrowUp01,
                      size: widget.iconSize,
                      color: widget.iconColor,
                    ),
                  ),
                  iconSize: widget.iconSize,
                  color: widget.iconColor,
                ),
              ),
              AnimatedVisibility(
                visible: !hidden,
                enter: slideInVertically(initialOffsetY: -1) + fadeIn(),
                exit: slideOutVertically(targetOffsetY: -1) + fadeOut(),
                enterDuration: Duration(milliseconds: 200),
                exitDuration: Duration(milliseconds: 200),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Tooltip(
                          message: "Zoom In (z)",
                          child: IconButton(
                            onPressed: () =>
                                widget.onTap(ToolbarDesintation.zoomIn),
                            icon: HugeIcon(
                              icon: HugeIcons.strokeRoundedSearchAdd,
                              size: widget.iconSize,
                              color: widget.iconColor,
                            ),
                            iconSize: widget.iconSize,
                            color: widget.iconColor,
                          ),
                        ),
                        Tooltip(
                          message: "Zoom Out (Shift+z)",
                          child: IconButton(
                            onPressed: () =>
                                widget.onTap(ToolbarDesintation.zoomOut),
                            icon: HugeIcon(
                              icon: HugeIcons.strokeRoundedSearchMinus,
                              size: widget.iconSize,
                              color: widget.iconColor,
                            ),
                            iconSize: widget.iconSize,
                            color: widget.iconColor,
                          ),
                        ),
                        Tooltip(
                          message: "Fit (f)",
                          child: IconButton(
                            onPressed: () =>
                                widget.onTap(ToolbarDesintation.zoomFit),
                            icon: HugeIcon(
                              icon: HugeIcons.strokeRoundedFitToScreen,
                              size: widget.iconSize,
                              color: widget.iconColor,
                            ),
                            iconSize: widget.iconSize,
                            color: widget.iconColor,
                          ),
                        ),
                      ],
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
