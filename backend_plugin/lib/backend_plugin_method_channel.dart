import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'backend_plugin_platform_interface.dart';

/// An implementation of [BackendPluginPlatform] that uses method channels.
class MethodChannelBackendPlugin extends BackendPluginPlatform {
  /// The method channel used to interact with the native platform.
  @visibleForTesting
  final methodChannel = const MethodChannel('backend_plugin');

  @override
  Future<String?> getPlatformVersion() async {
    final version = await methodChannel.invokeMethod<String>(
      'getPlatformVersion',
    );
    return version;
  }
}
