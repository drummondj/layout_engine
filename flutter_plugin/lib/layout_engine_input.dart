import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';

import 'layout_engine_plugin.dart';

/// LogicalKeyboardKey -> LeKeyCode. Extend alongside api.hpp's own
/// LeKeyCode enum as more interaction modes need shortcuts (see
/// LeEditor.keyDown's doc) - this is the one place a new key needs wiring
/// on the Dart side.
final Map<LogicalKeyboardKey, LeKeyCode> _keyCodeMap = {
  LogicalKeyboardKey.shiftLeft: LeKeyCode.LE_KEY_SHIFT,
  LogicalKeyboardKey.shiftRight: LeKeyCode.LE_KEY_SHIFT,
  LogicalKeyboardKey.keyZ: LeKeyCode.LE_KEY_ZOOM,
  LogicalKeyboardKey.keyF: LeKeyCode.LE_KEY_FIT,
  LogicalKeyboardKey.arrowLeft: LeKeyCode.LE_KEY_PAN_LEFT,
  LogicalKeyboardKey.arrowRight: LeKeyCode.LE_KEY_PAN_RIGHT,
  LogicalKeyboardKey.arrowUp: LeKeyCode.LE_KEY_PAN_UP,
  LogicalKeyboardKey.arrowDown: LeKeyCode.LE_KEY_PAN_DOWN,
  LogicalKeyboardKey.controlLeft: LeKeyCode.LE_KEY_CTRL,
  LogicalKeyboardKey.controlRight: LeKeyCode.LE_KEY_CTRL,
  LogicalKeyboardKey.keyA: LeKeyCode.LE_KEY_SELECT_ALL,
  LogicalKeyboardKey.keyD: LeKeyCode.LE_KEY_DESELECT_ALL,
  LogicalKeyboardKey.digit1: LeKeyCode.LE_KEY_1,
  LogicalKeyboardKey.digit2: LeKeyCode.LE_KEY_2,
  LogicalKeyboardKey.digit3: LeKeyCode.LE_KEY_3,
  LogicalKeyboardKey.digit4: LeKeyCode.LE_KEY_4,
  LogicalKeyboardKey.digit5: LeKeyCode.LE_KEY_5,
  LogicalKeyboardKey.digit6: LeKeyCode.LE_KEY_6,
  LogicalKeyboardKey.digit7: LeKeyCode.LE_KEY_7,
  LogicalKeyboardKey.digit8: LeKeyCode.LE_KEY_8,
  LogicalKeyboardKey.digit9: LeKeyCode.LE_KEY_9,
  LogicalKeyboardKey.digit0: LeKeyCode.LE_KEY_0,
  LogicalKeyboardKey.keyS: LeKeyCode.LE_KEY_SELECT_MODE,
  LogicalKeyboardKey.keyE: LeKeyCode.LE_KEY_EDIT_MODE,
  LogicalKeyboardKey.keyR: LeKeyCode.LE_KEY_RULER_MODE,
  LogicalKeyboardKey.escape: LeKeyCode.LE_KEY_FINISH_RULER,
  LogicalKeyboardKey.keyM: LeKeyCode.LE_KEY_MOVE,
};

/// Fixed per-scroll-event zoom step (UPDATES.md 9.2) - smaller than
/// [LeKeyCode.LE_KEY_ZOOM]'s own fixed step (0.3, see api.cpp's
/// kKeyZoomFactor) since a single trackpad/wheel gesture fires many
/// [PointerScrollEvent]s in quick succession; only the sign of
/// [PointerScrollEvent.scrollDelta]'s y component is used, not its
/// magnitude, which varies too wildly across input devices to use
/// directly (same "fixed step" reasoning as the keyboard shortcut).
const double kScrollZoomFactor = 0.1;

/// Converts standard Flutter `PointerEvent`/`KeyEvent` input straight into
/// [LeEditor]'s low-level selection API (`mouseDown`/`mouseUp`/
/// `setMousePosition`/`clearMousePosition`/`keyDown`/`keyUp`), so a caller
/// doesn't have to hand-roll a `LogicalKeyboardKey` -> `LeKeyCode` mapping
/// or pixel-space bookkeeping itself (see `example/lib/main.dart` for a
/// working `Listener`/`MouseRegion`/`Focus` wiring). Extends [LeEditorBase]
/// (not the concrete [LeEditor]) so it's also callable on a test double
/// implementing that interface - see [LeEditorBase]'s own doc comment for
/// why that distinction exists.
extension LeEditorInput on LeEditorBase {
  /// Converts a standard Flutter pointer event straight into the right
  /// low-level call - [x]/[y] come from [PointerEvent.localPosition],
  /// `.round()`'d, in the same raw-logical-pixel convention
  /// [LeEditor.setViewportSize]/[LeEditor.zoom]/[LeEditor.setMousePosition]
  /// already use everywhere in this codebase (no devicePixelRatio scaling -
  /// see CLAUDE.md). Every `PointerEvent` subtype is a plain, non-
  /// `covariant` Dart function parameter, so this method (typed `void
  /// Function(PointerEvent)`) is directly tear-off-assignable to a
  /// `Listener`'s onPointerDown/onPointerMove/onPointerHover/onPointerUp/
  /// onPointerCancel AND a `MouseRegion`'s onEnter/onHover/onExit - no
  /// wrapper closure needed for the conversion itself (though a real call
  /// site still wants one, to also call `LeTexture.markFrameAvailable()`
  /// after - this plugin's texture is pull-based, nothing repaints on its
  /// own).
  void handlePointerEvent(PointerEvent event) {
    final x = event.localPosition.dx.round();
    final y = event.localPosition.dy.round();
    switch (event) {
      case PointerDownEvent():
        setMousePosition(x, y);
        // The primary (left) button drives click/drag-select; the
        // secondary (right) button drives click/drag rectangle-zoom
        // (UPDATES.md 9.3) instead - the two gestures share the same
        // rubber-band machinery on the backend, differing only in which
        // one started it. `buttons` at down-time is the button(s) that
        // just went down, so this check is reliable here (unlike on the
        // matching PointerUpEvent below, where `buttons` reflects what's
        // *still* held post-release and can't identify which button was
        // lifted - so mouseUp is forwarded unconditionally instead;
        // le_mouse_up is already a no-op without a preceding
        // mouseDown/zoomDragDown, so an unrelated button's up event is
        // harmless).
        if (event.buttons & kPrimaryButton != 0) mouseDown(x, y);
        if (event.buttons & kSecondaryButton != 0) zoomDragDown(x, y);
      case PointerUpEvent():
        setMousePosition(x, y);
        mouseUp(x, y);
      case PointerMoveEvent():
      case PointerHoverEvent():
      case PointerEnterEvent():
        setMousePosition(x, y);
      case PointerCancelEvent():
      case PointerExitEvent():
        // No le_mouse_cancel exists - an unmatched mouseDown anchor is
        // inert until the next real mouseDown overwrites it (see
        // mouseUp's own doc), so only the hover/grid-snap visual needs
        // clearing here; self-correcting via the next hover/move anyway.
        clearMousePosition();
      default:
        break; // e.g. scroll/pan/zoom signal events - not this API's job.
    }
  }

  /// Converts a mouse scroll-wheel/trackpad signal straight into
  /// [LeEditor.zoom] (UPDATES.md 9.2), anchored at the pointer's current
  /// position. Flutter delivers this via `Listener.onPointerSignal` - a
  /// different callback/type hierarchy (`PointerSignalEvent`) from
  /// `onPointerDown`/`onPointerMove`/etc's `PointerEvent`, so it needs its
  /// own wiring:
  /// ```dart
  /// onPointerSignal: editor.handlePointerSignal,
  /// ```
  /// No-op for any [PointerSignalEvent] other than [PointerScrollEvent]
  /// (e.g. a future pointer-pan-zoom event isn't handled here).
  void handlePointerSignal(PointerSignalEvent event) {
    if (event is PointerScrollEvent) {
      final factor = event.scrollDelta.dy < 0 ? kScrollZoomFactor : -kScrollZoomFactor;
      zoom(factor, event.localPosition.dx.round(), event.localPosition.dy.round());
    }
  }

  /// Converts a standard Flutter key event straight into [LeEditor.keyDown]/
  /// [LeEditor.keyUp] for whichever [LeKeyCode] it maps to (see
  /// `_keyCodeMap`), or does nothing and returns false for any other key. A
  /// [KeyRepeatEvent] is just an extra, harmless `keyDown` call (it only
  /// marks a key "held").
  ///
  /// Returns whether the key was recognized, for a caller's own
  /// `Focus.onKeyEvent` (a different signature - `(FocusNode, KeyEvent) ->
  /// KeyEventResult` - so it always needs its own small wrapper, and often
  /// has other shortcuts of its own to interleave):
  /// ```dart
  /// onKeyEvent: (node, event) =>
  ///     editor.handleKeyEvent(event) ? KeyEventResult.handled : myOtherHandling(event),
  /// ```
  bool handleKeyEvent(KeyEvent event) {
    final keyCode = _keyCodeMap[event.logicalKey];
    if (keyCode == null) return false;
    switch (event) {
      case KeyDownEvent():
      case KeyRepeatEvent():
        keyDown(keyCode);
      case KeyUpEvent():
        keyUp(keyCode);
      default:
        return false;
    }
    return true;
  }

  /// Converts a `Focus` widget's `onFocusChange` straight into
  /// [LeEditor.clearAllKeys] on a focus *loss* - a no-op on gaining focus.
  /// A key's matching key-up event is not guaranteed to still reach a
  /// widget that no longer has focus by the time the physical key is
  /// released, so without this, a modifier (e.g. shift) held at the
  /// moment focus is lost would stay "held" indefinitely from the
  /// backend's point of view - silently turning every later plain click
  /// into a shift-click. Wire directly into `Focus.onFocusChange` (see
  /// `example/lib/main.dart`):
  /// ```dart
  /// onFocusChange: editor.handleFocusChange,
  /// ```
  void handleFocusChange(bool hasFocus) {
    if (!hasFocus) clearAllKeys();
  }
}
