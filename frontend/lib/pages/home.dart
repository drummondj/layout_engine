import 'dart:async';

import 'package:docking/docking.dart';
import 'package:flutter/material.dart';
import 'package:layout_engine/components/layer_manager.dart';
import 'package:layout_engine/components/layout_engine.dart';
import 'package:layout_engine/components/library_browser.dart';
import 'package:layout_engine/components/main_menu.dart';
import 'package:layout_engine/components/property_viewer.dart';
import 'package:layout_engine/components/status_bar.dart';
import 'package:layout_engine/components/terminal.dart';
import 'package:layout_engine/components/toolbars/mode_selector.dart';
import 'package:layout_engine/components/toolbars/mode_toolbar.dart';
import 'package:layout_engine/components/widget_card.dart';
import 'package:shared_preferences/shared_preferences.dart';

// Ids are Strings, so the default id<->String conversion is a plain no-op.
class _IdLayoutParser extends LayoutParser with LayoutParserMixin {
  const _IdLayoutParser();
}

class _HomeAreaBuilder extends AreaBuilder with AreaBuilderMixin {
  _HomeAreaBuilder(this.buildItem);

  final DockingItem Function({
    required dynamic id,
    double? weight,
    bool maximized,
  })
  buildItem;

  @override
  DockingItem buildDockingItem({
    required dynamic id,
    required double? weight,
    required bool maximized,
  }) {
    return buildItem(id: id, weight: weight, maximized: maximized);
  }
}

class Home extends StatefulWidget {
  const Home({super.key});

  @override
  State<Home> createState() => _HomeState();
}

class _HomeState extends State<Home> {
  static const String _layoutPrefsKey = 'docking_layout_v1';
  static const _IdLayoutParser _layoutParser = _IdLayoutParser();

  late final DockingLayout _layout;
  SharedPreferences? _prefs;
  Timer? _saveDebounce;

  @override
  void initState() {
    super.initState();
    _layout = _buildDefaultLayout();
    _layout.addListener(_scheduleSave);
    _restoreLayout();
  }

  @override
  void dispose() {
    _layout.removeListener(_scheduleSave);
    _saveDebounce?.cancel();
    _layout.dispose();
    super.dispose();
  }

  // The single source of truth for each panel's identity (name, widget,
  // closable/maximizable rules), keyed by the stable id also used to
  // persist/restore the layout. Used both for the hardcoded default tree
  // and to reconstruct panels from a saved layout string.
  DockingItem _dockingItem({
    required dynamic id,
    double? weight,
    bool maximized = false,
  }) {
    switch (id) {
      case 'file':
        return DockingItem(
          id: id,
          name: 'File',
          widget: WidgetCard(child: MainMenu()),
          closable: false,
          keepAlive: true,
          maximizable: false,
          weight: weight,
        );
      case 'browser':
        return DockingItem(
          id: id,
          name: 'Browser',
          widget: WidgetCard(child: LibraryBrowser()),
          closable: false,
          keepAlive: true,
          maximizable: false,
          weight: weight,
        );
      case 'layout':
        return DockingItem(
          id: id,
          name: 'Layout',
          widget: WidgetCard(
            child: Row(
              children: [
                ModeSelector(),
                Expanded(
                  child: Column(
                    children: [
                      ModeToolbar(),
                      Expanded(child: LayoutEngine()),
                      StatusBar(),
                    ],
                  ),
                ),
              ],
            ),
          ),
          closable: false,
          keepAlive: true,
          weight: weight,
          maximized: maximized,
        );
      case 'console':
        return DockingItem(
          id: id,
          name: 'Console',
          widget: WidgetCard(child: Terminal()),
          closable: false,
          keepAlive: true,
          minimalSize: 300,
          weight: weight ?? 0.2,
          maximized: maximized,
        );
      case 'layers':
        return DockingItem(
          id: id,
          name: 'Layers',
          widget: WidgetCard(child: LayerManager()),
          closable: false,
          keepAlive: true,
          weight: weight,
          maximized: maximized,
        );
      case 'properties':
        return DockingItem(
          id: id,
          name: 'Properties',
          widget: WidgetCard(child: PropertyViewer()),
          closable: false,
          keepAlive: true,
          weight: weight,
          maximized: maximized,
        );
      default:
        throw ArgumentError('Unknown DockingItem id: $id');
    }
  }

  DockingLayout _buildDefaultLayout() {
    return DockingLayout(
      root: DockingRow([
        DockingTabs(size: 300, minimalSize: 300, [
          _dockingItem(id: 'file'),
          _dockingItem(id: 'browser'),
        ]),
        DockingColumn([
          _dockingItem(id: 'layout'),
          _dockingItem(id: 'console'),
        ]),
        DockingTabs(size: 300, minimalSize: 300, [
          _dockingItem(id: 'layers'),
          _dockingItem(id: 'properties'),
        ]),
      ]),
    );
  }

  Future<void> _restoreLayout() async {
    final SharedPreferences prefs = await SharedPreferences.getInstance();
    if (!mounted) return;
    _prefs = prefs;

    final String? saved = prefs.getString(_layoutPrefsKey);
    if (saved == null) return;

    try {
      _layout.load(
        layout: saved,
        parser: _layoutParser,
        builder: _HomeAreaBuilder(_dockingItem),
      );
    } catch (_) {
      // Saved layout is corrupt or no longer compatible (e.g. after a panel
      // was renamed/removed) - keep the default layout instead.
    }
  }

  void _scheduleSave() {
    _saveDebounce?.cancel();
    // Layout changes fire on every frame of a drag, so debounce down to one
    // write shortly after things settle rather than hitting disk constantly.
    _saveDebounce = Timer(const Duration(milliseconds: 500), _saveLayout);
  }

  void _saveLayout() {
    final SharedPreferences? prefs = _prefs;
    if (prefs == null) return;
    prefs.setString(_layoutPrefsKey, _layout.stringify(parser: _layoutParser));
  }

  @override
  Widget build(BuildContext context) {
    Docking docking = Docking(layout: _layout);
    MultiSplitViewTheme theme = MultiSplitViewTheme(
      data: MultiSplitViewThemeData(
        dividerThickness: 16,
        dividerPainter: DividerPainters.grooved2(
          backgroundColor: Theme.of(context).colorScheme.surfaceDim,
          color: Theme.of(context).colorScheme.secondary,
          highlightedColor: Theme.of(context).colorScheme.primary,
        ),
      ),
      child: TabbedViewTheme(
        data: TabbedViewThemeData.dark()
          // ..tab.selectedStatus.innerTopBorder = BorderSide(
          //   color: Theme.of(context).colorScheme.tertiary,
          //   width: 3,
          // )
          ..tabsArea.color = Theme.of(context).colorScheme.surfaceDim
          ..tabsArea.equalHeights = .all
          ..tab.selectedStatus.decoration = BoxDecoration(
            color: Theme.of(context).colorScheme.surfaceContainerHighest,
            borderRadius: BorderRadius.only(
              topLeft: Radius.circular(5),
              topRight: Radius.circular(5),
            ),
            border: BoxBorder.fromLTRB(
              top: BorderSide(
                color: Theme.of(context).colorScheme.tertiary,
                width: 2,
              ),
            ),
          )
          ..tab.decoration = BoxDecoration(
            color: Theme.of(context).colorScheme.surfaceDim,
            borderRadius: BorderRadius.only(
              topLeft: Radius.circular(5),
              topRight: Radius.circular(5),
            ),
            border: BoxBorder.fromLTRB(
              top: BorderSide(
                color: Theme.of(context).colorScheme.surfaceDim,
                width: 1,
              ),
            ),
          )
          ..contentArea.decoration = BoxDecoration(
            border: BoxBorder.all(
              color: Theme.of(context).colorScheme.surfaceContainerHighest,
              width: 4,
            ),
          )
          ..contentArea.padding = EdgeInsets.all(2),
        child: docking,
      ),
    );
    return Scaffold(
      backgroundColor: Theme.of(context).colorScheme.surfaceDim,
      body: SafeArea(
        // multi_split_view resolves a divider drag by writing straight to
        // each Area's weight and calling plain setState() on its own
        // private State - it never calls the MultiSplitViewController's (or
        // by extension DockingLayout's) notifyListeners(), so there is no
        // change-notification hook anywhere in that path to save from.
        // Saving on every pointer-up instead sidesteps that entirely: it
        // fires after a divider drag ends (as well as everything else,
        // e.g. a tab move), well before the debounce in _scheduleSave()
        // reads the now-settled weights back out.
        child: Listener(
          onPointerUp: (_) => _scheduleSave(),
          child: Padding(padding: const EdgeInsets.all(16), child: theme),
        ),
      ),
    );
  }
}
