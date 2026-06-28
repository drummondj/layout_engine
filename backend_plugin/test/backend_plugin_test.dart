import 'package:flutter_test/flutter_test.dart';
import 'package:backend_plugin/backend_plugin.dart';
import 'package:backend_plugin/backend_plugin_platform_interface.dart';
import 'package:backend_plugin/backend_plugin_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class MockBackendPluginPlatform
    with MockPlatformInterfaceMixin
    implements BackendPluginPlatform {
  @override
  Future<String?> getPlatformVersion() => Future.value('42');
}

void main() {
  final BackendPluginPlatform initialPlatform = BackendPluginPlatform.instance;

  test('$MethodChannelBackendPlugin is the default instance', () {
    expect(initialPlatform, isInstanceOf<MethodChannelBackendPlugin>());
  });

  test('getPlatformVersion', () async {
    BackendPlugin backendPlugin = BackendPlugin();
    MockBackendPluginPlatform fakePlatform = MockBackendPluginPlatform();
    BackendPluginPlatform.instance = fakePlatform;

    expect(await backendPlugin.getPlatformVersion(), '42');
  });
}
