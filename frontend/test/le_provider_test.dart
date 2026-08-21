// Regression tests for the LeProvider.readLef() bug this session found via
// a real user report: reading a LEF file printed "INFO: Reading $path" and
// then silently did nothing - no error, no data - because readLef() called
// _lock.synchronized(...) without awaiting or catching it, so an exception
// thrown before runTclCommand's own refreshAndNotify ran (the real
// repro: createTclConsole throwing MissingPluginException, since
// linux/layout_engine_plugin.cc hadn't implemented it yet) vanished as an
// unhandled async error instead of surfacing anywhere. FakeLeEditor (no
// FFI/MethodChannel calls anywhere) reproduces that exact failure mode
// without needing Docker, Linux, or a real native build at all.
import 'package:flutter_test/flutter_test.dart';
import 'package:layout_engine/providers/le_provider.dart';

import 'fakes/fake_le_editor.dart';

void main() {
  test('readLef surfaces a native-side failure as an ERROR message, not silently', () async {
    final editor = FakeLeEditor()..tclConsoleCreationError = Exception('MissingPluginException');
    final provider = LeProvider(editor: editor);
    final messages = <String>[];
    provider.addMessageListener(messages.add);

    await provider.readLef('/some/design.lef');
    // _messageStreamController is a plain (non-sync) broadcast
    // StreamController - an add() call isn't guaranteed delivered to
    // listeners before the next `await` in the calling function resolves,
    // so a fresh microtask flush is needed before asserting on messages.
    // Purely a test-observation gap, not a real one: the app's own
    // frame/microtask pumping always provides plenty of turns before a
    // user would ever notice.
    await pumpEventQueue();

    expect(
      messages.any((m) => m.startsWith('ERROR:') && m.contains('/some/design.lef')),
      isTrue,
      reason: 'readLef should surface the thrown error as an ERROR message: $messages',
    );
  });

  test('readLef succeeds, records the file as open, and rejects reading it again', () async {
    final editor = FakeLeEditor();
    final provider = LeProvider(editor: editor);
    final messages = <String>[];
    provider.addMessageListener(messages.add);

    await provider.readLef('/some/design.lef');
    // _messageStreamController is a plain (non-sync) broadcast
    // StreamController - an add() call isn't guaranteed delivered to
    // listeners before the next `await` in the calling function resolves,
    // so a fresh microtask flush is needed before asserting on messages.
    // Purely a test-observation gap, not a real one: the app's own
    // frame/microtask pumping always provides plenty of turns before a
    // user would ever notice.
    await pumpEventQueue();

    expect(messages.where((m) => m.startsWith('ERROR:')), isEmpty);
    expect(editor.lastTclConsole?.evaluatedCommands, ['read_lef {/some/design.lef}']);

    await provider.readLef('/some/design.lef');
    await pumpEventQueue();
    expect(messages.any((m) => m == 'ERROR: /some/design.lef already open'), isTrue);
  });

  test('a non-zero read_lef result does not mark the file as open', () async {
    final editor = FakeLeEditor()..onTclEval = (command) async => '1';
    final provider = LeProvider(editor: editor);
    final messages = <String>[];
    provider.addMessageListener(messages.add);

    await provider.readLef('/some/other.lef');
    await provider.readLef('/some/other.lef');
    await pumpEventQueue();

    expect(
      messages.any((m) => m == 'ERROR: /some/other.lef already open'),
      isFalse,
      reason: 'a non-zero read_lef result should not mark the file as open: $messages',
    );
    expect(editor.lastTclConsole?.evaluatedCommands.length, 2);
  });
}
