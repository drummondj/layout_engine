import 'dart:async';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:layout_engine_plugin/layout_engine_plugin.dart';
import 'package:synchronized/synchronized.dart';

class LeLayerInfo {
  int index;
  LeLayer layer;
  bool isSelectable;
  bool isVisible;

  LeLayerInfo({
    required this.index,
    required this.layer,
    required this.isSelectable,
    required this.isVisible,
  });
}

class LePurposeInfo {
  LeLayerPurpose purpose;
  bool isSelectable;
  bool isVisible;
  LePurposeInfo({
    required this.purpose,
    required this.isSelectable,
    required this.isVisible,
  });
}

class LeProvider extends ChangeNotifier {
  final Lock _lock = Lock();

  // [LeEditor]'s own constructor eagerly calls dart:ffi's
  // DynamicLibrary.open, which only resolves inside a real built app
  // bundle - never under plain `flutter test`'s bare Dart VM (confirmed:
  // simply constructing a default LeProvider() there throws before any
  // test body even runs). Depending on LeEditorBase (LeEditor's own
  // public interface - see its doc comment) instead of the concrete
  // LeEditor lets a test inject a fake implementing that interface,
  // sidestepping the real FFI/platform-channel calls entirely - see
  // frontend/test/fakes/fake_le_editor.dart.
  LeProvider({LeEditorBase? editor}) : _editor = editor ?? LeEditor() {
    _messageStream = _messageStreamController.stream;
  }

  final double panFactor = 0.25;
  final LeEditorBase _editor;

  LeTextureBase? _texture;
  LeTextureBase? get texture => _texture;

  final List<String> _openLefFiles = [];

  final StreamController<String> _messageStreamController =
      StreamController<String>.broadcast();

  late final Stream<String> _messageStream;

  final List<LeLayerInfo> _layers = [];
  List<LeLayerInfo> get layers => _layers;

  final List<LePurposeInfo> _layerPurposes = [];
  List<LePurposeInfo> get layerPurposes => _layerPurposes;

  bool _allVisible = true;
  bool get allVisible => _allVisible;

  bool _allSelectable = true;
  bool get allSelectable => _allSelectable;

  bool _allLayersVisible = true;
  bool get allLayersVisible => _allLayersVisible;

  bool _allLayersSelectable = true;
  bool get allLayersSelectable => _allLayersSelectable;

  bool _allPurposesVisible = true;
  bool get allPurposesVisible => _allPurposesVisible;

  bool _allPurposesSelectable = true;
  bool get allPurposesSelectable => _allPurposesSelectable;

  Offset _snappedMousePosition = Offset.zero;
  Offset get snappedMousePosition => _snappedMousePosition;

  int _selectedCount = 0;
  int get selectedCount => _selectedCount;

  String _tooltipMessage = '';
  String get tooltipMessage => _tooltipMessage;

  LeMode _mode = LeMode.LE_MODE_SELECT;
  LeMode get mode => _mode;

  bool _moveArmed = false;
  bool get moveArmed => _moveArmed;

  int _hierDepth = 0;
  int get hierDepth => _hierDepth;

  // Command-recall log (UPDATES.md item 21, migrated from Terminal's own
  // local _commandHistory list) - cached here (rather than re-fetched on
  // every Up/Down press) so Terminal's own recall navigation can stay
  // synchronous. Same incremental-fetch shape as _lastMessageCount/
  // refreshMessages above: entries are never removed/reordered, so this
  // just needs to remember how many have already been pulled in.
  final List<String> _commandHistoryCache = [];
  List<String> get commandHistoryCache => _commandHistoryCache;
  int _lastCommandHistoryCount = 0;

  final List<LeObjectRef> _selectedObjects = [];
  List<LeObjectRef> get selectedObjects => _selectedObjects;

  // -1 so the first refreshSelection() call (selectionVersion is always
  // >= 0) always does a real refresh.
  int _lastSelectionVersion = -1;

  // Cursor into the backend's own message queue (le_message_count/
  // le_message_at) - messages there are never removed/reordered, so
  // this just needs to remember how many we've already pulled in.
  // Starts at 0 (not -1 like _lastSelectionVersion): messageCount is
  // also 0 before any backend operation has run, so "drain nothing yet"
  // is already the correct behavior with no special sentinel needed.
  int _lastMessageCount = 0;

  Future<void> refreshTexture() async {
    await _texture?.markFrameAvailable();
  }

  // Lazily created on first use, reused across every runTclCommand call -
  // a fresh LeTclConsole per command would mean a fresh Tcl_Interp per
  // command too (variables/state set by one command wouldn't survive to
  // the next), not what a console/REPL experience wants.
  LeTclConsoleBase? _tclConsole;

  // Drives the status bar's spinner (BUGS_AND_ENHANCEMENTS.md item E3) -
  // true for the duration of any runTclCommand call, success or failure.
  bool _isRunning = false;
  bool get isRunning => _isRunning;

  /// Runs one Tcl command against this provider's own editor (see
  /// TCL_EXPLORATION.md's show_gui design - LeTclConsole shares the same
  /// LeHandle this provider's texture already renders), returning the
  /// interpreter's result text. [onOutput], if given, is called with each
  /// chunk of `puts` text as the command emits it, ahead of the final
  /// result - see [LeTclConsoleBase.eval]'s own doc comment. Refreshes
  /// everything refreshAndNotify() already does afterward, same as any
  /// other mutating action - a Tcl command can change anything from design
  /// content to layer names, so there's no narrower refresh worth
  /// hand-picking here.
  Future<String> runTclCommand(String command, {void Function(String)? onOutput}) async {
    _tclConsole ??= await _editor.createTclConsole();
    _isRunning = true;
    notifyListeners();
    try {
      final result = await _tclConsole!.eval(command, onOutput: onOutput);
      // Set before refreshAndNotify() (rather than in a shared `finally`
      // below) so that call's own notifyListeners() already reflects
      // isRunning being false - same single notify, no extra one needed.
      _isRunning = false;
      refreshAndNotify();
      return result;
    } catch (e) {
      // Deliberately not refreshAndNotify() here - same contract as
      // before this method could throw partway through (see readLef's own
      // comment on this), just with isRunning also reset so the spinner
      // can't get stuck on.
      _isRunning = false;
      notifyListeners();
      rethrow;
    }
  }

  Future<void> refreshSnappedMousePosition() async {
    if (_editor.snappedMousePosition != null) {
      _snappedMousePosition = Offset(
        _editor.snappedMousePosition!.xUm,
        _editor.snappedMousePosition!.yUm,
      );
    }
  }

  // A cheap direct read (no version-gating needed, unlike
  // refreshSelection/refreshMessages - there's no list to accumulate
  // and nothing costly to skip).
  void refreshTooltipMessage() {
    _tooltipMessage = _editor.tooltipMessage;
  }

  // Same "cheap direct read" shape as refreshTooltipMessage above.
  void refreshMode() {
    _mode = _editor.mode;
  }

  // Same "cheap direct read" shape as refreshTooltipMessage/refreshMode
  // above - for the Move toolbox button's own armed/pressed visual state.
  void refreshMoveArmed() {
    _moveArmed = _editor.isMoveArmed;
  }

  // Same incremental-fetch shape as refreshMessages above.
  void refreshCommandHistory() {
    final count = _editor.commandHistoryCount;
    for (int i = _lastCommandHistoryCount; i < count; i++) {
      _commandHistoryCache.add(_editor.commandHistoryAt(i));
    }
    _lastCommandHistoryCount = count;
  }

  // Rebuilds _selectedObjects as a flat list of refs only - one FFI call
  // per selected object (selectedObjectRef), not one-plus-per-property the
  // way this used to fan out: the Property Viewer now fetches a ref's
  // properties itself, live, only for whichever ref it's currently
  // showing (see property_viewer.dart), not eagerly for every selected
  // object on every refresh. Gated on selectionVersion (bumped only on an
  // actual select/deselect/clear, not on every pointer event) so a mouse
  // move that doesn't change the selection skips this entirely instead of
  // paying that cost on every frame - see backend/BENCHMARKS.md for the
  // measured cost this fixes.
  Future<void> refreshSelection() async {
    final version = _editor.selectionVersion;
    if (version == _lastSelectionVersion) {
      return;
    }
    _lastSelectionVersion = version;

    _selectedCount = _editor.selectionCount;
    _selectedObjects.clear();
    for (int i = 0; i < _selectedCount; i++) {
      _selectedObjects.add(_editor.selectedObjectRef(i));
    }
  }

  // Read-only pass-throughs for the Property Viewer's database-hierarchy
  // navigation (UPDATES.md 7.2) - deliberately *not* wrapped in
  // refreshAndNotify()/notifyListeners() the way every mutating method on
  // this provider is: navigating via a parent/child link must never
  // affect canvas selection or trigger a provider-wide rebuild of
  // anything besides the Property Viewer itself (see property_viewer.dart's
  // own `_breadcrumb` state, which calls these directly via plain
  // `setState`, not through this provider's notify cycle).
  List<LeSelectedProperty> objectProperties(LeObjectRef ref) =>
      _editor.objectProperties(ref);

  LeObjectRef objectParent(LeObjectRef ref) => _editor.objectParent(ref);

  List<LeObjectRef> objectChildren(LeObjectRef ref) =>
      _editor.objectChildren(ref);

  // Pulls in any backend messages (LEF read errors/warnings/info - see
  // backend's le_message_count/le_message_at, UPDATES.md item 3) added
  // since the last check, in order, without re-adding ones already
  // seen - same incremental-fetch shape as refreshSelection's own
  // selectionVersion gate above, just for a queue instead of a
  // change-counter.
  Future<void> refreshMessages() async {
    final count = _editor.messageCount;
    for (int i = _lastMessageCount; i < count; i++) {
      final message = _editor.messageAt(i);
      if (message != null) {
        _messageStreamController.add(message);
      }
    }
    _lastMessageCount = count;
  }

  StreamSubscription<String> addMessageListener(Function(String) callback) {
    var subscription = _messageStream.listen(callback);
    _messageStreamController.add("Layout Engine: Version 0.1.0");
    return subscription;
  }

  // Same "cheap direct read" shape as refreshMode/refreshMoveArmed above -
  // for the Hierarchy Depth field in the layer manager (HierarchyRow).
  void refreshHierDepth() {
    _hierDepth = _editor.hierarchyDepth;
  }

  // TODO: this currently refreshed everything all the time (except
  // selection, see refreshSelection), choose what to refresh more
  // carefully.
  void refreshAndNotify() {
    refreshSnappedMousePosition();
    refreshSelection();
    refreshLayers();
    refreshMessages();
    refreshTooltipMessage();
    refreshMode();
    refreshMoveArmed();
    refreshHierDepth();
    refreshCommandHistory();
    refreshTexture();
    notifyListeners();
  }

  /// Sets how many further levels of Placement -> Design a Layout view
  /// recurses into before a placed instance falls back to its own
  /// Abstract (see LeEditorBase.hierarchyDepth) - the HierarchyRow field.
  Future<void> setHierDepth(int depth) async {
    _editor.setHierarchyDepth(depth);
    refreshAndNotify();
  }

  /// Switches the current interaction mode (UPDATES.md item 11) - also
  /// reachable via the 's'/'e'/'r' keyboard shortcuts (see handleKeyEvent).
  Future<void> setMode(LeMode mode) async {
    _editor.setMode(mode);
    refreshAndNotify();
  }

  /// Removes every ruler, finished or not (UPDATES.md item 13).
  Future<void> clearRulers() async {
    _editor.clearRulers();
    refreshAndNotify();
  }

  /// Undoes the most recently recorded transaction, if any - a typed Tcl
  /// command or a GUI edit like Move (UPDATES.md item 21). Also reachable
  /// via Ctrl-Z (see handleKeyEvent).
  Future<void> undo() async {
    _editor.undo();
    refreshAndNotify();
  }

  /// Redoes the most recently undone transaction, if any. Also reachable
  /// via Ctrl-Shift-Z.
  Future<void> redo() async {
    _editor.redo();
    refreshAndNotify();
  }

  /// Arms Move (UPDATES.md item 21) - the Move toolbox button's own
  /// handler. Only meaningful in Edit mode with a non-empty selection;
  /// a no-op otherwise. Also reachable via Ctrl-M.
  Future<void> armMove() async {
    _editor.armMove();
    refreshAndNotify();
  }

  /// Selects every currently selectable shape in the current Abstract -
  /// the Select-mode toolbox button's own handler (UPDATES.md item 21).
  Future<void> selectAll() async {
    _editor.selectAll();
    refreshAndNotify();
  }

  /// Clears the current selection - the Select-mode toolbox button's own
  /// handler.
  Future<void> deselectAll() async {
    _editor.deselectAll();
    refreshAndNotify();
  }

  Future<void> init() async {
    _texture = await _editor.createTexture();
    refreshAndNotify();
  }

  Future<void> resize(Size size) async {
    _editor.setViewportSize(size.width.toInt(), size.height.toInt());
    refreshAndNotify();
  }

  // Goes through the Tcl `read_lef` command (via runTclCommand), not
  // _editor.readLef()/le_read_lef directly - Tcl's own read_lef is meant
  // to be the only way a LEF file gets read into this provider's handle,
  // so anything driven from the UI stays consistent with (and visible to)
  // whatever a user also runs by hand through the Terminal (e.g.
  // current_technology getting selected only happens inside le_read_lef
  // itself, so both paths already share that regardless - this is about
  // keeping one entry point, not working around a behavior gap). Braces
  // around $path guard against Tcl word-splitting/substitution on
  // whitespace or special characters in the path - safe for any path
  // short of one containing a literal, unescaped '{' or '}', which a real
  // filesystem path essentially never does.
  Future<void> readLef(String path) async {
    // Awaited (not fire-and-forget) so a caller can actually tell when the
    // read finished, and wrapped in try/catch: without both, an exception
    // thrown before runTclCommand's own refreshAndNotify runs (e.g. a
    // MissingPluginException on a platform whose native side doesn't
    // implement the Tcl console channel) used to vanish as a silently
    // swallowed/unhandled async error - "INFO: Reading $path" would print
    // and then nothing else ever would, with no error surfaced anywhere
    // and the Browser/Layer Manager left permanently stale. Found via a
    // real repro on Linux (createTclConsole not yet implemented there).
    await _lock.synchronized(() async {
      if (_openLefFiles.contains(path)) {
        _messageStreamController.add("ERROR: $path already open");
        refreshAndNotify();
        return;
      }
      _messageStreamController.add("INFO: Reading $path");
      try {
        final result = await runTclCommand('read_lef {$path}');
        // On failure, the backend's own le_message_count/le_message_at
        // queue already has a more specific error (e.g. "ERROR: Could not
        // open LEF file ..." or a real parser diagnostic) - runTclCommand's
        // own refreshAndNotify (via refreshMessages) already pulled it in,
        // so no generic fallback message is added here.
        if (int.tryParse(result) == 0) {
          _openLefFiles.add(path);
        }
      } catch (e) {
        _messageStreamController.add("ERROR: $path: $e");
        refreshAndNotify();
      }
    });
  }

  Future<List<LeLibrary>> getLibraries() async {
    List<LeLibrary> libraries = [];
    for (int i = 0; i < _editor.libraryCount; i++) {
      var library = _editor.library(i);
      if (library != null) {
        libraries.add(library);
      }
    }
    return libraries;
  }

  Future<List<LeDesignEntry>> getDesigns(int libraryIndex) async {
    List<LeDesignEntry> designs = [];
    for (int i = 0; i < _editor.libraryDesignCount(libraryIndex); i++) {
      var design = _editor.libraryDesign(libraryIndex, i);
      if (design != null) {
        designs.add(design);
      }
    }
    return designs;
  }

  /// Opens `designRef`'s Abstract view (BUGS_AND_ENHANCEMENTS.md E15 - see
  /// [openDesignLayout] for the mirror-image Layout-view opener).
  Future<void> openDesign(LeDesignRef designRef) async {
    _editor.setCurrentDesignById(designRef);
    _editor.fitScene(10);
    refreshAndNotify();
  }

  /// Opens `designRef`'s Layout view instead of its Abstract view - only
  /// meaningful when [LeDesignEntry.layoutId] is non-null for this Design
  /// (BUGS_AND_ENHANCEMENTS.md E15).
  Future<void> openDesignLayout(LeDesignRef designRef) async {
    _editor.setCurrentDesignByIdLayout(designRef);
    _editor.fitScene(10);
    refreshAndNotify();
  }

  Future<void> refreshLayers() async {
    _layers.clear();
    _layerPurposes.clear();
    _allLayersSelectable = true;
    _allLayersVisible = true;
    _allPurposesSelectable = true;
    _allPurposesVisible = true;

    for (int i = 0; i < _editor.layerCount; i++) {
      var layer = _editor.layer(i);
      // Pseudo-rows with no physical Technology Layer of their own
      // (ROW/BOUNDARY/GCELLGRID/PLACEMENT_BLOCKAGE/REGION) already have
      // their own single-purpose entry below - showing them again here
      // as if they were a whole extra layer is a redundant, confusing
      // duplicate for the Layer Manager's own Layers section, not useful
      // extra information (BUGS_AND_ENHANCEMENTS.md E12). Excluded from
      // _allLayersSelectable/_allLayersVisible too, same reasoning -
      // those aggregate the Layers section alone now, the Purposes
      // section's own "All Purposes" toggle already covers this row via
      // its one purpose.
      if (layer != null && layer.hasPhysicalLayer) {
        bool isSelectable = _editor.isLayerNameSelectable(layer.name);
        bool isVisible = _editor.isLayerNameVisible(layer.name);
        var layerInfo = LeLayerInfo(
          index: i,
          layer: layer,
          isSelectable: isSelectable,
          isVisible: isVisible,
        );
        _layers.add(layerInfo);
        if (!isSelectable) {
          _allLayersSelectable = false;
        }
        if (!isVisible) {
          _allLayersVisible = false;
        }
      }
    }
    for (int i = 0; i < _editor.purposeCount; i++) {
      var purpose = _editor.purposeAt(i);
      if (purpose != null) {
        bool isSelectable = _editor.isPurposeSelectable(purpose);
        bool isVisible = _editor.isPurposeVisible(purpose);
        var purposeInfo = LePurposeInfo(
          purpose: purpose,
          isSelectable: isSelectable,
          isVisible: isVisible,
        );
        _layerPurposes.add(purposeInfo);
        if (!isSelectable) {
          _allPurposesSelectable = false;
        }
        if (!isVisible) {
          _allPurposesVisible = false;
        }
      }
    }
    _allVisible = _allLayersVisible && _allPurposesVisible;
    _allSelectable = _allLayersSelectable && _allLayersSelectable;

    notifyListeners();
  }

  Future<void> setLayerVisibility(LeLayerInfo layerInfo, bool? visible) async {
    _editor.setLayerNameVisible(layerInfo.layer.name, visible ?? false);
    refreshAndNotify();
  }

  Future<void> setLayerSelectable(
    LeLayerInfo layerInfo,
    bool? selectable,
  ) async {
    _editor.setLayerNameSelectable(layerInfo.layer.name, selectable ?? false);
    refreshAndNotify();
  }

  Future<void> setPurposeVisible(
    LePurposeInfo purposeInfo,
    bool? visible,
  ) async {
    _editor.setPurposeVisible(purposeInfo.purpose, visible ?? false);
    refreshAndNotify();
  }

  Future<void> setPurposeSelectable(
    LePurposeInfo purposeInfo,
    bool? visible,
  ) async {
    _editor.setPurposeSelectable(purposeInfo.purpose, visible ?? false);
    refreshAndNotify();
  }

  Future<void> setAllLayersVisible(bool? visible) async {
    for (var layerInfo in _layers) {
      await setLayerVisibility(layerInfo, visible);
    }
  }

  Future<void> setAllLayersSelectable(bool? selectable) async {
    for (var layerInfo in _layers) {
      await setLayerSelectable(layerInfo, selectable);
    }
  }

  Future<void> setAllPurposesVisible(bool? visible) async {
    for (var purposeInfo in _layerPurposes) {
      await setPurposeVisible(purposeInfo, visible);
    }
  }

  Future<void> setAllPurposesSelectable(bool? selectable) async {
    for (var purposeInfo in _layerPurposes) {
      await setPurposeSelectable(purposeInfo, selectable);
    }
  }

  Future<void> setAllVisible(bool? visible) async {
    await setAllLayersVisible(visible);
    await setAllPurposesVisible(visible);
  }

  Future<void> setAllSelectable(bool? selectable) async {
    await setAllLayersSelectable(selectable);
    await setAllPurposesSelectable(selectable);
  }

  void handlePointerEvent(PointerEvent event) {
    _editor.handlePointerEvent(event);
    refreshAndNotify();
  }

  void handlePointerSignal(PointerSignalEvent event) {
    _editor.handlePointerSignal(event);
    refreshAndNotify();
  }

  bool handleKeyEvent(KeyEvent event) {
    if (_editor.handleKeyEvent(event)) {
      refreshAndNotify();
      return true;
    } else {
      return false;
    }
  }

  void handleFocusChange(bool hasFocus) {
    _editor.handleFocusChange(hasFocus);
  }
}
