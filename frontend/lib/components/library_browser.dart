import 'package:flutter/material.dart';
import 'package:flutter_fancy_tree_view/flutter_fancy_tree_view.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:layout_engine_plugin/layout_engine_plugin.dart';
import 'package:provider/provider.dart';

class LibraryBrowser extends StatefulWidget {
  const LibraryBrowser({super.key});

  @override
  State<LibraryBrowser> createState() => _LibraryBrowserState();
}

// A design node's own two possible child views (BUGS_AND_ENHANCEMENTS.md
// E15) - not named `abstract`/`layout` directly since the former is a
// reserved word in Dart.
enum DesignView { abstractView, layoutView }

class Node {
  Node({
    required this.title,
    this.libraryId,
    this.designId,
    this.designView,
    Iterable<Node>? children,
  }) : assert(
         designView == null || designId != null,
         'designView is only meaningful alongside a designId',
       ),
       children = <Node>[...?children];

  final String title;
  final LeLibraryRef? libraryId;
  final LeDesignRef? designId;

  // Null for the design node itself (which groups both of its own
  // possible view children); set to distinguish a design's own Abstract
  // child node from its Layout child node, since both otherwise share
  // the same designId - see operator== below.
  final DesignView? designView;
  final List<Node> children;

  // buildTree() rebuilds fresh Node instances every time the provider
  // notifies, so expansion state (tracked by TreeController as a Set<Node>)
  // would otherwise reset on every rebuild. Value equality keyed on the
  // stable library/design ref (plus designView, so a design's own two view
  // children and their own parent design node - all three sharing the same
  // designId - don't collide with each other) lets a rebuilt node be
  // recognized as "the same" node the user previously expanded.
  @override
  bool operator ==(Object other) {
    if (identical(this, other)) return true;
    if (other is! Node) return false;
    if (designId != null || other.designId != null) {
      return designId == other.designId && designView == other.designView;
    }
    if (libraryId != null || other.libraryId != null) {
      return libraryId == other.libraryId;
    }
    return false;
  }

  @override
  int get hashCode {
    if (designId != null) return Object.hash(designId, designView);
    return libraryId?.hashCode ?? identityHashCode(this);
  }
}

class _LibraryBrowserState extends State<LibraryBrowser> {
  late LeProvider _provider;
  late final TextEditingController _searchBarTextEditingController;
  late final TreeController<Node> _treeController;
  late final Node _root = Node(title: '/');
  TreeSearchResult<Node>? _filter;
  Pattern? _searchPattern;
  int _buildTreeRequestId = 0;

  Iterable<Node> getChildren(Node node) {
    if (_filter case TreeSearchResult<Node> filter?) {
      return node.children.where(filter.hasMatch);
    }
    return node.children;
  }

  void search(String query) {
    // Needs to be reset before searching again, otherwise the tree controller
    // wouldn't reach some nodes because of the `getChildren()` impl above.
    _filter = null;

    Pattern pattern;
    try {
      pattern = RegExp(query);
    } on FormatException {
      pattern = query;
    }
    _searchPattern = pattern;

    _filter = _treeController.search(
      (Node node) => node.title.contains(pattern),
    );
    _treeController.rebuild();

    if (mounted) {
      setState(() {});
    }
  }

  void clearSearch() {
    if (_filter == null) return;

    setState(() {
      _filter = null;
      _searchPattern = null;
      _treeController.rebuild();
      _searchBarTextEditingController.clear();
    });
  }

  void onSearchQueryChanged() {
    final String query = _searchBarTextEditingController.text.trim();

    if (query.isEmpty) {
      clearSearch();
      return;
    }

    search(query);
  }

  @override
  void initState() {
    super.initState();
    _treeController = TreeController<Node>(
      roots: _root.children,
      childrenProvider: getChildren,
    );

    _provider = context.read<LeProvider>();
    _searchBarTextEditingController = TextEditingController();
    _searchBarTextEditingController.addListener(onSearchQueryChanged);

    _provider.addListener(buildTree);
    buildTree();
  }

  @override
  void dispose() {
    _provider.removeListener(buildTree);
    _filter = null;
    _searchPattern = null;
    _treeController.dispose();
    _searchBarTextEditingController.dispose();
    super.dispose();
  }

  void buildTree() async {
    // buildTree() can be re-entered before a previous call finishes (e.g. the
    // initial call in initState() racing the provider-listener call fired by
    // the same LEF load), so build into a local list and only touch _root
    // once this call is confirmed to be the most recent one. Otherwise two
    // overlapping calls both append into the shared _root.children list and
    // produce duplicate entries.
    final int requestId = ++_buildTreeRequestId;
    final List<Node> newChildren = [];
    for (final (index, library) in (await _provider.getLibraries()).indexed) {
      final libraryNode = Node(title: library.name, libraryId: library.id);
      for (final design in await _provider.getDesigns(index)) {
        // BUGS_AND_ENHANCEMENTS.md E15: a design node's own children are
        // the views that actually exist for it (a plain LEF macro has
        // only an Abstract; a DEF-defined design that also reused an
        // existing macro's own Design can have both) - each opened by
        // tapping it directly, distinct from the design node's own tap
        // (still opens the Abstract view, unchanged).
        final designNode = Node(title: design.name, designId: design.id);
        if (design.abstractId != null) {
          designNode.children.add(
            Node(
              title: 'Abstract',
              designId: design.id,
              designView: DesignView.abstractView,
            ),
          );
        }
        if (design.layoutId != null) {
          designNode.children.add(
            Node(
              title: 'Layout',
              designId: design.id,
              designView: DesignView.layoutView,
            ),
          );
        }
        libraryNode.children.add(designNode);
      }
      newChildren.add(libraryNode);
    }

    if (requestId != _buildTreeRequestId || !mounted) return;

    _root.children
      ..clear()
      ..addAll(newChildren);
    _treeController.rebuild();
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        return Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Padding(
              padding: const EdgeInsets.all(8),
              child: SearchBar(
                controller: _searchBarTextEditingController,
                hintText: 'filter',
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
                  Badge(
                    isLabelVisible: _filter != null,
                    label: Text(
                      '${_filter?.totalMatchCount}/${_filter?.totalNodeCount}',
                    ),
                  ),
                  IconButton(
                    icon: const Icon(Icons.clear, size: 16),
                    visualDensity: VisualDensity.compact,
                    onPressed: clearSearch,
                  ),
                ],
              ),
            ),
            Expanded(
              child: AnimatedTreeView<Node>(
                treeController: _treeController,
                nodeBuilder: (BuildContext context, TreeEntry<Node> entry) {
                  return TreeTile(
                    entry: entry,
                    match: _filter?.matchOf(entry.node),
                    searchPattern: _searchPattern,
                    onDesignTap: (LeDesignRef designId, DesignView? view) {
                      switch (view) {
                        case DesignView.layoutView:
                          provider.openDesignLayout(designId);
                        case DesignView.abstractView:
                        case null:
                          provider.openDesign(designId);
                      }
                    },
                  );
                },
                duration: Duration(milliseconds: 300),
              ),
            ),
          ],
        );
      },
    );
  }
}

class TreeTile extends StatefulWidget {
  const TreeTile({
    super.key,
    required this.entry,
    required this.match,
    required this.searchPattern,
    required this.onDesignTap,
  });

  final TreeEntry<Node> entry;
  final TreeSearchMatch? match;
  final Pattern? searchPattern;
  final void Function(LeDesignRef designId, DesignView? view) onDesignTap;

  @override
  State<TreeTile> createState() => _TreeTileState();
}

class _TreeTileState extends State<TreeTile> {
  late InlineSpan titleSpan;

  TextStyle? dimStyle;
  TextStyle? highlightStyle;

  bool get shouldShowBadge =>
      !widget.entry.isExpanded && (widget.match?.subtreeMatchCount ?? 0) > 0;

  @override
  Widget build(BuildContext context) {
    final LeDesignRef? designId = widget.entry.node.designId;

    return TreeIndentation(
      entry: widget.entry,
      child: Row(
        children: [
          if (widget.entry.hasChildren)
            ExpandIcon(
              key: GlobalObjectKey(widget.entry.node),
              isExpanded: widget.entry.isExpanded,
              onPressed: (_) =>
                  TreeViewScope.of<Node>(context)
                    ..controller.toggleExpansion(widget.entry.node),
            )
          else
            const SizedBox(width: 48),
          if (shouldShowBadge)
            Padding(
              padding: const EdgeInsetsDirectional.only(end: 8),
              child: Badge(label: Text('${widget.match?.subtreeMatchCount}')),
            ),
          Expanded(
            child: InkWell(
              onTap: designId == null
                  ? null
                  : () => widget.onDesignTap(
                      designId,
                      widget.entry.node.designView,
                    ),
              child: Padding(
                padding: const EdgeInsets.symmetric(vertical: 4),
                child: Text.rich(titleSpan),
              ),
            ),
          ),
        ],
      ),
    );
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    setupTextStyles();
    titleSpan = buildTextSpan();
  }

  @override
  void didUpdateWidget(covariant TreeTile oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.searchPattern != widget.searchPattern ||
        oldWidget.entry.node.title != widget.entry.node.title) {
      titleSpan = buildTextSpan();
    }
  }

  void setupTextStyles() {
    final TextStyle style = DefaultTextStyle.of(context).style;
    final Color highlightColor = Theme.of(context).colorScheme.primary;
    highlightStyle = style.copyWith(
      color: highlightColor,
      decorationColor: highlightColor,
      decoration: TextDecoration.underline,
    );
    dimStyle = style.copyWith(color: style.color?.withAlpha(128));
  }

  InlineSpan buildTextSpan() {
    final String title = widget.entry.node.title;

    if (widget.searchPattern == null) {
      return TextSpan(text: title);
    }

    final List<InlineSpan> spans = <InlineSpan>[];
    bool hasAnyMatches = false;

    title.splitMapJoin(
      widget.searchPattern!,
      onMatch: (Match match) {
        hasAnyMatches = true;
        spans.add(TextSpan(text: match.group(0)!, style: highlightStyle));
        return '';
      },
      onNonMatch: (String text) {
        spans.add(TextSpan(text: text));
        return '';
      },
    );

    if (hasAnyMatches) {
      return TextSpan(children: spans);
    }

    return TextSpan(text: title, style: dimStyle);
  }
}
