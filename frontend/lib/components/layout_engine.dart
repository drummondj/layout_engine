import 'package:flutter/widgets.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:provider/provider.dart';

class LayoutEngine extends StatefulWidget {
  const LayoutEngine({super.key});

  @override
  State<LayoutEngine> createState() => _LayoutEngineState();
}

class _LayoutEngineState extends State<LayoutEngine> {
  Size? _lastViewportSize;
  final FocusNode _textureFocusNode = FocusNode();

  @override
  void initState() {
    super.initState();
    context.read<LeProvider>().init();
    _textureFocusNode.skipTraversal = true;
  }

  void _handleViewportResize(BoxConstraints constraints) {
    final size = Size(constraints.maxWidth, constraints.maxHeight);
    if (size == _lastViewportSize) return;
    _lastViewportSize = size;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) context.read<LeProvider>().resize(size);
    });
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) => LayoutBuilder(
        builder: (context, constraints) {
          _handleViewportResize(constraints);
          return Focus(
            focusNode: _textureFocusNode,
            onFocusChange: provider.handleFocusChange,
            onKeyEvent: (node, value) {
              return provider.handleKeyEvent(value)
                  ? KeyEventResult.handled
                  : KeyEventResult.ignored;
            },
            child: MouseRegion(
              onEnter: (event) {
                _textureFocusNode.requestFocus();
              },
              child: Listener(
                onPointerDown: provider.handlePointerEvent,
                onPointerMove: provider.handlePointerEvent,
                onPointerHover: provider.handlePointerEvent,
                onPointerUp: provider.handlePointerEvent,
                onPointerCancel: provider.handlePointerEvent,
                onPointerSignal: provider.handlePointerSignal,
                child: SizedBox(
                  width: constraints.maxWidth,
                  height: constraints.maxHeight,

                  child: Container(
                    color: Color.fromARGB(255, 0, 15, 14),
                    child: provider.texture != null
                        ? Texture(textureId: provider.texture!.textureId)
                        : null,
                  ),
                ),
              ),
            ),
          );
        },
      ),
    );
  }
}
