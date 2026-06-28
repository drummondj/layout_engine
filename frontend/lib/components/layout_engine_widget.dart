import 'dart:ffi' hide Size;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:backend_plugin/backend_plugin.dart';
import 'package:frontend/components/toolbar.dart';

class LayoutEngineWidget extends StatefulWidget {
  const LayoutEngineWidget({super.key});

  @override
  State<LayoutEngineWidget> createState() => _LayoutEngineWidgetState();
}

class _LayoutEngineWidgetState extends State<LayoutEngineWidget>
    with WidgetsBindingObserver {
  Pointer<Void>? _layoutEngineId;
  int? _textureId;
  Size _lastRequestedSize = Size.zero;
  bool _resizeScheduled = false;
  final FocusNode _canvasFocusNode = FocusNode();

  @override
  void initState() {
    super.initState();
    _initLayoutEngine();
    WidgetsBinding.instance.addObserver(this);
    _canvasFocusNode.skipTraversal = true;
  }

  Future<void> _initLayoutEngine() async {
    final id = await BackendPlugin.initLayoutEngine();
    await BackendPlugin.generateTestData(id);

    // Now that the engine is ready, create the texture
    const initialWidth = 400.0;
    const initialHeight = 400.0;
    final textureId = await BackendPlugin.createTexture(
      id,
      width: initialWidth.toInt(),
      height: initialHeight.toInt(),
    );
    setState(() {
      _textureId = textureId;
      _layoutEngineId = id;
    });
    _scheduleResize();
  }

  @override
  void didChangeMetrics() {
    _scheduleResize();
  }

  void _scheduleResize() {
    if (_resizeScheduled || !mounted) return;
    _resizeScheduled = true;

    WidgetsBinding.instance.addPostFrameCallback((_) {
      _resizeScheduled = false;
      if (!mounted || _textureId == null) return;

      final renderBox = context.findRenderObject() as RenderBox?;
      if (renderBox == null || !renderBox.hasSize) return;

      final size = renderBox.size;
      final pixelRatio = View.of(context).devicePixelRatio;

      final target = Size(
        (size.width * pixelRatio).roundToDouble(),
        (size.height * pixelRatio).roundToDouble(),
      );

      if (target != _lastRequestedSize) {
        _lastRequestedSize = target;
        BackendPlugin.resizeTexture(
          _textureId!,
          target.width.toInt(),
          target.height.toInt(),
        );
      }
    });
  }

  void _zoom(double by) {
    if (_layoutEngineId != null) {
      BackendPlugin.zoom(_layoutEngineId!, by);
    }
  }

  void _zoomFit() {
    if (_layoutEngineId != null) {
      BackendPlugin.zoomFit(_layoutEngineId!);
    }
  }

  void _pan(Offset by) {
    if (_layoutEngineId != null) {
      BackendPlugin.pan(_layoutEngineId!, by.dx, by.dy);
    }
  }

  void _panUp() {
    _pan(Offset(0, 10));
  }

  void _panDown() {
    _pan(Offset(0, -10));
  }

  void _panLeft() {
    _pan(Offset(-10, 0));
  }

  void _panRight() {
    _pan(Offset(10, 0));
  }

  void _onToolbarTap(ToolbarDesintation destination) {
    switch (destination) {
      case ToolbarDesintation.zoomIn:
        _zoom(1.1);
      case ToolbarDesintation.zoomOut:
        _zoom(0.9);
      case ToolbarDesintation.zoomFit:
        _zoomFit();
    }
  }

  @override
  void dispose() {
    if (_layoutEngineId != null) {
      BackendPlugin.destroyLayoutEngine(_layoutEngineId!);
    }
    if (_textureId != null) {
      BackendPlugin.disposeTexture(_textureId!);
    }
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_layoutEngineId == null) {
      return const CircularProgressIndicator();
    }
    return Stack(
      children: [
        if (_textureId != null)
          LayoutBuilder(
            builder: (context, constraints) {
              return SizedBox.expand(
                child: _textureId == null
                    ? const SizedBox.shrink()
                    : Focus(
                        focusNode: _canvasFocusNode,
                        autofocus: true, // Grab focus immediately
                        onKeyEvent: (FocusNode node, KeyEvent event) {
                          if (event is KeyDownEvent ||
                              event is KeyRepeatEvent) {
                            if (event.logicalKey
                                case LogicalKeyboardKey.arrowUp) {
                              _panUp();
                              return KeyEventResult.handled;
                            } else if (event.logicalKey
                                case LogicalKeyboardKey.arrowDown) {
                              _panDown();
                              return KeyEventResult.handled;
                            } else if (event.logicalKey
                                case LogicalKeyboardKey.arrowLeft) {
                              _panLeft();
                              return KeyEventResult.handled;
                            } else if (event.logicalKey
                                case LogicalKeyboardKey.arrowRight) {
                              _panRight();
                              return KeyEventResult.handled;
                            }
                            if (event.character case "f") {
                              _zoomFit();
                              return KeyEventResult.handled;
                            } else if (event.character case "Z") {
                              _zoom(0.9);
                              return KeyEventResult.handled;
                            } else if (event.character case "z") {
                              _zoom(1.1);
                              return KeyEventResult.handled;
                            }
                          }
                          return KeyEventResult.ignored;
                        },
                        child: Texture(
                          textureId: _textureId!,
                          filterQuality: FilterQuality.high,
                        ),
                      ),
              );
            },
          )
        else
          Center(child: CircularProgressIndicator()),
        Toolbar(onTap: _onToolbarTap),
      ],
    );
  }
}
