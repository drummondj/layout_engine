// BUGS_AND_ENHANCEMENTS.md E20 - LeProvider methods that used to call
// LeEditorBase directly (setMode/undo/redo/armMove/selectAll/deselectAll/
// clearRulers/setHierDepth/layer-and-purpose visibility+selectability) now
// go through runTclCommand instead, so the exact same action taken from the
// GUI shows up as a real command in the console's own history (see
// runTclCommand's own doc comment) - these confirm each one submits the
// right Tcl command text via FakeTclConsole.evaluatedCommands, the same
// pattern le_provider_test.dart's own readLef tests already use. Real
// visibility/selectability/mode state itself is exercised end to end at the
// backend Tcl layer (backend/src/tcl/tests/smoke_test.tcl), not re-verified
// here - FakeTclConsole never actually executes anything.
import 'package:flutter_test/flutter_test.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:layout_engine_plugin/layout_engine_plugin.dart';

import 'fakes/fake_le_editor.dart';

void main() {
  test('setMode submits set_mode with the right keyword for each LeMode', () async {
    final editor = FakeLeEditor();
    final provider = LeProvider(editor: editor);

    await provider.setMode(LeMode.LE_MODE_EDIT);
    await provider.setMode(LeMode.LE_MODE_RULER);
    await provider.setMode(LeMode.LE_MODE_SELECT);

    expect(editor.lastTclConsole?.evaluatedCommands, [
      'set_mode edit',
      'set_mode ruler',
      'set_mode select',
    ]);
  });

  test('undo/redo submit the undo/redo Tcl commands', () async {
    final editor = FakeLeEditor();
    final provider = LeProvider(editor: editor);

    await provider.undo();
    await provider.redo();

    expect(editor.lastTclConsole?.evaluatedCommands, ['undo', 'redo']);
  });

  test('armMove/selectAll/deselectAll/clearRulers submit their own Tcl commands', () async {
    final editor = FakeLeEditor();
    final provider = LeProvider(editor: editor);

    await provider.armMove();
    await provider.selectAll();
    await provider.deselectAll();
    await provider.clearRulers();

    expect(editor.lastTclConsole?.evaluatedCommands, [
      'arm_move',
      'select_all',
      'deselect_all',
      'clear_rulers',
    ]);
  });

  test('setHierDepth submits set_hierarchy_depth with the given depth', () async {
    final editor = FakeLeEditor();
    final provider = LeProvider(editor: editor);

    await provider.setHierDepth(3);

    expect(editor.lastTclConsole?.evaluatedCommands, ['set_hierarchy_depth 3']);
  });

  test('setLayerVisibility/setLayerSelectable submit set_layer_visible/set_layer_selectable', () async {
    final editor = FakeLeEditor();
    final provider = LeProvider(editor: editor);
    final layerInfo = LeLayerInfo(
      index: 0,
      layer: const LeLayer(
        name: 'M1',
        colorR: 255,
        colorG: 0,
        colorB: 0,
        hasPhysicalLayer: true,
      ),
      isSelectable: true,
      isVisible: true,
    );

    await provider.setLayerVisibility(layerInfo, false);
    await provider.setLayerSelectable(layerInfo, true);

    expect(editor.lastTclConsole?.evaluatedCommands, [
      'set_layer_visible {M1} 0',
      'set_layer_selectable {M1} 1',
    ]);
  });

  test('setPurposeVisible/setPurposeSelectable submit set_purpose_visible/set_purpose_selectable', () async {
    final editor = FakeLeEditor();
    final provider = LeProvider(editor: editor);
    final purposeInfo = LePurposeInfo(
      purpose: LeLayerPurpose.trackPreferred,
      isSelectable: true,
      isVisible: true,
    );

    await provider.setPurposeVisible(purposeInfo, false);
    await provider.setPurposeSelectable(purposeInfo, true);

    expect(editor.lastTclConsole?.evaluatedCommands, [
      'set_purpose_visible trackPreferred 0',
      'set_purpose_selectable trackPreferred 1',
    ]);
  });

  test('setAllLayersVisible batches every layer into one semicolon-joined command', () async {
    final editor = FakeLeEditor()
      ..layers = [
        const LeLayer(name: 'M1', colorR: 1, colorG: 1, colorB: 1, hasPhysicalLayer: true),
        const LeLayer(name: 'M2', colorR: 2, colorG: 2, colorB: 2, hasPhysicalLayer: true),
      ];
    final provider = LeProvider(editor: editor);
    await provider.refreshLayers();

    await provider.setAllLayersVisible(false);

    expect(editor.lastTclConsole?.evaluatedCommands, [
      'set_layer_visible {M1} 0; set_layer_visible {M2} 0',
    ]);
  });

  test('setAllPurposesSelectable batches every purpose into one semicolon-joined command', () async {
    final editor = FakeLeEditor()
      ..purposes = [LeLayerPurpose.terminal, LeLayerPurpose.obstruction];
    final provider = LeProvider(editor: editor);
    await provider.refreshLayers();

    await provider.setAllPurposesSelectable(true);

    expect(editor.lastTclConsole?.evaluatedCommands, [
      'set_purpose_selectable terminal 1; set_purpose_selectable obstruction 1',
    ]);
  });

  test('setAllLayersVisible with no layers submits nothing', () async {
    final editor = FakeLeEditor();
    final provider = LeProvider(editor: editor);

    await provider.setAllLayersVisible(true);

    expect(editor.lastTclConsole, isNull);
  });
}
