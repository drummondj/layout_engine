// Regression coverage for BUGS_AND_ENHANCEMENTS.md E9: recalling history
// with the Up arrow and then pressing Down past the most recent entry
// should restore whatever was being typed before recall started (empty,
// if nothing was) - not leave the last-recalled command stuck in the
// input. Terminal is pumped standalone (just a Provider + Scaffold), not
// through the full docking-layout app widget_test.dart uses - it has no
// dependency on that layout, and this keeps the test focused on the
// input/history behavior itself.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:layout_engine/components/terminal.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:provider/provider.dart';

import 'fakes/fake_le_editor.dart';

Future<void> pumpTerminal(WidgetTester tester, LeProvider provider) async {
  await tester.pumpWidget(
    MaterialApp(
      home: ChangeNotifierProvider<LeProvider>.value(
        value: provider,
        child: const Scaffold(body: Terminal()),
      ),
    ),
  );
  await tester.pumpAndSettle();
}

void main() {
  testWidgets(
    'Down past the most recent history entry restores the in-progress draft',
    (tester) async {
      final editor = FakeLeEditor()
        ..commandHistory.addAll(['first', 'second', 'third']);
      final provider = LeProvider(editor: editor)..refreshCommandHistory();
      await pumpTerminal(tester, provider);

      final field = find.byType(TextField);
      await tester.tap(field);
      await tester.enterText(field, 'my draft');
      await tester.pump();

      await tester.sendKeyEvent(LogicalKeyboardKey.arrowUp);
      await tester.pump();
      expect(tester.widget<TextField>(field).controller!.text, 'third');

      // One more Up than there are entries - stays on the oldest one
      // rather than wrapping/erroring, unrelated to E9 but confirms the
      // draft is still intact underneath.
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowUp);
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowUp);
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowUp);
      await tester.pump();
      expect(tester.widget<TextField>(field).controller!.text, 'first');

      await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await tester.pump();
      expect(tester.widget<TextField>(field).controller!.text, 'third');

      // Down once more, past the most recent entry - restores the draft.
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await tester.pump();
      expect(tester.widget<TextField>(field).controller!.text, 'my draft');

      // A fresh Up press starts a new recall session from the current
      // (restored) text, not from wherever the previous session left off.
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowUp);
      await tester.pump();
      expect(tester.widget<TextField>(field).controller!.text, 'third');
    },
  );

  testWidgets(
    'Down past the most recent entry restores empty text when nothing was typed first',
    (tester) async {
      final editor = FakeLeEditor()..commandHistory.addAll(['first', 'second']);
      final provider = LeProvider(editor: editor)..refreshCommandHistory();
      await pumpTerminal(tester, provider);

      final field = find.byType(TextField);
      await tester.tap(field);
      await tester.pump();

      await tester.sendKeyEvent(LogicalKeyboardKey.arrowUp);
      await tester.pump();
      expect(tester.widget<TextField>(field).controller!.text, 'second');

      await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await tester.pump();
      expect(tester.widget<TextField>(field).controller!.text, '');
    },
  );

  testWidgets('Down with no active recall session is a no-op', (tester) async {
    final editor = FakeLeEditor()..commandHistory.addAll(['first', 'second']);
    final provider = LeProvider(editor: editor)..refreshCommandHistory();
    await pumpTerminal(tester, provider);

    final field = find.byType(TextField);
    await tester.tap(field);
    await tester.enterText(field, 'untouched');
    await tester.pump();

    await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
    await tester.pump();
    expect(tester.widget<TextField>(field).controller!.text, 'untouched');
  });
}
