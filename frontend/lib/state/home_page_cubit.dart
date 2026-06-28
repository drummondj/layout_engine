import 'dart:ffi';

import 'package:flutter/material.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:frontend/data/backend_plugin_provider.dart';
import 'package:backend_plugin/backend_plugin.dart';

enum HomePageStatus { loading, loaded }

class HomePageState {
  final HomePageStatus status;
  final Pointer<Void>? layoutEngineId;
  final int? textureId;

  HomePageState({
    this.status = HomePageStatus.loading,
    this.layoutEngineId,
    this.textureId,
  });
}

class HomePageCubit extends Cubit<HomePageState> {
  final BackendPluginProvider backend;

  HomePageCubit({required this.backend}) : super(HomePageState());

  final double _panPixels = 10.0;

  Future<void> initialize() async {
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
    emit(
      HomePageState(status: .loaded, textureId: textureId, layoutEngineId: id),
    );
  }

  Future<void> requestResize(int width, int height) async {
    if (state.status != .loaded) {
      return;
    }

    BackendPlugin.resizeTexture(state.textureId!, width, height);
  }

  void zoom(double by) {
    if (state.status != .loaded) {
      return;
    }
    BackendPlugin.zoom(state.layoutEngineId!, by);
  }

  void zoomFit() {
    if (state.status != .loaded) {
      return;
    }
    BackendPlugin.zoomFit(state.layoutEngineId!);
  }

  void pan(Offset by) {
    if (state.status != .loaded) {
      return;
    }
    BackendPlugin.pan(state.layoutEngineId!, by.dx, by.dy);
  }

  void panUp() {
    pan(Offset(0, _panPixels));
  }

  void panDown() {
    pan(Offset(0, -_panPixels));
  }

  void panLeft() {
    pan(Offset(-_panPixels, 0));
  }

  void panRight() {
    pan(Offset(_panPixels, 0));
  }

  void dispose() {
    if (state.status == .loading) {
      return;
    }

    BackendPlugin.disposeTexture(state.textureId!);
    BackendPlugin.destroyLayoutEngine(state.layoutEngineId!);
  }
}
