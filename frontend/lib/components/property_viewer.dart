import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:layout_engine_plugin/layout_engine_plugin.dart';
import 'package:provider/provider.dart';

class PropertyViewer extends StatefulWidget {
  const PropertyViewer({super.key});

  @override
  State<PropertyViewer> createState() => _PropertyViewerState();
}

class _PropertyViewerState extends State<PropertyViewer> {
  int _currentIndex = 0;
  late final TextEditingController _filterController;
  String _filter = '';
  bool _showHiddenProperties = false;

  // Database-hierarchy navigation - purely local widget state, entirely
  // independent of LeProvider/canvas selection. `_hierarchy` is the
  // selected object's own ancestor chain, root (Library) first, leaf
  // last - the tree box renders exactly this list, indented by position.
  // `_currentRef` is whichever entry the user last clicked (defaults to
  // the leaf); it's what ObjectDetail's DataTable shows properties for.
  // Reset to the selected object's own chain only when the outer pager
  // index or the underlying selection itself changes (detected in
  // _syncHierarchy, called once per build) - never on a tree click.
  List<LeObjectRef> _hierarchy = const [];
  LeObjectRef? _currentRef;
  List<LeObjectRef> _lastSelectedObjects = const [];
  int _lastCurrentIndex = -1;

  @override
  void initState() {
    super.initState();
    _filterController = TextEditingController();
    _filterController.addListener(() {
      setState(() {
        _filter = _filterController.text;
      });
    });
  }

  @override
  void dispose() {
    _filterController.dispose();
    super.dispose();
  }

  void _syncHierarchy(LeProvider provider, List<LeObjectRef> selectedObjects) {
    if (_currentIndex >= selectedObjects.length) {
      _currentIndex = 0;
    }
    final selectionChanged = !listEquals(selectedObjects, _lastSelectedObjects);
    final indexChanged = _currentIndex != _lastCurrentIndex;
    // A defensive copy, not just `_lastSelectedObjects = selectedObjects` -
    // LeProvider.selectedObjects returns its own backing list by
    // reference (never rebuilt, just cleared+refilled in place on every
    // refreshSelection()), so storing that same reference here would make
    // `_lastSelectedObjects` silently track every future selection change
    // too, and listEquals above would then always see two aliases of the
    // identical, already-current list - selectionChanged would never be
    // true again after the first selection.
    _lastSelectedObjects = List<LeObjectRef>.of(selectedObjects);
    _lastCurrentIndex = _currentIndex;

    if (selectedObjects.isEmpty) {
      _hierarchy = const [];
      _currentRef = null;
      return;
    }
    if (selectionChanged || indexChanged || _hierarchy.isEmpty) {
      _jumpTo(provider, selectedObjects[_currentIndex]);
    }
  }

  /// Rebuilds `_hierarchy` as `ref`'s own ancestor chain (root first) and
  /// makes `ref` the currently-displayed node. Called both during build
  /// (from _syncHierarchy, mutating fields directly - the old
  /// breadcrumb's _syncBreadcrumb did the same) and from a "Children" row
  /// tap in ObjectDetail's DataTable (wrapped in setState by the caller,
  /// since that happens outside build).
  void _jumpTo(LeProvider provider, LeObjectRef ref) {
    final List<LeObjectRef> chain = [ref];
    LeObjectRef parent = provider.objectParent(ref);
    while (parent.isValid) {
      chain.add(parent);
      parent = provider.objectParent(parent);
    }
    _hierarchy = chain.reversed.toList(growable: false);
    _currentRef = ref;
  }

  void _selectNode(LeObjectRef ref) {
    setState(() {
      _currentRef = ref;
    });
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        final selectedObjects = provider.selectedObjects;
        _syncHierarchy(provider, selectedObjects);

        return Padding(
          padding: const EdgeInsets.all(8.0),
          child: SingleChildScrollView(
            child: Column(
              mainAxisSize: .min,
              children: selectedObjects.isEmpty
                  ? [Text("No selection")]
                  : [
                      Row(
                        children: [
                          Checkbox(
                            value: _showHiddenProperties,
                            visualDensity: VisualDensity.compact,
                            materialTapTargetSize:
                                MaterialTapTargetSize.shrinkWrap,
                            onChanged: (bool? value) {
                              setState(() {
                                _showHiddenProperties = value ?? false;
                              });
                            },
                          ),
                          const SizedBox(width: 4),
                          Text(
                            'Show hidden properties',
                            style: Theme.of(context).textTheme.bodySmall,
                          ),
                          Spacer(),
                          IconButton(
                            icon: Icon(Icons.chevron_left),
                            onPressed: _currentIndex <= 0
                                ? null
                                : () {
                                    setState(() {
                                      _currentIndex--;
                                    });
                                  },
                          ),
                          Text(
                            "${_currentIndex + 1} / ${selectedObjects.length}",
                          ),
                          IconButton(
                            icon: Icon(Icons.chevron_right),
                            onPressed:
                                _currentIndex >= selectedObjects.length - 1
                                ? null
                                : () {
                                    setState(() {
                                      _currentIndex++;
                                    });
                                  },
                          ),
                        ],
                      ),
                      _HierarchyTree(
                        provider: provider,
                        hierarchy: _hierarchy,
                        currentRef: _currentRef,
                        onTapNode: _selectNode,
                      ),
                      Padding(
                        padding: const EdgeInsets.symmetric(vertical: 4.0),
                        child: SearchBar(
                          controller: _filterController,
                          hintText: 'Filter properties',
                          constraints: const BoxConstraints(maxHeight: 32),
                          padding: const WidgetStatePropertyAll(
                            EdgeInsets.symmetric(horizontal: 4),
                          ),
                          textStyle: WidgetStatePropertyAll(
                            Theme.of(context).textTheme.bodySmall,
                          ),
                          hintStyle: WidgetStatePropertyAll(
                            Theme.of(context).textTheme.bodySmall,
                          ),
                          leading: const Padding(
                            padding: EdgeInsets.only(left: 8),
                            child: Icon(Icons.filter_list, size: 16),
                          ),
                          trailing: [
                            IconButton(
                              icon: const Icon(Icons.clear, size: 16),
                              visualDensity: VisualDensity.compact,
                              onPressed: _filterController.clear,
                            ),
                          ],
                        ),
                      ),
                      ObjectDetail(
                        ref: _currentRef!,
                        filter: _filter,
                        showHiddenProperties: _showHiddenProperties,
                        onNavigateToChild: (ref) => setState(() {
                          _jumpTo(provider, ref);
                        }),
                      ),
                    ],
            ),
          ),
        );
      },
    );
  }
}

/// [ref]'s own TCL object token, in exactly the format the TCL API's
/// friendly ids use (backend/src/tcl/generated/le_tcl_shim_generated.inc):
/// `"<kind>:<name>"` for the three name-keyed classes (Library/Design/
/// Terminal), `"<kind>:<packed>"` for the four numeric ones, where
/// `packed = (generation << 32) | index` - the same `pack<IdT>()` the
/// shim uses to turn an {index, generation} handle into the integer a
/// friendly id like `"shape:3"` embeds (see le_tcl_shim.cpp's
/// format_numeric_friendly_id/pack). [LeObjectRef] already carries the
/// same index/generation shape, so this never needs a round trip through
/// TCL itself.
String _tokenFor(LeProvider provider, LeObjectRef ref) {
  if (!ref.isValid) return "?";
  final int packed = (ref.generation << 32) | ref.index;
  return switch (ref.kind) {
    LeObjectKind.LE_OBJECT_KIND_LIBRARY => "library:${_nameOf(provider, ref)}",
    LeObjectKind.LE_OBJECT_KIND_DESIGN => "design:${_nameOf(provider, ref)}",
    LeObjectKind.LE_OBJECT_KIND_TERMINAL =>
      "terminal:${_nameOf(provider, ref)}",
    LeObjectKind.LE_OBJECT_KIND_ABSTRACT => "abstract:$packed",
    LeObjectKind.LE_OBJECT_KIND_TERMINAL_PORT => "terminal_port:$packed",
    LeObjectKind.LE_OBJECT_KIND_OBSTRUCTION => "obstruction:$packed",
    LeObjectKind.LE_OBJECT_KIND_SHAPE => "shape:$packed",
    // E1 (BUGS_AND_ENHANCEMENTS.md) - Row/Placement/PhysicalPort/Route/
    // Region are name-keyed friendly ids (le_tcl_shim.cpp's own
    // Row/Placement/PhysicalPort/Route/Region prefix/format/resolve
    // triples), matching Library/Design/Terminal's own convention above;
    // Blockage/PhysicalPortSegment have no natural name (unnamed DEF
    // constructs) so use the numeric-packed convention Abstract/Shape
    // already use instead (le_tcl_shim_generated.inc's
    // format_numeric_friendly_id).
    LeObjectKind.LE_OBJECT_KIND_ROW => "row:${_nameOf(provider, ref)}",
    LeObjectKind.LE_OBJECT_KIND_PLACEMENT =>
      "placement:${_nameOf(provider, ref)}",
    LeObjectKind.LE_OBJECT_KIND_ROUTE => "route:${_nameOf(provider, ref)}",
    LeObjectKind.LE_OBJECT_KIND_PHYSICAL_PORT =>
      "physical_port:${_nameOf(provider, ref)}",
    LeObjectKind.LE_OBJECT_KIND_REGION => "region:${_nameOf(provider, ref)}",
    LeObjectKind.LE_OBJECT_KIND_BLOCKAGE => "blockage:$packed",
    LeObjectKind.LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT =>
      "physical_port_segment:$packed",
    // Layout (E1 follow-up, breadcrumb hierarchy fix) has no name field
    // (schema.py's Layout Klass) so it's numeric-packed, matching
    // Abstract/Shape/Blockage/PhysicalPortSegment's own convention above
    // (le_tcl_shim_generated.inc's kLayoutPrefix/format_layout_id).
    LeObjectKind.LE_OBJECT_KIND_LAYOUT => "layout:$packed",
  };
}

String _nameOf(LeProvider provider, LeObjectRef ref) {
  for (final property in provider.objectProperties(ref)) {
    if (property.name == "name") {
      return property.value.toString();
    }
  }
  return "";
}

/// The box above the DataTable (replaces the old horizontal breadcrumb):
/// [hierarchy] rendered as an indented tree, root (Library) at the top,
/// the selected object at the bottom - tapping any row calls [onTapNode]
/// to change which object's properties ObjectDetail shows, without
/// altering the hierarchy itself. Always fully expanded - it's a single
/// ancestor chain, never a branching tree, so there's nothing to
/// collapse.
class _HierarchyTree extends StatelessWidget {
  const _HierarchyTree({
    required this.provider,
    required this.hierarchy,
    required this.currentRef,
    required this.onTapNode,
  });

  final LeProvider provider;
  final List<LeObjectRef> hierarchy;
  final LeObjectRef? currentRef;
  final ValueChanged<LeObjectRef> onTapNode;

  @override
  Widget build(BuildContext context) {
    final ColorScheme colorScheme = Theme.of(context).colorScheme;
    return Container(
      width: double.infinity,
      margin: const EdgeInsets.symmetric(vertical: 4.0),
      decoration: BoxDecoration(
        border: Border.all(color: colorScheme.surfaceBright),
        borderRadius: BorderRadius.circular(4),
      ),
      child: Column(
        mainAxisSize: .min,
        crossAxisAlignment: .start,
        children: [
          for (final (depth, ref) in hierarchy.indexed)
            _HierarchyTreeRow(
              label: _tokenFor(provider, ref),
              depth: depth,
              isCurrent: ref == currentRef,
              onTap: () => onTapNode(ref),
            ),
        ],
      ),
    );
  }
}

class _HierarchyTreeRow extends StatelessWidget {
  const _HierarchyTreeRow({
    required this.label,
    required this.depth,
    required this.isCurrent,
    required this.onTap,
  });

  final String label;
  final int depth;
  final bool isCurrent;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final ColorScheme colorScheme = Theme.of(context).colorScheme;
    return InkWell(
      onTap: onTap,
      child: Container(
        width: double.infinity,
        color: isCurrent ? colorScheme.primaryContainer : null,
        padding: EdgeInsets.only(
          left: 8.0 + depth * 16.0,
          right: 8,
          top: 4,
          bottom: 4,
        ),
        child: Row(
          children: [
            if (depth > 0) ...[
              Icon(
                Icons.subdirectory_arrow_right,
                size: 14,
                color: colorScheme.outline,
              ),
              const SizedBox(width: 4),
            ],
            Expanded(
              child: Text(
                label,
                overflow: TextOverflow.ellipsis,
                style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                  fontWeight: isCurrent ? FontWeight.bold : FontWeight.normal,
                  color: isCurrent ? colorScheme.onPrimaryContainer : null,
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

/// [ref]'s own property table - every property of whichever node is
/// currently selected in the hierarchy tree above, rendered in full in
/// the DataTable. Its children (if any - e.g. an Obstruction's Shapes)
/// get one extra row appended at the bottom, same table, with each child
/// rendered as its own tappable token; tapping one calls
/// [onNavigateToChild] (forwarded up to _PropertyViewerState._jumpTo) to
/// re-anchor the hierarchy tree on it - purely local navigation, never a
/// provider mutation.
class ObjectDetail extends StatelessWidget {
  // Some parents (an Obstruction/TerminalPort with many Shapes) can have
  // hundreds of children - listing them all as tappable tokens would blow
  // out the row's height and make the DataTable unusable, so only the
  // first _maxChildLinks are rendered, with a "+N more" count instead of
  // the rest.
  static const int _maxChildLinks = 10;

  final LeObjectRef ref;
  final String filter;
  final bool showHiddenProperties;
  final ValueChanged<LeObjectRef> onNavigateToChild;

  const ObjectDetail({
    super.key,
    required this.ref,
    required this.filter,
    required this.showHiddenProperties,
    required this.onNavigateToChild,
  });

  String _formatValue(LeSelectedProperty property) => switch (property.type) {
    LePropertyType.LE_PROPERTY_TYPE_DOUBLE =>
      (property.value as double).toStringAsFixed(3),
    LePropertyType.LE_PROPERTY_TYPE_STRING ||
    LePropertyType.LE_PROPERTY_TYPE_INT => property.value.toString(),
  };

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<LeProvider>();
    final ColorScheme colorScheme = Theme.of(context).colorScheme;
    final String normalizedFilter = filter.trim().toLowerCase();
    final List<LeSelectedProperty> properties = provider.objectProperties(ref);
    final Iterable<LeSelectedProperty> filteredProperties = properties
        .where(
          (property) =>
              showHiddenProperties || _formatValue(property).isNotEmpty,
        )
        .where(
          (property) =>
              normalizedFilter.isEmpty ||
              property.name.toLowerCase().contains(normalizedFilter),
        );
    // objectChildren can mix kinds in one list (an Abstract's own
    // Terminals and Obstructions together) - group by kind so each gets
    // its own correctly-labeled row, rather than assuming every entry
    // shares children.first's kind.
    final Map<LeObjectKind, List<LeObjectRef>> childrenByKind = {};
    for (final child in provider.objectChildren(ref)) {
      childrenByKind.putIfAbsent(child.kind, () => []).add(child);
    }

    return Column(
      mainAxisSize: .min,
      crossAxisAlignment: .center,
      children: [
        // Padding(
        //   padding: const EdgeInsets.symmetric(vertical: 4.0),
        //   child: Center(
        //     child: Text(
        //       _kindLabel(ref.kind),
        //       style: Theme.of(context).textTheme.titleMedium,
        //     ),
        //   ),
        // ),
        // DataTable's dividerThickness only ever sets the row divider's
        // *width* - Flutter treats BorderSide(width: 0.0) as a special
        // "hairline" border that still renders as one physical pixel, not
        // as no border at all. The divider only disappears by making its
        // color transparent, which DataTable reads from the ambient
        // DividerTheme rather than exposing directly itself.
        DividerTheme(
          data: const DividerThemeData(color: Colors.transparent),
          child: DataTable(
            dividerThickness: 0.0,
            headingRowHeight: 0.0,
            // The Children row's cell wraps a variable number of tappable
            // tokens (see below), which can need more than one line - the
            // default fixed row height would clip it.
            dataRowMinHeight: 0.0,
            dataRowMaxHeight: double.infinity,
            decoration: BoxDecoration(border: Border.all(width: 0)),
            columns: [
              DataColumn(label: Text("Property")),
              // Without an explicit columnWidth, DataTable sizes every
              // column to its content (IntrinsicColumnWidth), so the whole
              // table shrinks instead of filling the available width.
              // FlexColumnWidth lets this one column absorb the rest.
              DataColumn(
                label: Text("Value"),
                columnWidth: const FlexColumnWidth(),
              ),
            ],
            rows: [
              for (final (index, property) in filteredProperties.indexed)
                DataRow(
                  color: WidgetStateProperty.all(
                    index.isEven ? colorScheme.surface : colorScheme.surfaceDim,
                  ),
                  cells: [
                    DataCell(SelectableText(property.name)),
                    DataCell(SelectableText(_formatValue(property))),
                  ],
                ),
              for (final (groupIndex, group) in childrenByKind.entries.indexed)
                DataRow(
                  color: WidgetStateProperty.all(
                    (filteredProperties.length + groupIndex).isEven
                        ? colorScheme.surface
                        : colorScheme.surfaceDim,
                  ),
                  cells: [
                    DataCell(Text(_childLabel(group.key))),
                    DataCell(
                      Wrap(
                        spacing: 8,
                        runSpacing: 4,
                        children: [
                          for (final child in group.value.take(_maxChildLinks))
                            InkWell(
                              onTap: () => onNavigateToChild(child),
                              child: Text(
                                _tokenFor(provider, child),
                                style: Theme.of(context).textTheme.bodyMedium
                                    ?.copyWith(
                                      color: colorScheme.primary,
                                      decoration: TextDecoration.underline,
                                    ),
                              ),
                            ),
                          if (group.value.length > _maxChildLinks)
                            Text(
                              "truncated, +${group.value.length - _maxChildLinks} more",
                              style: Theme.of(context).textTheme.bodyMedium
                                  ?.copyWith(color: colorScheme.outline),
                            ),
                        ],
                      ),
                    ),
                  ],
                ),
            ],
          ),
        ),
      ],
    );
  }

  String _childLabel(LeObjectKind kind) => switch (kind) {
    LeObjectKind.LE_OBJECT_KIND_TERMINAL_PORT => "ports",
    LeObjectKind.LE_OBJECT_KIND_SHAPE => "shapes",
    LeObjectKind.LE_OBJECT_KIND_LIBRARY => "libraries",
    LeObjectKind.LE_OBJECT_KIND_DESIGN => "designs",
    LeObjectKind.LE_OBJECT_KIND_ABSTRACT => "abstracts",
    LeObjectKind.LE_OBJECT_KIND_TERMINAL => "terminals",
    LeObjectKind.LE_OBJECT_KIND_OBSTRUCTION => "obstructions",
    // E1 (BUGS_AND_ENHANCEMENTS.md) - only Blockage/Route/
    // PhysicalPortSegment report real children (their own Shapes,
    // objectChildren) - Row/Placement/Region are hierarchy leaves (see
    // LeEditor.objectChildren's own comment), so this label is never
    // actually rendered for those three, but the switch still needs to
    // be exhaustive.
    LeObjectKind.LE_OBJECT_KIND_BLOCKAGE => "shapes",
    LeObjectKind.LE_OBJECT_KIND_ROUTE => "shapes",
    LeObjectKind.LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT => "shapes",
    LeObjectKind.LE_OBJECT_KIND_PHYSICAL_PORT => "segments",
    LeObjectKind.LE_OBJECT_KIND_ROW => "rows",
    LeObjectKind.LE_OBJECT_KIND_PLACEMENT => "placements",
    LeObjectKind.LE_OBJECT_KIND_REGION => "regions",
    // Layout (E1 follow-up, breadcrumb hierarchy fix) also reports no
    // children through objectChildren yet - same "never actually
    // rendered, but the switch still needs to be exhaustive" note above.
    LeObjectKind.LE_OBJECT_KIND_LAYOUT => "layouts",
  };
}
