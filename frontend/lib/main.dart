import 'dart:ffi' hide Size;
import 'dart:typed_data';

import 'package:flat_buffers/flat_buffers.dart';
import 'package:flutter/material.dart';
import 'package:backend_plugin/backend_plugin.dart';
import 'package:flutter/services.dart';
import 'package:frontend/database_le.database_generated.dart' as db;

void main() => runApp(const MyApp());

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(
      home: Scaffold(body: Center(child: LayoutEngineWidget())),
    );
  }
}

class LayoutEngineWidget extends StatefulWidget {
  const LayoutEngineWidget({super.key});

  @override
  State<LayoutEngineWidget> createState() => _LayoutEngineWidgetState();
}

class _LayoutEngineWidgetState extends State<LayoutEngineWidget>
    with WidgetsBindingObserver {
  Pointer<Void>? _layoutEngineId;
  int? _textureId;
  db.BrowserPayload? _browserInfo;
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
    await BackendPlugin.serializeLibraryBrowserData(id);
    final size = await BackendPlugin.getBrowserDataSize();
    final pointer = await BackendPlugin.getBrowserDataPtr();
    final Uint8List rawBytes = pointer.asTypedList(size);
    final browserInfo = db.BrowserPayload(rawBytes);
    debugPrint(browserInfo.toString());

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
      _browserInfo = browserInfo;
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

  List<TableRow>? _buildTableRows() {
    if (_browserInfo == null || _browserInfo!.libraries == null) {
      return null;
    }

    List<TableRow> rows = [];

    for (final libraryInfo in _browserInfo!.libraries!) {
      if (libraryInfo.designs == null) {
        continue;
      }
      for (final designInfo in libraryInfo.designs!) {
        if (designInfo.views == null) {
          continue;
        }
        for (final viewInfo in designInfo.views!) {
          rows.add(
            TableRow(
              children: [
                Text(libraryInfo.name ?? "unknown"),
                Text(designInfo.name ?? "unknown"),
                Text(viewInfo.name ?? "unknown"),
              ],
            ),
          );
        }
      }
    }

    return rows;
  }

  @override
  Widget build(BuildContext context) {
    if (_layoutEngineId == null) {
      return const CircularProgressIndicator(color: Colors.cyan);
    }

    final rows = _buildTableRows();

    return Column(
      children: [
        Row(
          children: [
            OutlinedButton.icon(
              onPressed: () => _zoom(1.1),
              label: Text("Zoom In"),
              icon: Icon(Icons.zoom_in),
            ),
            OutlinedButton.icon(
              onPressed: () => _zoom(0.9),
              label: Text("Zoom Out"),
              icon: Icon(Icons.zoom_out),
            ),
            OutlinedButton.icon(
              onPressed: _zoomFit,
              label: Text("Zoom Fit"),
              icon: Icon(Icons.fit_screen),
            ),
          ],
        ),
        if (_textureId != null)
          Expanded(
            child: LayoutBuilder(
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
                              switch (event.logicalKey) {
                                case LogicalKeyboardKey.arrowUp:
                                  _panUp();
                                  return KeyEventResult.handled;
                                case LogicalKeyboardKey.arrowDown:
                                  _panDown();
                                  return KeyEventResult.handled;
                                case LogicalKeyboardKey.arrowLeft:
                                  _panLeft();
                                  return KeyEventResult.handled;
                                case LogicalKeyboardKey.arrowRight:
                                  _panRight();
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
            ),
          )
        else
          Center(child: CircularProgressIndicator()),

        // Text("LayoutEngine id = $_layoutEngineId"),
        // if (rows != null) Table(children: rows),
      ],
    );
  }
}
