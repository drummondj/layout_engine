import 'dart:ffi' as ffi;
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'backend_plugin_platform_interface.dart';
import 'layout_engine_bindings.g.dart' as bindings;

class BackendPlugin {
  Future<String?> getPlatformVersion() {
    return BackendPluginPlatform.instance.getPlatformVersion();
  }

  // Define a unique channel identifier matching your platform plugin
  static const MethodChannel _channel = MethodChannel(
    'com.synthosilicon.layout_engine/channel',
  );

  static Future<ffi.Pointer<ffi.Void>> initLayoutEngine() async {
    return bindings.init_layout_engine();
  }

  static Future<void> generateTestData(
    ffi.Pointer<ffi.Void> layoutEngineId,
  ) async {
    bindings.generate_test_data(layoutEngineId);
  }

  static Future<void> zoom(
    ffi.Pointer<ffi.Void> layoutEngineId,
    double zoom,
  ) async {
    bindings.zoom(layoutEngineId, zoom);
  }

  static Future<void> zoomFit(ffi.Pointer<ffi.Void> layoutEngineId) async {
    bindings.zoom_fit(layoutEngineId);
  }

  static Future<void> pan(
    ffi.Pointer<ffi.Void> layoutEngineId,
    double x,
    double y,
  ) async {
    bindings.pan(layoutEngineId, x, y);
  }

  static Future<void> serializeLibraryBrowserData(
    ffi.Pointer<ffi.Void> layoutEngineId,
  ) async {
    bindings.serialize_library_browser_data(layoutEngineId);
  }

  static Future<int> getBrowserDataSize() async {
    return bindings.get_browser_data_size();
  }

  static Future<ffi.Pointer<ffi.Uint8>> getBrowserDataPtr() async {
    return bindings.get_browser_data_ptr();
  }

  static Future<void> destroyLayoutEngine(
    ffi.Pointer<ffi.Void> layoutEngineId,
  ) async {
    bindings.destroy(layoutEngineId);
  }

  /// Requests the native side to allocate a texture and start a 60FPS loop.
  /// Returns the unique Texture ID required by the Flutter Texture widget.
  static Future<int> createTexture(
    ffi.Pointer<ffi.Void> layoutEngine, { // This was the positional argument
    required int width,
    int height = 400,
  }) async {
    try {
      debugPrint("${layoutEngine.address}, $width, $height");
      final int? textureId = await _channel.invokeMethod<int>('createTexture', {
        'handle': layoutEngine.address,
        'width': width,
        'height': height,
      });
      if (textureId == null) {
        throw PlatformException(
          code: 'NULL_ID',
          message: 'Failed to generate a Texture ID.',
        );
      }
      return textureId;
    } on PlatformException catch (e) {
      print("Failed to initialize texture canvas: ${e.message}");
      rethrow;
    }
  }

  static Future<void> resizeTexture(
    int textureId,
    int width,
    int height,
  ) async {
    try {
      await _channel.invokeMethod('resizeTexture', {
        'textureId': textureId,
        'width': width,
        'height': height,
      });
    } on PlatformException catch (e) {
      print("Failed to resize texture: ${e.message}");
    }
  }

  /// Optional: Tell the native side to freeze or kill the rendering loop
  static Future<void> disposeTexture(int textureId) async {
    await _channel.invokeMethod('disposeTexture', {'textureId': textureId});
  }
}
