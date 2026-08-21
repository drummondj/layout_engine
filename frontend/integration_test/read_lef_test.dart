// A real end-to-end test against the actual native plugin - unlike
// test/widget_test.dart, nothing here is mocked: LeProvider.readLef()
// goes through the real 'layout_engine_plugin' MethodChannel
// (createTclConsole/evalTclCommand, see le_provider.dart's own comment on
// why read_lef is Tcl-routed) to a real embedded Tcl interpreter linked
// against the real backend. This is what actually caught the bug this
// test guards against: on Linux, createTclConsole was unimplemented
// (MissingPluginException), and LeProvider.readLef() silently swallowed
// it instead of surfacing an error - reading a file appeared to do
// nothing, with the Browser/Layer Manager staying empty and no feedback
// anywhere. A plain flutter_test widget test structurally cannot catch
// this class of bug: Flutter's default test binding intercepts every
// MethodChannel call and never reaches real native code, mocked or not -
// only integration_test's binding actually dispatches to a real running
// embedder.
//
// Must be run against a real desktop target, not plain `flutter test`:
//   flutter test integration_test/read_lef_test.dart -d macos
//   flutter test integration_test/read_lef_test.dart -d linux   # inside
//   Docker - see Dockerfile.linux-ci's frontend-gui stage for the
//   Xvfb/software-GL setup this needs on Linux, same requirement as
//   actually running the app.
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:layout_engine/providers/le_provider.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  // A comprehensive, checked-in vendored LEF fixture (backend's own
  // LEFReader regression test, backend/src/lefdef/lef/TEST/complete.5.8.lef)
  // - reused rather than duplicated. Has real LAYER and MACRO content, so
  // reading it actually exercises the Browser (library/design) and Layer
  // Manager population path this test is guarding, not just "didn't
  // crash".
  final String fixturePath =
      '${Directory.current.path}/../backend/src/lefdef/lef/TEST/complete.5.8.lef';

  testWidgets('readLef against the real native plugin populates layers and libraries', (
    WidgetTester tester,
  ) async {
    expect(
      File(fixturePath).existsSync(),
      isTrue,
      reason: 'fixture LEF file missing: $fixturePath',
    );

    final provider = LeProvider();
    final errors = <String>[];
    provider.addMessageListener((message) {
      if (message.startsWith('ERROR:')) errors.add(message);
    });

    await provider.init();
    await provider.readLef(fixturePath);

    expect(errors, isEmpty, reason: 'readLef reported: $errors');
    expect(
      provider.layers,
      isNotEmpty,
      reason: "Layer Manager should show the fixture's layers",
    );
    expect(
      await provider.getLibraries(),
      isNotEmpty,
      reason: "Browser should show the fixture's library/design",
    );
  });
}
