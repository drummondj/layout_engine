// BUGS_AND_ENHANCEMENTS.md E17 - a status-bar spinner for a render
// actually in progress, driven by interactive zoom/pan (LeEditor.zoom()/
// pan() never go through a Tcl command, unlike the existing isRunning
// spinner from E3) polling LeEditor.isRendering the same way
// LeTclConsole.eval polls for command completion. FakeLeEditor (no FFI/
// MethodChannel calls anywhere) exercises this without a real native
// build - see fakes/fake_le_editor.dart's own isRenderingValue.
import 'package:flutter_test/flutter_test.dart';
import 'package:layout_engine/providers/le_provider.dart';

import 'fakes/fake_le_editor.dart';

void main() {
  test('isRendering becomes true as soon as refreshTexture observes a render in progress', () async {
    final editor = FakeLeEditor()..isRenderingValue = true;
    final provider = LeProvider(editor: editor);
    expect(provider.isRendering, isFalse);

    // The watch loop's own first check is synchronous (no await before
    // it), so it already runs by the time this await returns.
    await provider.refreshTexture();
    expect(provider.isRendering, isTrue);
  });

  test('isRendering goes back to false once the render finishes', () async {
    final editor = FakeLeEditor()..isRenderingValue = true;
    final provider = LeProvider(editor: editor);

    await provider.refreshTexture();
    expect(provider.isRendering, isTrue);

    editor.isRenderingValue = false;
    // One poll interval (50ms) for the detached watch loop to observe
    // the change on its next tick and stop.
    await Future.delayed(const Duration(milliseconds: 80));
    expect(provider.isRendering, isFalse);
  });

  test('isRendering stays false immediately after refreshTexture when nothing is rendering yet', () async {
    final editor = FakeLeEditor(); // isRenderingValue defaults false
    final provider = LeProvider(editor: editor);

    await provider.refreshTexture();
    expect(provider.isRendering, isFalse);
  });

  // The actual bug this fixes: a first draft of _watchForRendering bailed
  // out on its own very first poll if that read false - which it always
  // did, since markFrameAvailable() only *signals* that a new frame is
  // wanted; the native platform pulls it asynchronously, on its own
  // schedule (typically the next vsync), not synchronously with
  // refreshTexture() itself. That meant the spinner could only ever
  // "accidentally" show up (e.g. a burst of rapid mouse-move events
  // happening to overlap a *previous* move's own still-in-flight
  // render), never for a real, single action like toggling every layer's
  // own visibility - reported directly: "I see the spinner sometimes for
  // mouse movement, but never for longer tasks, such as switching all
  // layer visibility on and off." This simulates that exact timing gap.
  test('isRendering catches a render that starts a short while after refreshTexture returns', () async {
    final editor = FakeLeEditor(); // starts false, like the real native gap
    final provider = LeProvider(editor: editor);

    await provider.refreshTexture();
    expect(provider.isRendering, isFalse);

    // Simulate the native texture callback starting its own render
    // slightly later, asynchronously - well within the grace window.
    await Future.delayed(const Duration(milliseconds: 20));
    editor.isRenderingValue = true;

    await Future.delayed(const Duration(milliseconds: 20));
    expect(provider.isRendering, isTrue);

    editor.isRenderingValue = false;
    await Future.delayed(const Duration(milliseconds: 20));
    expect(provider.isRendering, isFalse);
  });

  test('gives up after the grace window if nothing ever renders, and is ready to watch again afterward', () async {
    final editor = FakeLeEditor();
    final provider = LeProvider(editor: editor);

    await provider.refreshTexture();
    // Longer than _maxTicksWaitingForRenderToStart (25 * 8ms = 200ms) -
    // the watch loop must have given up and reset _watchingForRendering
    // by now, not still be spinning forever waiting for a render that
    // will never come.
    await Future.delayed(const Duration(milliseconds: 260));
    expect(provider.isRendering, isFalse);

    // A real render happening *now* must still be observable - proves
    // the earlier give-up actually reset the guard rather than leaving
    // it stuck true (which would silently no-op every future
    // refreshTexture call).
    editor.isRenderingValue = true;
    await provider.refreshTexture();
    await Future.delayed(const Duration(milliseconds: 20));
    expect(provider.isRendering, isTrue);
  });

  test('notifyListeners fires when isRendering actually changes, not on every poll', () async {
    final editor = FakeLeEditor()..isRenderingValue = true;
    final provider = LeProvider(editor: editor);
    int notifyCount = 0;
    provider.addListener(() => notifyCount++);

    await provider.refreshTexture();
    expect(provider.isRendering, isTrue);
    final int countAfterStart = notifyCount;
    expect(countAfterStart, greaterThan(0));

    // While isRenderingValue stays true, further polls observe no change
    // and must not notify again.
    await Future.delayed(const Duration(milliseconds: 120));
    expect(notifyCount, countAfterStart);

    editor.isRenderingValue = false;
    await Future.delayed(const Duration(milliseconds: 80));
    expect(provider.isRendering, isFalse);
    expect(notifyCount, greaterThan(countAfterStart));
  });

  test('a rapid second refreshTexture call while already watching does not start a second loop', () async {
    final editor = FakeLeEditor()..isRenderingValue = true;
    final provider = LeProvider(editor: editor);

    await provider.refreshTexture();
    await provider.refreshTexture();
    await provider.refreshTexture();
    expect(provider.isRendering, isTrue);

    editor.isRenderingValue = false;
    await Future.delayed(const Duration(milliseconds: 80));
    expect(provider.isRendering, isFalse);
  });
}
