import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:ffi/ffi.dart' as pkg_ffi;
import 'package:flutter/services.dart';

import 'layout_engine_plugin_bindings_generated.dart' hide LeObjectRef;
import 'layout_engine_plugin_bindings_generated.dart' as native
    show LeObjectRef;

/// Re-exported so callers of [LeEditor.keyDown]/[LeEditor.keyUp]/
/// [LeEditor.isKeyHeld] can reference key codes (e.g.
/// `LeKeyCode.LE_KEY_SHIFT`) without importing the generated bindings
/// file themselves - it's a plain generated enum (no ffi.Struct/native
/// pointer involved), so unlike `LePixelBuffer`/`LeLibraryId`/etc. there's
/// no need to wrap it in a hand-written type first.
export 'layout_engine_plugin_bindings_generated.dart' show LeKeyCode;

/// Re-exported for the same reason as [LeKeyCode] - see [LeObjectRef]/
/// [LeSelectedProperty.type]. The raw generated `LeObjectRef` struct
/// itself is deliberately *not* re-exported - [LeObjectRef] (this file's
/// own wrapper class, same name) is the public type; the native struct
/// only exists internally to cross the FFI boundary (see `_toNativeRef`/
/// `_fromNativeRef`), same reasoning as `LeLibraryId`/`LeTerminalId`/etc.
/// never being exposed directly either.
export 'layout_engine_plugin_bindings_generated.dart'
    show LeObjectKind, LePropertyType;

/// Re-exported for the same reason as [LeKeyCode] - see [LeEditor.mode].
export 'layout_engine_plugin_bindings_generated.dart' show LeMode;

/// Re-exported so callers get the pointer/key convenience layer
/// (handlePointerEvent/handleKeyEvent) from the same import used for
/// LeEditor itself - see layout_engine_input.dart.
export 'layout_engine_input.dart' show LeEditorInput;

const String _libName = 'layout_engine_plugin';

/// Method channel used only to hand a texture id back and forth with
/// native platform code (see [LeEditor.createTexture]/[LeTexture]) - Dart
/// FFI can't reach the Flutter engine's texture registry itself, only
/// platform embedder code can. Must match the channel name registered by
/// `LayoutEnginePlugin` (macOS: `macos/Classes/LayoutEnginePlugin.swift`; Linux:
/// `linux/layout_engine_plugin.cc`).
const MethodChannel _channel = MethodChannel('layout_engine_plugin');

/// The dynamic library in which the symbols for [LayoutEnginePluginBindings]
/// can be found.
final ffi.DynamicLibrary _dylib = () {
  if (Platform.isMacOS) {
    return ffi.DynamicLibrary.open('$_libName.framework/$_libName');
  }
  if (Platform.isLinux) {
    return ffi.DynamicLibrary.open('lib$_libName.so');
  }
  throw UnsupportedError(
    'layout_engine_plugin only supports macOS and Linux, not '
    '${Platform.operatingSystem} (see this plugin\'s CLAUDE.md).',
  );
}();

final LayoutEnginePluginBindings _bindings = LayoutEnginePluginBindings(_dylib);

/// A rendered frame from [LeEditor.renderPixelBuffer]: a copy of the
/// backend's RGBA8888 pixel buffer (see api.hpp's `LePixelBuffer`), safe to
/// keep around after the next render call unlike the native buffer it was
/// copied from.
class LeFrame {
  const LeFrame({
    required this.pixels,
    required this.width,
    required this.height,
    required this.rowBytes,
  });

  /// Premultiplied RGBA8888, row-major, top-to-bottom. [rowBytes] may
  /// exceed `width * 4` - always index by it, never assume a tight stride.
  final Uint8List pixels;
  final int width;
  final int height;
  final int rowBytes;
}

/// A stable identity for one Library, Design, or Abstract (mirrors the
/// database's own `Id<Tag>` handles at the FFI boundary - see api.hpp's
/// `LeLibraryId`/`LeDesignId`/`LeAbstractId`). Unlike [LeFrame]'s pixel
/// data, these carry no native pointer and no validity window - safe to
/// hold onto indefinitely and pass back into e.g. [LeEditor.setCurrentDesignById]
/// later, but only ever a value read from this API, never hand-built.
abstract class _LeRef {
  const _LeRef(this.index, this.generation);

  final int index;
  final int generation;

  /// False for a ref read off an invalid/out-of-range row (`index` is the
  /// database's own sentinel, `Id<Tag>::valid()`'s complement).
  bool get isValid => index != 0xFFFFFFFF;

  @override
  bool operator ==(Object other) =>
      other is _LeRef &&
      other.runtimeType == runtimeType &&
      other.index == index &&
      other.generation == generation;

  @override
  int get hashCode => Object.hash(runtimeType, index, generation);
}

/// Identifies one Library (one `read_lef` call's worth of Designs - see
/// [LeEditor.createTclConsole], the only way this plugin reads a LEF file).
class LeLibraryRef extends _LeRef {
  const LeLibraryRef(super.index, super.generation);
}

/// Identifies one Design within a Library.
class LeDesignRef extends _LeRef {
  const LeDesignRef(super.index, super.generation);
}

/// Identifies one Design's Abstract (placement) view.
class LeAbstractRef extends _LeRef {
  const LeAbstractRef(super.index, super.generation);
}

/// A stable identity for one database object of any of the seven
/// [LeObjectKind] classes (Library/Design/Abstract/Terminal/TerminalPort/
/// Obstruction/Shape) - the Dart-facing wrapper for api.hpp's
/// `LeObjectRef`, deliberately one concrete class (not a `_LeRef` leaf
/// per kind, unlike [LeLibraryRef]/[LeDesignRef]/[LeAbstractRef]): the
/// whole point is generic dispatch across kinds, e.g.
/// [LeEditor.objectPropertyCount]/[objectParent]/[objectChildren] work
/// the same way regardless of which kind `ref` is. Used to let the
/// Property Viewer walk the database hierarchy (Shape -> TerminalPort ->
/// Terminal -> Abstract -> Design -> Library) generically. Carries no
/// native pointer - safe to hold indefinitely - but only ever a value
/// read from this API, never hand-built.
class LeObjectRef {
  const LeObjectRef({
    required this.kind,
    required this.index,
    required this.generation,
  });

  final LeObjectKind kind;
  final int index;
  final int generation;

  /// False for a ref that doesn't resolve to a real object - e.g.
  /// [LeEditor.objectParent] on a Library (no parent), or an out-of-range
  /// [LeEditor.selectedObjectRef] index (`index` is the database's own
  /// sentinel, `Id<Tag>::valid()`'s complement).
  bool get isValid => index != 0xFFFFFFFF;

  @override
  bool operator ==(Object other) =>
      other is LeObjectRef &&
      other.kind == kind &&
      other.index == index &&
      other.generation == generation;

  @override
  int get hashCode => Object.hash(kind, index, generation);
}

/// Builds a native `LeObjectRef` struct value to pass by-value into an FFI
/// call. Deliberately *not* [LeEditor.setCurrentDesignById]'s calloc-then-
/// free-in-`finally` pattern: that one is safe only because the struct
/// view is consumed synchronously, inline, inside the very call
/// expression that also frees it - this helper instead *returns* the
/// struct view to its caller, so freeing before returning would hand back
/// a dangling reference. [ffi.Struct.create] allocates struct storage
/// backed by a plain Dart-managed buffer instead of malloc'd native
/// memory, with no manual free step at all - safe to return and use
/// later, exactly this helper's shape.
native.LeObjectRef _toNativeRef(LeObjectRef ref) {
  final result = ffi.Struct.create<native.LeObjectRef>();
  result.kind = ref.kind.value;
  result.index = ref.index;
  result.generation = ref.generation;
  return result;
}

LeObjectRef _fromNativeRef(native.LeObjectRef ref) {
  return LeObjectRef(
    kind: LeObjectKind.fromValue(ref.kind),
    index: ref.index,
    generation: ref.generation,
  );
}

/// One row of [LeEditor.library]: a Library's identity and name.
class LeLibrary {
  const LeLibrary({required this.id, required this.name});

  final LeLibraryRef id;
  final String name;
}

/// One row of [LeEditor.libraryDesign]: a Design's identity (plus its
/// parent Library's) and name.
class LeDesignEntry {
  const LeDesignEntry({
    required this.libraryId,
    required this.id,
    required this.abstractId,
    required this.name,
  });

  final LeLibraryRef libraryId;
  final LeDesignRef id;

  /// Null if this Design has no Abstract view yet (see api.hpp's
  /// `LeDesignInfo::abstract_id`).
  final LeAbstractRef? abstractId;
  final String name;
}

/// Mirrors `le::ViewLayerPurpose`'s declaration order (api.hpp's
/// `le_purpose_at`/`le_is_purpose_visible`/etc.) - the layer widget's
/// column axis, independent of any row (see [LeEditor.purposeAt]).
enum LeLayerPurpose {
  terminal,
  obstruction,
  boundary;

  static LeLayerPurpose? fromValue(int value) => switch (value) {
    0 => LeLayerPurpose.terminal,
    1 => LeLayerPurpose.obstruction,
    2 => LeLayerPurpose.boundary,
    _ => null,
  };
}

/// One row of [LeEditor.layer]: a layer-widget row's name and swatch
/// color - not tied to a physical Layer existing (BOUNDARY is a row like
/// any other). Visibility/selectability are set by [LeLayer.name] directly
/// ([LeEditor.setLayerNameVisible]/[setLayerNameSelectable]) - a whole row
/// together, not addressed by any id; see [LeLayerPurpose] for the other,
/// row-independent axis.
class LeLayer {
  const LeLayer({
    required this.name,
    required this.colorR,
    required this.colorG,
    required this.colorB,
  });

  final String name;

  /// The row's swatch color (0-255 per channel) - not the fill color or
  /// pattern, which are resolved per shape during rendering.
  final int colorR;
  final int colorG;
  final int colorB;
}

/// The current mouse position's coordinates in microns, snapped to the
/// minor grid (see [LeEditor.snappedMousePosition]) - the same point
/// [LeEditor.setMousePosition]'s grid-snap indicator box is centered on.
class LeSnappedMouse {
  const LeSnappedMouse({required this.xUm, required this.yUm});

  final double xUm;
  final double yUm;
}

/// One name/value row of an [LeObjectRef]'s property table - see
/// [LeEditor.objectProperties]. [value] is a `String`, `int`, or `double`
/// matching [type] (LE_PROPERTY_TYPE_STRING/_INT/_DOUBLE respectively) -
/// coordinates are always microns (double), converted from the
/// database's own dbu by the backend.
class LeSelectedProperty {
  const LeSelectedProperty({
    required this.name,
    required this.type,
    required this.value,
  });

  final String name;
  final LePropertyType type;
  final Object value;
}

/// The subset of [LeEditor]'s public surface that [LeEditorInput]
/// (`layout_engine_input.dart`) and the frontend app's own `LeProvider`
/// (`frontend/lib/providers/le_provider.dart`) actually call - exists so
/// `LeProvider` can depend on this interface instead of the concrete
/// [LeEditor] directly, and take an injected test double instead
/// (`frontend/test/fakes/fake_le_editor.dart`) in a plain `flutter test`.
/// [LeEditor]'s own constructor calls `dart:ffi`'s `DynamicLibrary.open`
/// eagerly, which only resolves inside a real built app bundle
/// (`flutter run`/`flutter build`) - never under plain `flutter test`'s
/// bare Dart VM - so merely *constructing* a real [LeEditor] crashes
/// there regardless of which methods are actually called; a test double
/// implementing this interface sidesteps that entirely. See each member's
/// matching doc comment on [LeEditor] itself for the real behavior - not
/// repeated here.
abstract interface class LeEditorBase {
  int get commandHistoryCount;
  String commandHistoryAt(int index);
  Future<LeTclConsoleBase> createTclConsole();
  Future<LeTextureBase> createTexture();
  void armMove();
  void clearRulers();
  void deselectAll();
  void fitScene(int paddingPx);
  bool isLayerNameSelectable(String layerName);
  bool isLayerNameVisible(String layerName);
  bool get isMoveArmed;
  bool isPurposeSelectable(LeLayerPurpose purpose);
  bool isPurposeVisible(LeLayerPurpose purpose);
  LeLayer? layer(int rowIndex);
  int get layerCount;
  LeLibrary? library(int index);
  int get libraryCount;
  LeDesignEntry? libraryDesign(int libraryIndex, int designIndex);
  int libraryDesignCount(int libraryIndex);
  String? messageAt(int index);
  int get messageCount;
  LeMode get mode;
  List<LeObjectRef> objectChildren(LeObjectRef ref);
  LeObjectRef objectParent(LeObjectRef ref);
  List<LeSelectedProperty> objectProperties(LeObjectRef ref);
  LeLayerPurpose? purposeAt(int index);
  int get purposeCount;
  bool redo();
  void selectAll();
  LeObjectRef selectedObjectRef(int selectionIndex);
  int get selectionCount;
  int get selectionVersion;
  bool setCurrentDesignById(LeDesignRef designId);
  void setLayerNameSelectable(String layerName, bool selectable);
  void setLayerNameVisible(String layerName, bool visible);
  void setMode(LeMode mode);
  void setPurposeSelectable(LeLayerPurpose purpose, bool selectable);
  void setPurposeVisible(LeLayerPurpose purpose, bool visible);
  void setViewportSize(int widthPx, int heightPx);
  LeSnappedMouse? get snappedMousePosition;
  String get tooltipMessage;
  bool undo();

  // Below: not called by LeProvider directly, but needed so
  // LeEditorInput's extension methods (handlePointerEvent/
  // handlePointerSignal/handleKeyEvent/handleFocusChange - also called
  // directly by LeProvider) can be retargeted onto this interface instead
  // of the concrete LeEditor.
  void clearAllKeys();
  void clearMousePosition();
  void keyDown(LeKeyCode keyCode);
  void keyUp(LeKeyCode keyCode);
  void mouseDown(int x, int y);
  void mouseUp(int x, int y);
  void setMousePosition(int x, int y);
  void zoom(double factor, int x, int y);
  void zoomDragDown(int x, int y);
}

/// [LeTexture]'s public surface actually called by `LeProvider`/
/// `layout_engine.dart` - same rationale and pairing as [LeEditorBase]
/// (see its doc comment): a test double can implement just this, with no
/// real `MethodChannel` involved at all, rather than needing
/// [LeEditorBase.createTexture]'s test double to construct (and a test to
/// mock the channel behind) a real [LeTexture].
abstract interface class LeTextureBase {
  int get textureId;
  Future<void> markFrameAvailable();
}

/// [LeTclConsole]'s public surface actually called by `LeProvider` - same
/// rationale as [LeTextureBase].
abstract interface class LeTclConsoleBase {
  Future<String> eval(String command);
}

/// One editor instance: owns a native `LeHandle*` (a Root/ViewLayerSet/
/// Scene/Pipeline/Renderer, see api.hpp) that's reused across calls,
/// matching the backend's own "one instance per Scene-equivalent lifetime"
/// design - don't create a fresh [LeEditor] per frame.
class LeEditor implements LeEditorBase {
  LeEditor() : _handle = _bindings.le_create() {
    // le_create() is documented to never return null; this is a sanity
    // check against that contract changing, not an expected runtime path.
    if (_handle == ffi.nullptr) {
      throw StateError('le_create() returned null');
    }
  }

  final ffi.Pointer<LeHandle> _handle;
  bool _disposed = false;

  /// The raw handle address, for handing off to native platform texture
  /// code - see this plugin's CLAUDE.md: the texture path calls
  /// `le_render_pixel_buffer` directly from native code on every frame
  /// pull, not through this Dart wrapper.
  int get nativeHandleAddress => _handle.address;

  void _checkNotDisposed() {
    if (_disposed) {
      throw StateError('LeEditor used after dispose()');
    }
  }

  /// Total number of error/warning/info messages produced by this
  /// handle's backend operations so far (currently just a `read_lef` Tcl
  /// command - see [createTclConsole], the only way this plugin reads a
  /// LEF file - file-open/parse errors, parser warnings, parser info
  /// notes, and a success summary, each already formatted with its own
  /// "ERROR "/"WARNING "/"INFO " prefix). Monotonically increasing -
  /// entries are never removed, cleared, or reordered - so a caller can
  /// poll this like [selectionVersion] and only fetch [messageAt] for
  /// indices at or past what it last saw.
  @override
  int get messageCount {
    _checkNotDisposed();
    return _bindings.le_message_count(_handle);
  }

  /// The message at [index] (0..[messageCount] - 1), or null if [index]
  /// is out of range.
  @override
  String? messageAt(int index) {
    _checkNotDisposed();
    final msgPtr = _bindings.le_message_at(_handle, index);
    if (msgPtr == ffi.nullptr) return null;
    return msgPtr.cast<pkg_ffi.Utf8>().toDartString();
  }

  /// Number of Designs loaded across every LEF file read into this handle
  /// so far.
  int get designCount {
    _checkNotDisposed();
    return _bindings.le_design_count(_handle);
  }

  /// Name of the Design at [index] (0..[designCount] - 1), or null if
  /// [index] is out of range.
  String? designName(int index) {
    _checkNotDisposed();
    final namePtr = _bindings.le_design_name(_handle, index);
    if (namePtr == ffi.nullptr) return null;
    return namePtr.cast<pkg_ffi.Utf8>().toDartString();
  }

  /// Selects the Design at [index] as the one [renderPixelBuffer] renders
  /// (its Abstract view). Returns true on success; the current selection is
  /// left unchanged on failure.
  bool setCurrentDesign(int index) {
    _checkNotDisposed();
    return _bindings.le_set_current_design(_handle, index) == 0;
  }

  /// Number of Libraries currently loaded - one per `read_lef` Tcl command
  /// so far (see [createTclConsole]).
  /// The top level of a Library -> Design -> Abstract browser widget; see
  /// [libraryDesignCount]/[libraryDesign] for the next level.
  @override
  int get libraryCount {
    _checkNotDisposed();
    return _bindings.le_library_count(_handle);
  }

  /// The Library at [index] (0..[libraryCount] - 1), or null if [index] is
  /// out of range.
  @override
  LeLibrary? library(int index) {
    _checkNotDisposed();
    final info = _bindings.le_library_at(_handle, index);
    if (info.name == ffi.nullptr) return null;
    return LeLibrary(
      id: LeLibraryRef(info.id.index, info.id.generation),
      name: info.name.cast<pkg_ffi.Utf8>().toDartString(),
    );
  }

  /// Number of Designs belonging to the Library at [libraryIndex] (into the
  /// same enumeration [library] uses).
  @override
  int libraryDesignCount(int libraryIndex) {
    _checkNotDisposed();
    return _bindings.le_library_design_count(_handle, libraryIndex);
  }

  /// The Design at [designIndex] within the Library at [libraryIndex]
  /// (0..[libraryDesignCount] - 1), or null if either index is out of
  /// range.
  @override
  LeDesignEntry? libraryDesign(int libraryIndex, int designIndex) {
    _checkNotDisposed();
    final info = _bindings.le_library_design_at(
      _handle,
      libraryIndex,
      designIndex,
    );
    if (info.name == ffi.nullptr) return null;
    final abstractId = info.abstract_id.index == 0xFFFFFFFF
        ? null
        : LeAbstractRef(info.abstract_id.index, info.abstract_id.generation);
    return LeDesignEntry(
      libraryId: LeLibraryRef(
        info.library_id.index,
        info.library_id.generation,
      ),
      id: LeDesignRef(info.id.index, info.id.generation),
      abstractId: abstractId,
      name: info.name.cast<pkg_ffi.Utf8>().toDartString(),
    );
  }

  /// Selects a Design by its stable [LeDesignRef] (e.g. one read from
  /// [libraryDesign]'s [LeDesignEntry.id]) as the one [renderPixelBuffer]
  /// renders - same effect as [setCurrentDesign] but addressed by identity
  /// instead of a position in the flat [designCount] list, the natural fit
  /// for a browser widget's row click. Returns true on success; the
  /// current selection is left unchanged on failure.
  @override
  bool setCurrentDesignById(LeDesignRef designId) {
    _checkNotDisposed();
    final idPtr = pkg_ffi.calloc<LeDesignId>();
    try {
      idPtr.ref.index = designId.index;
      idPtr.ref.generation = designId.generation;
      return _bindings.le_set_current_design_by_id(_handle, idPtr.ref) == 0;
    } finally {
      pkg_ffi.calloc.free(idPtr);
    }
  }

  /// Number of layer-widget rows currently available (0 before the first
  /// `read_lef` Tcl command, since no ViewLayerSet has been built yet). Includes
  /// BOUNDARY and any future non-Technology-derived row, not just physical
  /// Layers. See [layer] for each row's contents.
  @override
  int get layerCount {
    _checkNotDisposed();
    return _bindings.le_layer_count(_handle);
  }

  /// The row at [rowIndex] (0..[layerCount] - 1), or null if out of range.
  @override
  LeLayer? layer(int rowIndex) {
    _checkNotDisposed();
    final row = _bindings.le_layer_at(_handle, rowIndex);
    if (row.name == ffi.nullptr) return null;
    return LeLayer(
      name: row.name.cast<pkg_ffi.Utf8>().toDartString(),
      colorR: row.color_r,
      colorG: row.color_g,
      colorB: row.color_b,
    );
  }

  /// Number of distinct purposes across the currently loaded ViewLayerSet -
  /// the layer widget's column axis, independent of any row. See
  /// [purposeAt] for each one.
  @override
  int get purposeCount {
    _checkNotDisposed();
    return _bindings.le_purpose_count(_handle);
  }

  /// The purpose at [index] (0..[purposeCount] - 1), or null if out of
  /// range.
  @override
  LeLayerPurpose? purposeAt(int index) {
    _checkNotDisposed();
    return LeLayerPurpose.fromValue(_bindings.le_purpose_at(_handle, index));
  }

  /// Whether every ViewLayer named [layerName] (a whole [LeLayer] row
  /// together, not one purpose-column) is currently visible - true by
  /// default until toggled with [setLayerNameVisible]. See
  /// [isPurposeVisible] for the other, row-independent axis.
  @override
  bool isLayerNameVisible(String layerName) {
    _checkNotDisposed();
    final namePtr = layerName.toNativeUtf8();
    try {
      return _bindings.le_is_layer_name_visible(_handle, namePtr.cast()) != 0;
    } finally {
      pkg_ffi.calloc.free(namePtr);
    }
  }

  /// Sets the visibility of every ViewLayer named [layerName] - e.g. a
  /// layer-widget row-header checkbox. Affects rendering.
  @override
  void setLayerNameVisible(String layerName, bool visible) {
    _checkNotDisposed();
    final namePtr = layerName.toNativeUtf8();
    try {
      _bindings.le_set_layer_name_visible(
        _handle,
        namePtr.cast(),
        visible ? 1 : 0,
      );
    } finally {
      pkg_ffi.calloc.free(namePtr);
    }
  }

  /// Whether every ViewLayer whose purpose is [purpose] (a whole column,
  /// not one row) is currently visible - true by default until toggled
  /// with [setPurposeVisible].
  @override
  bool isPurposeVisible(LeLayerPurpose purpose) {
    _checkNotDisposed();
    return _bindings.le_is_purpose_visible(_handle, purpose.index) != 0;
  }

  /// Sets the visibility of every ViewLayer whose purpose is [purpose] -
  /// e.g. a layer-widget column-header checkbox. Affects rendering.
  @override
  void setPurposeVisible(LeLayerPurpose purpose, bool visible) {
    _checkNotDisposed();
    _bindings.le_set_purpose_visible(_handle, purpose.index, visible ? 1 : 0);
  }

  /// The current interaction mode (UPDATES.md item 11). Select is the
  /// only mode where mouse clicks/drags ([mouseDown]/[mouseUp]) change
  /// the current selection - Edit mode restricts mouse interaction to
  /// editing whatever is already selected (behavior TBD, a later item).
  @override
  LeMode get mode {
    _checkNotDisposed();
    return LeMode.fromValue(_bindings.le_get_mode(_handle));
  }

  /// Switches the current interaction mode - see [mode]. Also reachable
  /// via [LeEditorInput.handleKeyEvent]'s 's'/'e'/'r' shortcuts
  /// (LE_KEY_SELECT_MODE/LE_KEY_EDIT_MODE/LE_KEY_RULER_MODE).
  @override
  void setMode(LeMode mode) {
    _checkNotDisposed();
    _bindings.le_set_mode(_handle, mode.value);
  }

  /// Number of rulers (UPDATES.md item 13) - multiple can exist at once,
  /// since starting a new one never clears an existing one. Indexes
  /// [rulerPointCount]/[rulerPointAt]'s own `rulerIndex` parameter.
  int get rulerCount {
    _checkNotDisposed();
    return _bindings.le_ruler_count(_handle);
  }

  /// Number of committed points on the ruler at [rulerIndex]
  /// (0..[rulerCount]-1) - indexes [rulerPointAt]'s own `pointIndex`
  /// parameter.
  int rulerPointCount(int rulerIndex) {
    _checkNotDisposed();
    return _bindings.le_ruler_point_count(_handle, rulerIndex);
  }

  /// The point at `pointIndex` (0..[rulerPointCount] - 1) on the ruler
  /// at [rulerIndex], in microns.
  Offset rulerPointAt(int rulerIndex, int pointIndex) {
    _checkNotDisposed();
    final p = _bindings.le_ruler_point_at(_handle, rulerIndex, pointIndex);
    return Offset(p.x_um, p.y_um);
  }

  /// Finishes the active ruler, if any - the next click in Ruler mode
  /// starts a new ruler instead of appending to this one. Reachable via
  /// the Esc key (LE_KEY_FINISH_RULER) - see [LeEditorInput.handleKeyEvent].
  void finishRuler() {
    _checkNotDisposed();
    _bindings.le_finish_ruler(_handle);
  }

  /// Removes every ruler (finished or not).
  @override
  void clearRulers() {
    _checkNotDisposed();
    _bindings.le_clear_rulers(_handle);
  }

  // --- Editing / undo-redo (UPDATES.md item 21) ---

  /// Begins recording a new undo/redo transaction labeled [label] (e.g.
  /// the raw text of a typed command). See [endCommand].
  void beginCommand(String label) {
    _checkNotDisposed();
    final labelPtr = label.toNativeUtf8();
    try {
      _bindings.le_begin_command(_handle, labelPtr.cast());
    } finally {
      pkg_ffi.calloc.free(labelPtr);
    }
  }

  /// Ends the transaction started by [beginCommand]. If it recorded at
  /// least one step, pushes it onto the undo stack regardless of
  /// [succeeded]; if [succeeded] is true, the label is also appended to
  /// the command-recall log ([commandHistoryCount]/[commandHistoryAt]).
  void endCommand(bool succeeded) {
    _checkNotDisposed();
    _bindings.le_end_command(_handle, succeeded ? 1 : 0);
  }

  /// Undoes the most recently recorded transaction, if any. Returns true
  /// if something was undone.
  @override
  bool undo() {
    _checkNotDisposed();
    return _bindings.le_undo(_handle) != 0;
  }

  /// Redoes the most recently undone transaction, if any. Returns true
  /// if something was redone.
  @override
  bool redo() {
    _checkNotDisposed();
    return _bindings.le_redo(_handle) != 0;
  }

  /// True if [undo] would currently do something.
  bool get canUndo {
    _checkNotDisposed();
    return _bindings.le_can_undo(_handle) != 0;
  }

  /// True if [redo] would currently do something.
  bool get canRedo {
    _checkNotDisposed();
    return _bindings.le_can_redo(_handle) != 0;
  }

  /// Number of recorded command-recall entries (only successfully
  /// executed commands - see [endCommand]) - indexes [commandHistoryAt]'s
  /// own `index` parameter.
  @override
  int get commandHistoryCount {
    _checkNotDisposed();
    return _bindings.le_command_history_count(_handle);
  }

  /// The command text at [index] (0..[commandHistoryCount]-1).
  @override
  String commandHistoryAt(int index) {
    _checkNotDisposed();
    final textPtr = _bindings.le_command_history_at(_handle, index);
    if (textPtr == ffi.nullptr) return '';
    return textPtr.cast<pkg_ffi.Utf8>().toDartString();
  }

  /// Selects every currently selectable shape in the current Abstract -
  /// the Select-mode toolbox button's direct entry point (same underlying
  /// behavior as Ctrl-A - see [LeEditorInput.handleKeyEvent] - but not
  /// gated on a held Ctrl key).
  @override
  void selectAll() {
    _checkNotDisposed();
    _bindings.le_select_all(_handle);
  }

  /// Clears the current selection - the Select-mode toolbox button's
  /// direct entry point (same underlying behavior as Ctrl-D).
  @override
  void deselectAll() {
    _checkNotDisposed();
    _bindings.le_deselect_all(_handle);
  }

  /// Arms Move - equivalent to Ctrl-M. Only meaningful in Edit mode with
  /// a non-empty selection; a no-op otherwise. The next two [mouseUp]
  /// clicks in Edit mode set the move's anchor, then commit it.
  @override
  void armMove() {
    _checkNotDisposed();
    _bindings.le_arm_move(_handle);
  }

  /// Cancels an in-progress move without applying it.
  void cancelMove() {
    _checkNotDisposed();
    _bindings.le_cancel_move(_handle);
  }

  /// True if Move is currently armed (whether or not its anchor has been
  /// set yet) - for the Move toolbox button's own pressed/armed visual
  /// state.
  @override
  bool get isMoveArmed {
    _checkNotDisposed();
    return _bindings.le_is_move_armed(_handle) != 0;
  }

  /// Whether every ViewLayer named [layerName] is currently selectable -
  /// true by default until toggled with [setLayerNameSelectable]. Purely
  /// an interaction-layer concern (no hit-testing/click-to-select API
  /// exists yet to consult it) - doesn't affect rendering.
  @override
  bool isLayerNameSelectable(String layerName) {
    _checkNotDisposed();
    final namePtr = layerName.toNativeUtf8();
    try {
      return _bindings.le_is_layer_name_selectable(_handle, namePtr.cast()) !=
          0;
    } finally {
      pkg_ffi.calloc.free(namePtr);
    }
  }

  /// Sets the selectability of every ViewLayer named [layerName].
  @override
  void setLayerNameSelectable(String layerName, bool selectable) {
    _checkNotDisposed();
    final namePtr = layerName.toNativeUtf8();
    try {
      _bindings.le_set_layer_name_selectable(
        _handle,
        namePtr.cast(),
        selectable ? 1 : 0,
      );
    } finally {
      pkg_ffi.calloc.free(namePtr);
    }
  }

  /// Whether every ViewLayer whose purpose is [purpose] is currently
  /// selectable - true by default until toggled with
  /// [setPurposeSelectable].
  @override
  bool isPurposeSelectable(LeLayerPurpose purpose) {
    _checkNotDisposed();
    return _bindings.le_is_purpose_selectable(_handle, purpose.index) != 0;
  }

  /// Sets the selectability of every ViewLayer whose purpose is [purpose].
  @override
  void setPurposeSelectable(LeLayerPurpose purpose, bool selectable) {
    _checkNotDisposed();
    _bindings.le_set_purpose_selectable(
      _handle,
      purpose.index,
      selectable ? 1 : 0,
    );
  }

  /// Zooms the viewport, keeping the dbu point under screen pixel (x, y)
  /// fixed on screen. [factor] is a signed fractional step applied to the
  /// current scale (new_scale = scale * (1 + factor)) - positive zooms in,
  /// negative zooms out (e.g. 0.1 zooms in 10%, -0.1 zooms out 10%); a
  /// factor <= -1.0 is ignored. [x]/[y] are in the same pixel space as
  /// [renderPixelBuffer]'s output (top-left origin, y increasing downward)
  /// - safe to feed straight from a pointer/tap event on the rendered
  /// image. The backend owns pan/scale entirely - there is no direct
  /// setter; use [fitScene] to reset to a known view.
  @override
  void zoom(double factor, int x, int y) {
    _checkNotDisposed();
    _bindings.le_zoom(_handle, factor, x, y);
  }

  /// Pans the viewport by a fraction of its own size, in dbu-space
  /// directions (positive [xFactor]/[yFactor] move the view toward
  /// increasing dbu x/y, not screen space). E.g. `xFactor: 1.0` pans right
  /// by exactly one full viewport width of content.
  void pan(double xFactor, double yFactor) {
    _checkNotDisposed();
    _bindings.le_pan(_handle, xFactor, yFactor);
  }

  /// Sets the viewport size in pixels - also the size of the buffer
  /// [renderPixelBuffer] produces.
  @override
  void setViewportSize(int widthPx, int heightPx) {
    _checkNotDisposed();
    _bindings.le_set_viewport_size(_handle, widthPx, heightPx);
  }

  /// Fits the viewport's pan/scale to the currently selected Design's
  /// content bbox: uniform scale (no stretch) so the content fills the
  /// viewport set via [setViewportSize] with [paddingPx] of margin on every
  /// side, pan centering it. Degrades to scale 1.0 / pan (0, 0) if no
  /// Design is selected or its Abstract has no shapes, rather than
  /// throwing. Call [setViewportSize] first - this fits to that viewport's
  /// current size, not a size set afterward.
  @override
  void fitScene(int paddingPx) {
    _checkNotDisposed();
    _bindings.le_fit_scene(_handle, paddingPx);
  }

  /// Spacing (dbu) between minor grid dots, drawn behind the design by
  /// [renderPixelBuffer]. Defaults to 5 (dbu), matching a 5nm minor grid
  /// under the common "1 dbu = 1nm" Technology convention.
  int get minorGridSpacing {
    _checkNotDisposed();
    return _bindings.le_minor_grid_spacing(_handle);
  }

  /// Sets the minor grid dot spacing (dbu). Affects rendering; values <= 0
  /// are ignored, same guard as [zoom]'s scale.
  set minorGridSpacing(int dbu) {
    _checkNotDisposed();
    _bindings.le_set_minor_grid_spacing(_handle, dbu);
  }

  /// Spacing (dbu) between major grid dots (drawn bolder than minor ones).
  /// Defaults to 50 (dbu), matching a 50nm major grid under the common
  /// "1 dbu = 1nm" Technology convention.
  int get majorGridSpacing {
    _checkNotDisposed();
    return _bindings.le_major_grid_spacing(_handle);
  }

  /// Sets the major grid dot spacing (dbu). Affects rendering; values <= 0
  /// are ignored.
  set majorGridSpacing(int dbu) {
    _checkNotDisposed();
    _bindings.le_set_major_grid_spacing(_handle, dbu);
  }

  /// On-screen text size (px) for every ruler label - tick values, each
  /// segment's own point-to-point distance, and a ruler's running total
  /// (UPDATES.md item 13). Defaults to 11.0.
  double get rulerLabelSize {
    _checkNotDisposed();
    return _bindings.le_ruler_label_size(_handle);
  }

  /// Sets the ruler label text size (px). Affects rendering; values <= 0
  /// are ignored, same guard as [minorGridSpacing].
  set rulerLabelSize(double px) {
    _checkNotDisposed();
    _bindings.le_set_ruler_label_size(_handle, px);
  }

  /// Sets the current mouse position, in the same pixel space as
  /// [renderPixelBuffer]'s output (top-left origin, y increasing downward)
  /// and [zoom]'s x/y - feed straight from a pointer-move event. Drives
  /// the grid-snap indicator box [renderPixelBuffer] draws, and updates
  /// which selectable shape (if any) is hovered - drawn with a yellow
  /// outline - via a hit-test against the shapes currently on screen.
  /// Never invalidates the (potentially design-sized) rasterized design
  /// cache, only the small overlay picture, but the hit-test itself is
  /// bounded by the number of shapes currently visible (already
  /// viewport-culled), not the whole design - see backend/BENCHMARKS.md
  /// for measured cost on a large design.
  @override
  void setMousePosition(int x, int y) {
    _checkNotDisposed();
    _bindings.le_set_mouse_position(_handle, x, y);
  }

  /// Clears the current mouse position (e.g. on a pointer-leave event) so
  /// the grid-snap indicator box and any hover outline stop showing at/for
  /// the last known position.
  @override
  void clearMousePosition() {
    _checkNotDisposed();
    _bindings.le_clear_mouse_position(_handle);
  }

  /// The current mouse position's coordinates in microns, snapped to the
  /// minor grid - the same point [setMousePosition]'s grid-snap indicator
  /// box is centered on. Null if no mouse position has been set (or
  /// [clearMousePosition] was called since), or no Technology has been
  /// read yet (`read_lef`) to convert dbu to microns with.
  @override
  LeSnappedMouse? get snappedMousePosition {
    _checkNotDisposed();
    final result = _bindings.le_snapped_mouse_position(_handle);
    if (result.has_position == 0) return null;
    return LeSnappedMouse(xUm: result.x_um, yUm: result.y_um);
  }

  /// Marks [keyCode] as currently held, e.g. on a key-down event - queried
  /// internally by gesture commands (e.g. [mouseUp]'s shift-click/
  /// shift-drag behavior).
  @override
  void keyDown(LeKeyCode keyCode) {
    _checkNotDisposed();
    _bindings.le_key_down(_handle, keyCode.value);
  }

  /// Marks [keyCode] as no longer held, e.g. on a key-up event.
  @override
  void keyUp(LeKeyCode keyCode) {
    _checkNotDisposed();
    _bindings.le_key_up(_handle, keyCode.value);
  }

  /// Whether [keyCode] is currently held (see [keyDown]).
  bool isKeyHeld(LeKeyCode keyCode) {
    _checkNotDisposed();
    return _bindings.le_is_key_held(_handle, keyCode.value) != 0;
  }

  /// Clears every currently-held key at once - call when the widget
  /// receiving key events loses focus (e.g. a `Focus` widget's
  /// `onFocusChange(false)`, see `LeEditorInput`/the example app). A
  /// key's matching key-up event is not guaranteed to still reach a
  /// widget that no longer has focus by the time the physical key is
  /// released, so without calling this on a focus loss, a modifier held
  /// at that moment stays "held" indefinitely - silently turning every
  /// later plain click into a shift-click until that same key happens to
  /// be pressed and released again while focused.
  @override
  void clearAllKeys() {
    _checkNotDisposed();
    _bindings.le_clear_all_keys(_handle);
  }

  /// Begins a mouse gesture at [x]/[y] (same pixel space as
  /// [setMousePosition]/[zoom]), e.g. on a pointer-down event. Records the
  /// gesture's anchor point; [mouseUp] later decides whether the gesture
  /// was a click or a rubber-band drag-select by comparing the down/up
  /// pixel distance against a small threshold.
  @override
  void mouseDown(int x, int y) {
    _checkNotDisposed();
    _bindings.le_mouse_down(_handle, x, y);
  }

  /// Begins a rectangle-zoom gesture at [x]/[y] (same pixel space as
  /// [mouseDown]), e.g. on a right-button pointer-down event. Like
  /// [mouseDown], only records the gesture's anchor point - [mouseUp] is
  /// the shared endpoint for both gesture kinds (see its own doc for why).
  @override
  void zoomDragDown(int x, int y) {
    _checkNotDisposed();
    _bindings.le_zoom_drag_down(_handle, x, y);
  }

  /// Ends a mouse gesture at [x]/[y], e.g. on a pointer-up event. A no-op
  /// if there was no matching [mouseDown]/[zoomDragDown] call first.
  ///
  /// - Started by [mouseDown]: click (small down/up distance) hit-tests the
  ///   single point; drag-select (larger distance) hit-tests every
  ///   selectable shape on every layer fully enclosed by the down/up
  ///   rectangle - either way, replaces the current selection unless
  ///   [LeKeyCode.LE_KEY_SHIFT] is currently held ([isKeyHeld]), in which
  ///   case the results are added to it instead.
  /// - Started by [zoomDragDown]: fits the viewport to the down/up
  ///   rectangle (same math as [fitScene] with 0 padding), unless the drag
  ///   was click-sized, in which case nothing happens. Selection is
  ///   untouched either way.
  @override
  void mouseUp(int x, int y) {
    _checkNotDisposed();
    _bindings.le_mouse_up(_handle, x, y);
  }

  /// Instructional text describing which mouse gestures and keyboard
  /// modifiers are currently available, for display in the GUI's status
  /// bar below the texture - e.g. "Left click to select. Shift for
  /// multi-select. Left click and drag for rectangle multi-select." for
  /// the current (and, for now, only) interaction mode, Select.
  @override
  String get tooltipMessage {
    _checkNotDisposed();
    final msgPtr = _bindings.le_tooltip_message(_handle);
    if (msgPtr == ffi.nullptr) return '';
    return msgPtr.cast<pkg_ffi.Utf8>().toDartString();
  }

  /// Number of currently selected objects. Indexes [selectedObjectRef]'s
  /// own `selectionIndex` parameter, 0..this-1, in the same (insertion)
  /// order as the selection itself.
  @override
  int get selectionCount {
    _checkNotDisposed();
    return _bindings.le_selection_count(_handle);
  }

  /// Monotonic counter bumped only on an actual selection change (a
  /// select/deselect/clear that wasn't a no-op) - cheap to check on every
  /// pointer event to decide whether [selectionCount]/[selectedObjectRef]
  /// need re-fetching at all, instead of always re-fetching them (a real,
  /// measured cost that scales with how many objects are selected - see
  /// backend/BENCHMARKS.md).
  @override
  int get selectionVersion {
    _checkNotDisposed();
    return _bindings.le_selection_version(_handle);
  }

  /// The selected object at [selectionIndex] (0..[selectionCount] - 1),
  /// as a generic ref usable with [objectPropertyCount]/[objectPropertyAt]/
  /// [objectParent]/[objectChildren] - always `LE_OBJECT_KIND_SHAPE`
  /// (selection is shape-granular, see backend/src/scene/scene.hpp's own
  /// comment on why). An invalid ref ([LeObjectRef.isValid] false) if
  /// [selectionIndex] is out of range.
  @override
  LeObjectRef selectedObjectRef(int selectionIndex) {
    _checkNotDisposed();
    return _fromNativeRef(
      _bindings.le_selected_object_ref(_handle, selectionIndex),
    );
  }

  /// Number of property rows [ref] has (see [LeSelectedProperty]) -
  /// indexes [objectPropertyAt]'s own `index` parameter, 0..this-1.
  /// Read-only: never mutates the current canvas selection or bumps
  /// [selectionVersion] - safe to call while navigating parent/child
  /// links (see [objectParent]/[objectChildren]) independently of it.
  /// 0 if [ref] doesn't resolve to a real object.
  int objectPropertyCount(LeObjectRef ref) {
    _checkNotDisposed();
    return _bindings.le_object_property_count(_handle, _toNativeRef(ref));
  }

  /// The property row at [index] (0..[objectPropertyCount] - 1) for
  /// [ref] - e.g. for a `LE_OBJECT_KIND_SHAPE` ref, exactly the same rows
  /// `get_properties shape:<id>` shows over TCL. Read-only, same contract
  /// as [objectPropertyCount].
  LeSelectedProperty objectPropertyAt(LeObjectRef ref, int index) {
    _checkNotDisposed();
    final row = _bindings.le_object_property_at(
      _handle,
      _toNativeRef(ref),
      index,
    );
    final type = LePropertyType.fromValue(row.type);
    final Object value = switch (type) {
      LePropertyType.LE_PROPERTY_TYPE_STRING =>
        row.string_value.cast<pkg_ffi.Utf8>().toDartString(),
      LePropertyType.LE_PROPERTY_TYPE_INT => row.int_value,
      LePropertyType.LE_PROPERTY_TYPE_DOUBLE => row.double_value,
    };
    return LeSelectedProperty(
      name: row.name.cast<pkg_ffi.Utf8>().toDartString(),
      type: type,
      value: value,
    );
  }

  /// Every property row for [ref] - see [objectPropertyAt].
  @override
  List<LeSelectedProperty> objectProperties(LeObjectRef ref) {
    _checkNotDisposed();
    final count = objectPropertyCount(ref);
    return [for (var index = 0; index < count; index++) objectPropertyAt(ref, index)];
  }

  /// [ref]'s immediate parent in the database hierarchy (Shape ->
  /// TerminalPort/Obstruction -> Terminal/Abstract -> Design -> Library) -
  /// the same hop graph TCL's `-filter` DSL already uses internally.
  /// Read-only: never mutates the current canvas selection. An invalid
  /// ref if [ref] is a Library (no parent) or doesn't resolve.
  @override
  LeObjectRef objectParent(LeObjectRef ref) {
    _checkNotDisposed();
    return _fromNativeRef(
      _bindings.le_object_parent(_handle, _toNativeRef(ref)),
    );
  }

  /// The children of [ref] in the same database hierarchy [objectParent]
  /// walks upward (Library -> Design -> Abstract -> Terminal/Obstruction
  /// -> TerminalPort/Shape) - a Library's own Designs, a Design's own
  /// Abstracts, an Abstract's own Terminals *and* Obstructions (mixed
  /// kinds in one list - a caller grouping by kind, e.g. the Property
  /// Viewer's one-row-per-kind children display, should partition on
  /// each entry's own [LeObjectRef.kind] rather than assuming a single
  /// kind throughout), a Terminal's own TerminalPorts, or a
  /// TerminalPort/Obstruction's own Shapes. Empty for Shape, the
  /// hierarchy's leaf. Read-only: never mutates the current canvas
  /// selection.
  @override
  List<LeObjectRef> objectChildren(LeObjectRef ref) {
    _checkNotDisposed();
    switch (ref.kind) {
      case LeObjectKind.LE_OBJECT_KIND_LIBRARY:
        final libraryId = ffi.Struct.create<LeLibraryId>()
          ..index = ref.index
          ..generation = ref.generation;
        final count = _bindings.le_get_designs(
          _handle,
          libraryId,
          ffi.nullptr,
          ffi.nullptr,
        );
        return [
          for (var i = 0; i < count; i++)
            _designRef(_bindings.le_search_result_design_at(_handle, i)),
        ];
      case LeObjectKind.LE_OBJECT_KIND_DESIGN:
        final designId = ffi.Struct.create<LeDesignId>()
          ..index = ref.index
          ..generation = ref.generation;
        final count = _bindings.le_get_abstracts(_handle, designId, ffi.nullptr);
        return [
          for (var i = 0; i < count; i++)
            _abstractRef(_bindings.le_search_result_abstract_at(_handle, i)),
        ];
      case LeObjectKind.LE_OBJECT_KIND_ABSTRACT:
        final abstractId = ffi.Struct.create<LeAbstractId>()
          ..index = ref.index
          ..generation = ref.generation;
        final terminalCount = _bindings.le_get_terminals(
          _handle,
          abstractId,
          ffi.nullptr,
          ffi.nullptr,
        );
        final terminals = [
          for (var i = 0; i < terminalCount; i++)
            _terminalRef(_bindings.le_search_result_terminal_at(_handle, i)),
        ];
        final obstructionCount = _bindings.le_get_obstructions(
          _handle,
          abstractId,
          ffi.nullptr,
        );
        final obstructions = [
          for (var i = 0; i < obstructionCount; i++)
            _obstructionRef(
              _bindings.le_search_result_obstruction_at(_handle, i),
            ),
        ];
        return [...terminals, ...obstructions];
      case LeObjectKind.LE_OBJECT_KIND_TERMINAL:
        final terminalId = ffi.Struct.create<LeTerminalId>()
          ..index = ref.index
          ..generation = ref.generation;
        final count = _bindings.le_get_terminal_ports(
          _handle,
          terminalId,
          ffi.nullptr,
        );
        return [
          for (var i = 0; i < count; i++)
            _terminalPortRef(_bindings.le_search_result_terminal_port_at(_handle, i)),
        ];
      case LeObjectKind.LE_OBJECT_KIND_TERMINAL_PORT:
        final portId = ffi.Struct.create<LeTerminalPortId>()
          ..index = ref.index
          ..generation = ref.generation;
        final count = _bindings.le_get_shapes(
          _handle,
          portId,
          _invalidObstructionId,
          ffi.nullptr,
        );
        return [
          for (var i = 0; i < count; i++)
            _shapeRef(_bindings.le_search_result_shape_at(_handle, i)),
        ];
      case LeObjectKind.LE_OBJECT_KIND_OBSTRUCTION:
        final obstructionId = ffi.Struct.create<LeObstructionId>()
          ..index = ref.index
          ..generation = ref.generation;
        final count = _bindings.le_get_shapes(
          _handle,
          _invalidTerminalPortId,
          obstructionId,
          ffi.nullptr,
        );
        return [
          for (var i = 0; i < count; i++)
            _shapeRef(_bindings.le_search_result_shape_at(_handle, i)),
        ];
      case LeObjectKind.LE_OBJECT_KIND_SHAPE:
        return const [];
    }
  }

  LeObjectRef _designRef(LeDesignId id) => LeObjectRef(
    kind: LeObjectKind.LE_OBJECT_KIND_DESIGN,
    index: id.index,
    generation: id.generation,
  );

  LeObjectRef _abstractRef(LeAbstractId id) => LeObjectRef(
    kind: LeObjectKind.LE_OBJECT_KIND_ABSTRACT,
    index: id.index,
    generation: id.generation,
  );

  LeObjectRef _terminalRef(LeTerminalId id) => LeObjectRef(
    kind: LeObjectKind.LE_OBJECT_KIND_TERMINAL,
    index: id.index,
    generation: id.generation,
  );

  LeObjectRef _obstructionRef(LeObstructionId id) => LeObjectRef(
    kind: LeObjectKind.LE_OBJECT_KIND_OBSTRUCTION,
    index: id.index,
    generation: id.generation,
  );

  LeObjectRef _terminalPortRef(LeTerminalPortId id) => LeObjectRef(
    kind: LeObjectKind.LE_OBJECT_KIND_TERMINAL_PORT,
    index: id.index,
    generation: id.generation,
  );

  LeObjectRef _shapeRef(LeShapeId id) => LeObjectRef(
    kind: LeObjectKind.LE_OBJECT_KIND_SHAPE,
    index: id.index,
    generation: id.generation,
  );

  LeObstructionId get _invalidObstructionId =>
      ffi.Struct.create<LeObstructionId>()
        ..index = 0xFFFFFFFF
        ..generation = 0;

  LeTerminalPortId get _invalidTerminalPortId =>
      ffi.Struct.create<LeTerminalPortId>()
        ..index = 0xFFFFFFFF
        ..generation = 0;

  /// Runs the full pipeline+render chain for the currently selected Design
  /// and viewport, returning the resulting frame copied into Dart-owned
  /// memory. The native `LePixelBuffer.data` this copies from is only valid
  /// until the next `le_render_pixel_buffer`/`le_destroy` call on this
  /// handle (see api.hpp) - never cache the native pointer itself, only
  /// this copy.
  ///
  /// Calling this from Dart is for previewing/testing without a platform
  /// texture; the actual on-screen path calls `le_render_pixel_buffer`
  /// directly from native texture code instead (see CLAUDE.md).
  LeFrame renderPixelBuffer() {
    _checkNotDisposed();
    final buffer = _bindings.le_render_pixel_buffer(_handle);
    final byteCount = buffer.row_bytes * buffer.height;
    final pixels = buffer.data == ffi.nullptr || byteCount <= 0
        ? Uint8List(0)
        : Uint8List.fromList(
            buffer.data.cast<ffi.Uint8>().asTypedList(byteCount),
          );
    return LeFrame(
      pixels: pixels,
      width: buffer.width,
      height: buffer.height,
      rowBytes: buffer.row_bytes,
    );
  }

  /// Registers a platform [LeTexture] backed by this handle, for use with
  /// Flutter's `Texture(textureId: ...)` widget - see [LeTexture]'s own doc
  /// for the lifetime and thread-safety constraints this implies. Native
  /// code calls `le_render_pixel_buffer` on this handle directly (not
  /// through this Dart wrapper) whenever the engine pulls a frame, which
  /// only happens after a [LeTexture.markFrameAvailable] call - creating
  /// the texture does not render eagerly on its own.
  @override
  Future<LeTexture> createTexture() async {
    _checkNotDisposed();
    final textureId = await _channel.invokeMethod<int>('createTexture', {
      'handleAddress': nativeHandleAddress,
    });
    if (textureId == null) {
      throw StateError('createTexture returned null');
    }
    return LeTexture._(textureId);
  }

  /// Creates a Tcl console sharing this handle's own database (see
  /// [LeTclConsole] - TCL_EXPLORATION.md's show_gui design): native code
  /// embeds a Tcl interpreter pointed at this same `LeHandle*` via
  /// `set_session_handle`, so every CRUD/search command a caller runs
  /// through the returned [LeTclConsole] mutates the exact database this
  /// [LeEditor] is already rendering - call [LeTexture.markFrameAvailable]
  /// after each command to see the result, same as any other mutation.
  @override
  Future<LeTclConsole> createTclConsole() async {
    _checkNotDisposed();
    final consoleId = await _channel.invokeMethod<int>('createTclConsole', {
      'handleAddress': nativeHandleAddress,
    });
    if (consoleId == null) {
      throw StateError('createTclConsole returned null');
    }
    return LeTclConsole._(consoleId);
  }

  /// Destroys the native handle. Safe to call more than once. Dispose every
  /// [LeTexture] created from this editor first - see [LeTexture]'s own doc.
  void dispose() {
    if (_disposed) return;
    _bindings.le_destroy(_handle);
    _disposed = true;
  }
}

/// A live platform texture backed by one [LeEditor]'s native handle. Obtain
/// via [LeEditor.createTexture]; display with a Flutter
/// `Texture(textureId: texture.textureId)` widget.
///
/// **Lifetime:** the source [LeEditor] must outlive this [LeTexture].
/// Native texture code holds the raw handle address, not a reference that
/// keeps the handle alive, so disposing the [LeEditor] first leaves the
/// texture pointing at freed memory the next time the engine pulls a
/// frame. Always [dispose] every [LeTexture] before disposing the [LeEditor]
/// it came from.
///
/// **Thread safety (unresolved, not just undocumented):** native code pulls
/// a frame - calling `le_render_pixel_buffer` on the shared handle - on the
/// Flutter engine's raster thread, asynchronously relative to whatever
/// thread calls into [LeEditor] from Dart. Backend's Scene/Pipeline/
/// Renderer aren't documented as thread-safe (see backend/CLAUDE.md), so a
/// [LeEditor] setter (`zoom`/`pan`/`setViewportSize`/`fitScene`/
/// `minorGridSpacing`/`majorGridSpacing`/`setMousePosition`/
/// `clearMousePosition`/`keyDown`/`keyUp`/`mouseDown`/`mouseUp`/
/// `setCurrentDesign`) - or a [LeTclConsole.eval] call, e.g. `read_lef` -
/// racing a pending, not-yet-rendered [markFrameAvailable] is a genuine
/// data race, not just a hypothetical
/// one. Not solved at this layer - a real fix needs a lock inside the C
/// API itself, guarding every `le_*` call on a handle regardless of which
/// thread it's called from.
class LeTexture implements LeTextureBase {
  LeTexture._(this.textureId);

  /// Pass to Flutter's `Texture(textureId: ...)` widget.
  @override
  final int textureId;

  bool _disposed = false;

  /// Tells the engine to pull a fresh frame on the raster thread - call
  /// after any change on the source [LeEditor] that should become visible.
  /// Cheap to call even when nothing actually changed (see
  /// `le_render_pixel_buffer`'s own caching note in api.hpp).
  @override
  Future<void> markFrameAvailable() async {
    if (_disposed) return;
    await _channel.invokeMethod<void>('markTextureFrameAvailable', {
      'textureId': textureId,
    });
  }

  /// Unregisters the texture. Safe to call more than once. Call this
  /// before disposing the source [LeEditor], not after - see this class's
  /// own Lifetime doc.
  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    await _channel.invokeMethod<void>('disposeTexture', {
      'textureId': textureId,
    });
  }
}

/// A live Tcl console backed by one [LeEditor]'s native handle - obtain via
/// [LeEditor.createTclConsole]. Every [eval] call runs synchronously
/// (from Tcl's own point of view) on the native platform thread, against
/// an embedded `Tcl_Interp` sharing the source [LeEditor]'s own database
/// (see TCL_EXPLORATION.md's show_gui design) - a command that mutates the
/// database (`create_terminal`, `delete_shape`, ...) doesn't refresh the
/// screen on its own; call [LeTexture.markFrameAvailable] afterward, same
/// as any other mutating action.
///
/// **Lifetime:** the source [LeEditor] must outlive this [LeTclConsole] -
/// same constraint as [LeTexture], and for the same reason (native code
/// holds the raw handle address, not a reference keeping it alive).
class LeTclConsole implements LeTclConsoleBase {
  LeTclConsole._(this._consoleId);

  final int _consoleId;
  bool _disposed = false;

  /// Evaluates one Tcl command, returning the interpreter's string result
  /// whether the command succeeded or failed - Tcl already puts the error
  /// message in the same place on failure, so a caller checks the text
  /// itself rather than a separate success flag, matching how a real
  /// interactive Tcl shell prints either case. Also includes any text the
  /// command wrote via `puts` (stdout or stderr), ahead of the
  /// interpreter's own result - `puts` inside this console's interpreter
  /// never reaches this app's real stdout/stderr (see macOS's
  /// LeTclBridge.evalTcl:), so this is the only place a script's own
  /// output is observable at all.
  @override
  Future<String> eval(String command) async {
    if (_disposed) {
      throw StateError('eval called on a disposed LeTclConsole');
    }
    final result = await _channel.invokeMethod<String>('evalTclCommand', {
      'consoleId': _consoleId,
      'command': command,
    });
    return result ?? '';
  }

  /// Destroys the native Tcl interpreter. Safe to call more than once.
  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    await _channel.invokeMethod<void>('disposeTclConsole', {
      'consoleId': _consoleId,
    });
  }
}
