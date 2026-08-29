// Drives a caller-supplied .tcl script against the real running app - not
// le_shell (backend/src/tcl/le_shell.cpp), which has no GUI/Texture and
// therefore never calls le_render_pixel_buffer, so the actual render
// pipeline (DesignRenderPipeline/FrameRenderPipeline/HierarchyResolver,
// and le_render_pixel_buffer's own FrameMarkStart/FrameMarkEnd) is never
// exercised there. This test exists to be run with a `tracy-capture`
// process already listening (see frontend/scripts/profile_tcl_script.sh,
// which drives this file end to end and exports the resulting .tracy
// trace to CSV via tracy-csvexport) - it has no assertions of its own
// about performance, only that the script ran cleanly.
//
// The script path is supplied at compile time via --dart-define, not a
// hardcoded fixture like read_lef_test.dart's - the whole point here is
// to profile a caller-chosen script, not assert one fixed script's own
// behavior:
//
//   flutter test integration_test/tcl_script_profile_test.dart -d macos \
//     --dart-define=TCL_SCRIPT_PATH=/absolute/path/to/script.tcl
//
// Must run against a real desktop target, not plain `flutter test` - see
// read_lef_test.dart's own comment (every MethodChannel call is mocked
// and never reaches real native code otherwise).
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:layout_engine/main.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:layout_engine_plugin/layout_engine_plugin.dart' show maximizeWindow;

const String _scriptPathEnv = String.fromEnvironment('TCL_SCRIPT_PATH');

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets(
    'runs the given Tcl script against the real app so the render pipeline is exercised for real',
    (WidgetTester tester) async {
      expect(
        _scriptPathEnv,
        isNotEmpty,
        reason:
            'Pass the script with --dart-define=TCL_SCRIPT_PATH=/abs/path/to/script.tcl',
      );
      final scriptFile = File(_scriptPathEnv);
      expect(
        scriptFile.existsSync(),
        isTrue,
        reason: 'Tcl script not found: $_scriptPathEnv',
      );

      final errors = <String>[];
      final provider = LeProvider();
      provider.addMessageListener((message) {
        if (message.startsWith('ERROR:')) errors.add(message);
      });

      // Builds the real app, including LayoutEngine's Texture widget -
      // it's part of home.dart's default docking layout, not built
      // lazily behind a button, so this always mounts it. A real,
      // on-screen Texture is what makes the native side actually call
      // le_render_pixel_buffer once told a frame is available; driving
      // LeProvider standalone the way read_lef_test.dart does never
      // reaches that path at all. `provider:` overrides the app's own
      // LeProvider construction (see MyApp's own doc comment) so this
      // test keeps a direct reference to the same instance LayoutEngine
      // ends up using, rather than fishing it back out of the tree.
      // `initiallyMaximizedItemId: 'layout'` starts the Layout docking
      // panel already maximized (see Home's own doc comment), so
      // LayoutEngine's viewport uses most of the window instead of
      // sharing it with the Browser/Console/Layers/Properties panels.
      await tester.pumpWidget(
        MyApp(provider: provider, initiallyMaximizedItemId: 'layout'),
      );

      // Fills the screen (see maximizeWindow's own doc comment) so the
      // maximized Layout panel above gets a large, representative
      // viewport regardless of whatever frame a previous run left the
      // window at - a real macOS window, not a fixed test surface, since
      // this runs against the real embedder (-d macos).
      await maximizeWindow();
      await tester.pumpAndSettle();

      // Grace window for tracy-capture (started before this test, see
      // profile_tcl_script.sh) to actually finish connecting -
      // TRACY_ON_DEMAND is off (backend/CMakeLists.txt), so the client
      // starts broadcasting/listening at process startup regardless of
      // whether any zone has fired yet, but the handshake itself still
      // takes a moment. Confirmed necessary by a real failure: without
      // this, a short-lived test (script runs, a few frames pump, done -
      // all in ~1 second) can finish and tear down the app before
      // tracy-capture's own connection handshake completes, which it
      // reports as "the client ... has disconnected during the initial
      // connection handshake" and produces no trace at all.
      await Future<void>.delayed(const Duration(seconds: 3));

      // LayoutEngine.initState() kicks off provider.init() (creates the
      // real native texture) without this test awaiting it directly -
      // pumpAndSettle() can return before that finishes, since the
      // platform-channel round trip inside init() doesn't itself
      // schedule a Flutter frame to pump. Poll the one Dart-visible
      // completion signal instead of guessing a frame/duration count.
      while (provider.texture == null) {
        await Future<void>.delayed(const Duration(milliseconds: 20));
      }
      await tester.pumpAndSettle();

      // Evaluated as one script - le_tcl_procs.tcl's own le_repl_eval
      // does `uplevel #0 $command` on whatever string it's handed, so a
      // whole multi-command file runs as a single runTclCommand call,
      // the same as one line typed into the Terminal (see
      // le_provider.dart's own doc comment on runTclCommand). Mutates
      // the same LeHandle the Texture above renders from (see
      // TCL_EXPLORATION.md's show_gui design) - not a separate handle.
      final String script = await scriptFile.readAsString();
      await provider.runTclCommand(script);

      // runTclCommand's own refreshAndNotify() already called
      // texture.markFrameAvailable() - pump real frames so the engine
      // actually pulls one through the native texture callback
      // (le_render_pixel_buffer), rather than asserting on Dart-side
      // state alone the way read_lef_test.dart does.
      for (var i = 0; i < 10; i++) {
        await tester.pump(const Duration(milliseconds: 16));
      }
      await tester.pumpAndSettle();

      // Trailing grace window, same reasoning as the startup one above -
      // TRACY_NO_EXIT is also off, so the client doesn't block process
      // exit to flush queued data to an already-connected tracy-capture;
      // this gives its background sender thread time to actually get
      // everything out over the socket before the test (and the app
      // along with it) tears down.
      await Future<void>.delayed(const Duration(seconds: 2));

      expect(errors, isEmpty, reason: 'Tcl script reported: $errors');
    },
  );
}
