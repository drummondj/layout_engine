import 'package:flutter/material.dart';

class WidgetCard extends StatelessWidget {
  final Widget child;

  const WidgetCard({super.key, required this.child});

  @override
  Widget build(BuildContext context) {
    return Container(
      color: Theme.of(context).colorScheme.surfaceContainer,
      child: child,
    );
  }
}
