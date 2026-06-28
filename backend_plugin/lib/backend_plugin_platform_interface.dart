import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'backend_plugin_method_channel.dart';

abstract class BackendPluginPlatform extends PlatformInterface {
  /// Constructs a BackendPluginPlatform.
  BackendPluginPlatform() : super(token: _token);

  static final Object _token = Object();

  static BackendPluginPlatform _instance = MethodChannelBackendPlugin();

  /// The default instance of [BackendPluginPlatform] to use.
  ///
  /// Defaults to [MethodChannelBackendPlugin].
  static BackendPluginPlatform get instance => _instance;

  /// Platform-specific implementations should set this with their own
  /// platform-specific class that extends [BackendPluginPlatform] when
  /// they register themselves.
  static set instance(BackendPluginPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  Future<String?> getPlatformVersion() {
    throw UnimplementedError('platformVersion() has not been implemented.');
  }
}
