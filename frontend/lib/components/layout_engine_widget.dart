import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:frontend/components/toolbar.dart';
import 'package:frontend/state/home_page_cubit.dart';

class LayoutEngineWidget extends StatefulWidget {
  const LayoutEngineWidget({super.key});

  @override
  State<LayoutEngineWidget> createState() => _LayoutEngineWidgetState();
}

class _LayoutEngineWidgetState extends State<LayoutEngineWidget>
    with WidgetsBindingObserver {
  late HomePageCubit cubit;

  Size _lastRequestedSize = Size.zero;
  bool _resizeScheduled = false;
  final FocusNode _canvasFocusNode = FocusNode();

  @override
  void initState() {
    super.initState();
    cubit = context.read();

    WidgetsBinding.instance.addObserver(this);
    _canvasFocusNode.skipTraversal = true;
    cubit.initialize().then((_) => _scheduleResize());
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
      if (!mounted) return;

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
        cubit.requestResize(target.width.toInt(), target.height.toInt());
      }
    });
  }

  void _onToolbarTap(ToolbarDesintation destination) {
    switch (destination) {
      case ToolbarDesintation.zoomIn:
        cubit.zoom(1.1);
      case ToolbarDesintation.zoomOut:
        cubit.zoom(0.9);
      case ToolbarDesintation.zoomFit:
        cubit.zoomFit();
    }
  }

  @override
  void dispose() {
    cubit.dispose();
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return BlocBuilder<HomePageCubit, HomePageState>(
      builder: (context, state) {
        switch (state.status) {
          case HomePageStatus.loading:
            return const CircularProgressIndicator();
          case HomePageStatus.loaded:
            return Stack(
              children: [
                if (state.textureId != null)
                  LayoutBuilder(
                    builder: (context, constraints) {
                      return SizedBox.expand(
                        child: state.textureId == null
                            ? const SizedBox.shrink()
                            : Focus(
                                focusNode: _canvasFocusNode,
                                autofocus: true, // Grab focus immediately
                                onKeyEvent: (FocusNode node, KeyEvent event) {
                                  if (event is KeyDownEvent ||
                                      event is KeyRepeatEvent) {
                                    if (event.logicalKey
                                        case LogicalKeyboardKey.arrowUp) {
                                      cubit.panUp();
                                      return KeyEventResult.handled;
                                    } else if (event.logicalKey
                                        case LogicalKeyboardKey.arrowDown) {
                                      cubit.panDown();
                                      return KeyEventResult.handled;
                                    } else if (event.logicalKey
                                        case LogicalKeyboardKey.arrowLeft) {
                                      cubit.panLeft();
                                      return KeyEventResult.handled;
                                    } else if (event.logicalKey
                                        case LogicalKeyboardKey.arrowRight) {
                                      cubit.panRight();
                                      return KeyEventResult.handled;
                                    }
                                    if (event.character case "f") {
                                      cubit.zoomFit();
                                      return KeyEventResult.handled;
                                    } else if (event.character case "Z") {
                                      cubit.zoom(0.9);
                                      return KeyEventResult.handled;
                                    } else if (event.character case "z") {
                                      cubit.zoom(1.1);
                                      return KeyEventResult.handled;
                                    }
                                  }
                                  return KeyEventResult.ignored;
                                },
                                child: Texture(
                                  textureId: state.textureId!,
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
      },
    );
  }
}
