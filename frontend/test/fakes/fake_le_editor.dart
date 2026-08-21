import 'package:layout_engine_plugin/layout_engine_plugin.dart';

/// A [LeTextureBase] that does nothing - no real texture id, no channel
/// call. Good enough for anything that only needs `provider.texture` to
/// be non-null (e.g. layout_engine.dart's `Texture(textureId: ...)`
/// build check) or to call [markFrameAvailable] without caring what
/// happens.
class FakeTexture implements LeTextureBase {
  int markFrameAvailableCallCount = 0;

  @override
  final int textureId = 0;

  @override
  Future<void> markFrameAvailable() async {
    markFrameAvailableCallCount++;
  }
}

/// A [LeTclConsoleBase] whose [eval] result (or thrown error) a test
/// controls directly via [onEval] - defaults to "0" (success, matching
/// how `LeProvider.readLef` checks `int.tryParse(result) == 0`).
class FakeTclConsole implements LeTclConsoleBase {
  FakeTclConsole({this.onEval});

  /// Called for every [eval] - override to return a specific result, or
  /// throw to simulate a native-side failure (e.g. the real
  /// MissingPluginException this session found on Linux before
  /// linux/layout_engine_plugin.cc implemented createTclConsole).
  Future<String> Function(String command)? onEval;

  final List<String> evaluatedCommands = [];

  @override
  Future<String> eval(String command) async {
    evaluatedCommands.add(command);
    return onEval == null ? '0' : await onEval!(command);
  }
}

/// A fully in-memory [LeEditorBase] - no FFI, no MethodChannel, no native
/// code anywhere in its call graph, so it's safe to construct in a plain
/// `flutter test` (unlike the real [LeEditor] - see [LeEditorBase]'s own
/// doc comment for why that matters). Every getter/setter here is backed
/// by a plain mutable field a test can poke directly before/after
/// pumping, rather than going through the real backend's own semantics
/// (e.g. [setLayerNameVisible] doesn't affect rendering - there's no
/// renderer here at all).
class FakeLeEditor implements LeEditorBase {
  // --- Tcl console / texture ---

  /// Set by a test to make [createTclConsole] throw instead of
  /// succeeding - e.g. `MissingPluginException('...')`, reproducing the
  /// real Linux gap this session found (see le_provider.dart's readLef()
  /// comment).
  Object? tclConsoleCreationError;

  /// Set by a test *before* the call that triggers console creation (e.g.
  /// before [readLef]) to control every [eval] result on the console
  /// [createTclConsole] is about to construct - LeProvider creates its
  /// own console lazily and caches it internally, so there's no handle to
  /// configure [FakeTclConsole.onEval] on after the fact.
  Future<String> Function(String command)? onTclEval;

  /// The most recently created console, if any - for a test to inspect
  /// afterward (e.g. [FakeTclConsole.evaluatedCommands]).
  FakeTclConsole? lastTclConsole;

  final FakeTexture texture = FakeTexture();

  @override
  Future<LeTclConsoleBase> createTclConsole() async {
    if (tclConsoleCreationError != null) {
      throw tclConsoleCreationError!;
    }
    return lastTclConsole = FakeTclConsole(onEval: onTclEval);
  }

  @override
  Future<LeTextureBase> createTexture() async => texture;

  // --- Layers / purposes ---

  List<LeLayer> layers = [];
  final Map<String, bool> layerVisibility = {};
  final Map<String, bool> layerSelectability = {};

  List<LeLayerPurpose> purposes = [];
  final Map<LeLayerPurpose, bool> purposeVisibility = {};
  final Map<LeLayerPurpose, bool> purposeSelectability = {};

  @override
  int get layerCount => layers.length;

  @override
  LeLayer? layer(int rowIndex) =>
      rowIndex >= 0 && rowIndex < layers.length ? layers[rowIndex] : null;

  @override
  int get purposeCount => purposes.length;

  @override
  LeLayerPurpose? purposeAt(int index) =>
      index >= 0 && index < purposes.length ? purposes[index] : null;

  @override
  bool isLayerNameVisible(String layerName) => layerVisibility[layerName] ?? true;

  @override
  void setLayerNameVisible(String layerName, bool visible) {
    layerVisibility[layerName] = visible;
  }

  @override
  bool isLayerNameSelectable(String layerName) => layerSelectability[layerName] ?? true;

  @override
  void setLayerNameSelectable(String layerName, bool selectable) {
    layerSelectability[layerName] = selectable;
  }

  @override
  bool isPurposeVisible(LeLayerPurpose purpose) => purposeVisibility[purpose] ?? true;

  @override
  void setPurposeVisible(LeLayerPurpose purpose, bool visible) {
    purposeVisibility[purpose] = visible;
  }

  @override
  bool isPurposeSelectable(LeLayerPurpose purpose) => purposeSelectability[purpose] ?? true;

  @override
  void setPurposeSelectable(LeLayerPurpose purpose, bool selectable) {
    purposeSelectability[purpose] = selectable;
  }

  // --- Libraries / designs ---

  List<LeLibrary> libraries = [];
  Map<int, List<LeDesignEntry>> designsByLibraryIndex = {};
  LeDesignRef? currentDesignId;

  @override
  int get libraryCount => libraries.length;

  @override
  LeLibrary? library(int index) =>
      index >= 0 && index < libraries.length ? libraries[index] : null;

  @override
  int libraryDesignCount(int libraryIndex) => designsByLibraryIndex[libraryIndex]?.length ?? 0;

  @override
  LeDesignEntry? libraryDesign(int libraryIndex, int designIndex) {
    final designs = designsByLibraryIndex[libraryIndex];
    if (designs == null || designIndex < 0 || designIndex >= designs.length) return null;
    return designs[designIndex];
  }

  @override
  bool setCurrentDesignById(LeDesignRef designId) {
    currentDesignId = designId;
    return true;
  }

  @override
  void fitScene(int paddingPx) {}

  @override
  void setViewportSize(int widthPx, int heightPx) {}

  // --- Mode / editing ---

  LeMode _mode = LeMode.LE_MODE_SELECT;
  @override
  LeMode get mode => _mode;
  @override
  void setMode(LeMode mode) => _mode = mode;

  @override
  void clearRulers() {}

  bool _isMoveArmed = false;
  @override
  bool get isMoveArmed => _isMoveArmed;
  @override
  void armMove() => _isMoveArmed = true;

  bool undoCalled = false;
  @override
  bool undo() {
    undoCalled = true;
    return false;
  }

  bool redoCalled = false;
  @override
  bool redo() {
    redoCalled = true;
    return false;
  }

  final List<String> commandHistory = [];
  @override
  int get commandHistoryCount => commandHistory.length;
  @override
  String commandHistoryAt(int index) => commandHistory[index];

  bool selectAllCalled = false;
  @override
  void selectAll() => selectAllCalled = true;

  bool deselectAllCalled = false;
  @override
  void deselectAll() => deselectAllCalled = true;

  // --- Selection / properties ---

  @override
  int selectionCount = 0;
  @override
  int selectionVersion = 0;
  List<LeObjectRef> selectedObjects = [];
  Map<LeObjectRef, List<LeSelectedProperty>> propertiesByRef = {};
  Map<LeObjectRef, LeObjectRef> parentByRef = {};
  Map<LeObjectRef, List<LeObjectRef>> childrenByRef = {};

  @override
  LeObjectRef selectedObjectRef(int selectionIndex) => selectedObjects[selectionIndex];

  @override
  List<LeSelectedProperty> objectProperties(LeObjectRef ref) => propertiesByRef[ref] ?? const [];

  @override
  LeObjectRef objectParent(LeObjectRef ref) =>
      parentByRef[ref] ??
      const LeObjectRef(
        kind: LeObjectKind.LE_OBJECT_KIND_SHAPE,
        index: 0xFFFFFFFF,
        generation: 0,
      );

  @override
  List<LeObjectRef> objectChildren(LeObjectRef ref) => childrenByRef[ref] ?? const [];

  // --- Messages ---

  final List<String> messages = [];
  @override
  int get messageCount => messages.length;
  @override
  String? messageAt(int index) => index >= 0 && index < messages.length ? messages[index] : null;

  // --- Mouse / snap / tooltip ---

  LeSnappedMouse? snappedMousePositionValue;
  @override
  LeSnappedMouse? get snappedMousePosition => snappedMousePositionValue;

  String tooltipMessageValue = '';
  @override
  String get tooltipMessage => tooltipMessageValue;

  @override
  void setMousePosition(int x, int y) {}
  @override
  void clearMousePosition() {}
  @override
  void mouseDown(int x, int y) {}
  @override
  void mouseUp(int x, int y) {}
  @override
  void zoom(double factor, int x, int y) {}
  @override
  void zoomDragDown(int x, int y) {}

  // --- Keyboard ---

  final Set<LeKeyCode> heldKeys = {};
  @override
  void keyDown(LeKeyCode keyCode) => heldKeys.add(keyCode);
  @override
  void keyUp(LeKeyCode keyCode) => heldKeys.remove(keyCode);
  @override
  void clearAllKeys() => heldKeys.clear();
}
