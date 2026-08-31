import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:provider/provider.dart';

const Map<String, Color> kMessageColors = {
  "ERROR": Colors.redAccent,
  "WARNING": Colors.amberAccent,
  "INFO": Colors.greenAccent,
};

const String prompt = "le_shell";

// A single misbehaving command (e.g. a bare get_shapes on a design with
// thousands of shapes) shouldn't be able to dump megabytes of text into
// the terminal's scrollback - truncate what's *displayed*, not the value
// itself, so a script relying on the full result is unaffected.
const int kMaxResultDisplayLength = 10000;

String _truncateForDisplay(String text) {
  if (text.length <= kMaxResultDisplayLength) return text;
  return '${text.substring(0, kMaxResultDisplayLength)}..truncated';
}

class Terminal extends StatefulWidget {
  const Terminal({super.key});

  @override
  State<Terminal> createState() => _TerminalState();
}

class _TerminalState extends State<Terminal> {
  late final LeProvider _provider;
  final ScrollController _scrollController = ScrollController();
  final TextEditingController _inputController = TextEditingController();
  final FocusNode _inputFocusNode = FocusNode();
  final List<String> _lines = [];
  bool _running = false;
  int _nextCommandNumber = 1;
  int _currentHistoryIndex = -1;
  late StreamSubscription<String> _messageSubscription;

  // Multiple Tab-completion candidates (see _completeCommand) - a
  // transient hint shown right under the input line, not permanent
  // scrollback, so it shouldn't survive past the moment it stops being
  // relevant: cleared by _onInputChanged the instant the input text
  // changes for any reason (typing further, a single-match completion
  // splicing in, history recall, or _submit's own clear()), and by the
  // Escape binding below for an explicit dismiss.
  List<String>? _completionSuggestions;

  @override
  void initState() {
    super.initState();
    _provider = context.read<LeProvider>();
    _messageSubscription = _provider.addMessageListener((message) {
      setState(() {
        _lines.add(message);
        _scrollToEnd();
      });
    });
    _inputController.addListener(_onInputChanged);
  }

  @override
  void dispose() {
    _messageSubscription.cancel();
    _inputController.removeListener(_onInputChanged);
    _scrollController.dispose();
    _inputController.dispose();
    _inputFocusNode.dispose();
    super.dispose();
  }

  void _onInputChanged() {
    if (_completionSuggestions == null) return;
    setState(() {
      _completionSuggestions = null;
    });
  }

  TextSpan _messageToTextSpan(String message) {
    for (var name in kMessageColors.keys) {
      var match = RegExp(
        "^$name(.*)",
        caseSensitive: false,
      ).firstMatch(message);
      if (match != null) {
        return TextSpan(
          children: [
            TextSpan(
              text: name,
              style: TextStyle(color: kMessageColors[name]),
            ),
            TextSpan(text: "${match.group(1)}\n"),
          ],
        );
      }
    }
    if (!message.endsWith("\n")) {
      message += "\n";
    }
    return TextSpan(text: message);
  }

  Future<void> _submit() async {
    final command = _inputController.text.trim();
    if (command.isEmpty || _running) return;
    if (command == "history") {
      // Sourced from the backend's own command-recall log (UPDATES.md
      // item 21), not a local list - only successfully executed commands
      // appear, an intentional, requirement-driven behavior change (the
      // old local _commandHistory recorded every submission regardless
      // of outcome).
      final history = _provider.commandHistoryCache;
      var padding = history.length.toString().length;
      setState(() {
        _lines.add("");
        _lines.addAll(
          history.indexed.map(
            (e) => "${(e.$1 + 1).toString().padLeft(padding)}  ${e.$2}",
          ),
        );
        _lines.add("");
      });
    } else {
      setState(() {
        _lines.add('$_nextCommandNumber> $command');
        _nextCommandNumber++;
        _running = true;
      });

      // Holds back whatever's been streamed in via onOutput that doesn't
      // yet end in a newline (e.g. a `puts -nonewline` call, or a chunk
      // arriving mid-line) - flushed a completed line at a time into
      // _lines as soon as one is available, rather than waiting for the
      // whole command to finish (BUGS_AND_ENHANCEMENTS.md item E3), and
      // flushed outright once the command completes either way.
      var pendingLine = '';
      void onOutput(String chunk) {
        pendingLine += chunk;
        final parts = pendingLine.split('\n');
        pendingLine = parts.removeLast();
        if (parts.isEmpty) return;
        setState(() {
          _lines.addAll(parts);
          _scrollToEnd();
        });
      }

      String result;
      try {
        result = await _provider.runTclCommand(command, onOutput: onOutput);
      } catch (error) {
        result = 'error: $error';
      }

      setState(() {
        if (pendingLine.isNotEmpty) _lines.add(pendingLine);
        if (result.isNotEmpty) _lines.add(_truncateForDisplay(result));
        _running = false;
      });
    }
    _scrollToEnd();
    _currentHistoryIndex = -1;
    _inputController.clear();
    _inputFocusNode.requestFocus();
  }

  void _scrollToEnd() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!_scrollController.hasClients) return;
      _scrollController.jumpTo(_scrollController.position.maxScrollExtent);
    });
  }

  // Tab-completion (UPDATES.md item 20 point 3's GUI side) - asks the
  // backend's own complete_command (le_tcl_procs.tcl) for candidates
  // completing whichever whitespace-delimited token is currently being
  // typed (a command name, a -flag, or a .-prefixed property path - see
  // that proc's own doc comment), then either splices the sole candidate
  // in directly or, for more than one, shows them as a transient hint
  // under the input line (_completionSuggestions - see build()) without
  // touching the input - the same "single match completes, several just
  // get listed" convention an interactive shell uses, except the listing
  // disappears on its own once it's no longer relevant (_onInputChanged)
  // rather than becoming permanent scrollback. Silent (does nothing) on
  // zero candidates, an empty input, or a command still running -
  // matches _submit()'s own guard.
  Future<void> _completeCommand() async {
    if (_running) return;
    final text = _inputController.text;
    if (text.trim().isEmpty) return;

    // complete_command takes the whole line as one Tcl string argument -
    // built here as a "..."-quoted literal (not {...}), since the
    // partial line being typed can itself contain an unbalanced brace
    // (e.g. "-rects {{0 0 1" mid-flag) that would break {...} grouping;
    // "..." only needs its own four special characters escaped, the
    // same set backend/codegen's tcl_dquote_escape() escapes server-side
    // for the same reason.
    final escaped = text
        .replaceAll('\\', '\\\\')
        .replaceAll('"', '\\"')
        .replaceAll('\$', '\\\$')
        .replaceAll('[', '\\[')
        .replaceAll(']', '\\]');

    String result;
    try {
      result = await _provider.runTclCommand('complete_command "$escaped"');
    } catch (_) {
      return;
    }
    final candidates = result.trim().isEmpty
        ? const <String>[]
        : result.trim().split(RegExp(r'\s+'));
    if (candidates.isEmpty) return;

    if (candidates.length == 1) {
      final completed = _replaceLastToken(text, candidates.first);
      setState(() {
        _inputController.text = completed;
        _inputController.selection = TextSelection.collapsed(
          offset: completed.length,
        );
      });
    } else {
      setState(() {
        _completionSuggestions = candidates;
      });
    }
    // The input row (and the hint that now renders right below it for
    // the multi-candidate case) can already be scrolled out of view by
    // that point if the scrollback above it is tall enough - e.g. right
    // after any long-output command - with nothing else to bring it back
    // on screen, this looked exactly like "Tab does nothing" even though
    // completion ran and updated state correctly (a real bug: this call
    // was dropped when the multi-candidate case stopped appending to
    // _lines - see _completionSuggestions - since _lines.addAll used to
    // be paired with it here).
    _scrollToEnd();
    // Tab can fire while focus is on the scrollback (see build()'s own
    // comment) - bring it back to the input either way, so the user can
    // keep typing immediately without an extra click.
    _inputFocusNode.requestFocus();
  }

  // Lays `items` out in justified columns, like a shell's own
  // multi-match completion listing - each column padded to the widest
  // item plus a 2-space gutter, wrapped once a row would exceed a
  // conventional 80-column terminal width. There's no real per-pixel
  // measurement of this console's own pane here (it's one resizable
  // pane of a docking layout, not the whole window), so 80 is an
  // assumption, not a measurement - the same fallback width a shell's
  // own completion menu uses when it can't detect the real terminal
  // size either. Relies on the app's own monospace theme font
  // (JetBrains Mono, see main.dart) for the padding to actually line up
  // visually.
  List<String> _formatColumns(List<String> items) {
    final maxLen = items.map((item) => item.length).reduce(
      (a, b) => a > b ? a : b,
    );
    final colWidth = maxLen + 2;
    const terminalWidth = 80;
    final columns = (terminalWidth / colWidth).floor().clamp(1, items.length);

    final rows = <String>[];
    for (var i = 0; i < items.length; i += columns) {
      final rowItems = items.skip(i).take(columns).toList();
      final row = StringBuffer();
      for (var j = 0; j < rowItems.length; j++) {
        final isLastInRow = j == rowItems.length - 1;
        row.write(isLastInRow ? rowItems[j] : rowItems[j].padRight(colWidth));
      }
      rows.add(row.toString());
    }
    return rows;
  }

  // Splices `candidate` in place of whichever trailing, whitespace-
  // delimited token of `text` is currently being typed - mirrors
  // complete_command's own "every candidate is a full replacement for
  // the last token" contract (le_tcl_procs.tcl) exactly, so this never
  // needs to know whether that token was a command name, a flag, or a
  // .-prefixed property path. Appends outright when `text` is empty or
  // already ends in whitespace (nothing partial to replace).
  String _replaceLastToken(String text, String candidate) {
    final match = RegExp(r'\S+$').firstMatch(text);
    if (match == null) {
      return '$text$candidate';
    }
    return text.substring(0, match.start) + candidate;
  }

  void _history(int by) {
    final history = _provider.commandHistoryCache;
    if (history.isEmpty) {
      return;
    }
    int index = -1;
    if (_currentHistoryIndex == -1) {
      index = history.length + by;
    } else {
      index = _currentHistoryIndex + by;
    }

    if (index < 0 || index > history.length - 1) {
      return;
    }

    _inputController.clear();
    _inputController.text = history[index];
    _currentHistoryIndex = index;
    // Same reasoning as _completeCommand's own trailing requestFocus -
    // arrow-key history recall can also fire while focus is on the
    // scrollback.
    _inputFocusNode.requestFocus();
  }

  @override
  Widget build(BuildContext context) {
    // CallbackShortcuts wraps the *whole* pane, scrollback included, not
    // just the input field - it used to wrap only the TextField, which
    // meant Tab/Enter/history stopped working the moment focus moved
    // anywhere else in this widget, most commonly after clicking into
    // the scrollback to select/copy some of it (report_properties's own
    // output is exactly the kind of thing worth copying - a real bug
    // report traced to this: "after I run report_properties, command
    // completion stops working" was really "after I click the output to
    // select it"). Shortcuts/Actions resolve from wherever focus
    // currently sits up through its ancestors, so binding here instead
    // catches every one of these keys regardless of which descendant -
    // the input or the scrollback's own SelectableText - actually holds
    // focus; each handler still explicitly returns focus to the input
    // afterward so typing resumes immediately either way.
    return CallbackShortcuts(
      bindings: <ShortcutActivator, VoidCallback>{
        const SingleActivator(LogicalKeyboardKey.arrowUp): () => _history(-1),
        const SingleActivator(LogicalKeyboardKey.arrowDown): () =>
            _history(1),
        const SingleActivator(LogicalKeyboardKey.enter): () => _submit(),
        const SingleActivator(LogicalKeyboardKey.tab): () =>
            _completeCommand(),
        const SingleActivator(LogicalKeyboardKey.escape): () {
          setState(() => _completionSuggestions = null);
          _inputFocusNode.requestFocus();
        },
      },
      child: Padding(
        padding: const EdgeInsets.all(8.0),
        child: SingleChildScrollView(
          controller: _scrollController,
          child: Column(
            crossAxisAlignment: .stretch,
            children: [
              if (_lines.isNotEmpty)
                SelectableText.rich(
                  scrollPhysics: NeverScrollableScrollPhysics(),
                  TextSpan(
                    style: DefaultTextStyle.of(context).style,
                    children: _lines
                        .map((message) => _messageToTextSpan(message))
                        .toList(),
                  ),
                ),
              Row(
                mainAxisSize: .min,
                crossAxisAlignment: .start,
                children: [
                  Text("$prompt % "),
                  Expanded(
                    child: TextField(
                      controller: _inputController,
                      focusNode: _inputFocusNode,
                      enabled: !_running,
                      cursorColor: Colors.white70,
                      cursorWidth: 10,
                      decoration: const InputDecoration(
                        isDense: true,
                        border: UnderlineInputBorder(
                          borderSide: BorderSide.none,
                        ),
                      ),
                      style: DefaultTextStyle.of(context).style,
                      maxLines: null,
                      onSubmitted: (_) => _submit(),
                      textAlignVertical: .top,
                    ),
                  ),
                ],
              ),
              if (_completionSuggestions != null)
                Padding(
                  padding: const EdgeInsets.only(left: 16, top: 2),
                  child: Text(
                    _formatColumns(_completionSuggestions!).join('\n'),
                    style: DefaultTextStyle.of(
                      context,
                    ).style.copyWith(color: Colors.white54),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}
