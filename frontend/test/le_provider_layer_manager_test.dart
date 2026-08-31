// Regression coverage for BUGS_AND_ENHANCEMENTS.md E12: a pseudo-row with
// no physical Technology Layer of its own (ROW/BOUNDARY/GCELLGRID/
// PLACEMENT_BLOCKAGE/REGION) already has its own single-purpose entry in
// LeProvider.layerPurposes - showing it *again* in LeProvider.layers (the
// Layer Manager's own Layers section) is a redundant, confusing duplicate
// two different checkboxes toggle the exact same underlying flag - so
// refreshLayers() excludes it from `layers` entirely, leaving it visible
// only via its own purpose entry.
import 'package:flutter_test/flutter_test.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:layout_engine_plugin/layout_engine_plugin.dart';

import 'fakes/fake_le_editor.dart';

void main() {
  test(
    'refreshLayers excludes purpose-only pseudo-rows from layers but keeps their purpose entry',
    () async {
      final editor = FakeLeEditor()
        ..layers = [
          const LeLayer(
            name: 'M1',
            colorR: 255,
            colorG: 0,
            colorB: 0,
            hasPhysicalLayer: true,
          ),
          const LeLayer(
            name: 'BOUNDARY',
            colorR: 160,
            colorG: 160,
            colorB: 160,
            hasPhysicalLayer: false,
          ),
        ]
        ..purposes = [LeLayerPurpose.terminal, LeLayerPurpose.boundary];
      final provider = LeProvider(editor: editor);

      await provider.refreshLayers();

      expect(provider.layers.map((l) => l.layer.name), ['M1']);
      expect(
        provider.layerPurposes.map((p) => p.purpose),
        [LeLayerPurpose.terminal, LeLayerPurpose.boundary],
      );
    },
  );

  test(
    'allLayersVisible/Selectable reflect only physical layers, not pseudo-rows',
    () async {
      final editor = FakeLeEditor()
        ..layers = [
          const LeLayer(
            name: 'M1',
            colorR: 255,
            colorG: 0,
            colorB: 0,
            hasPhysicalLayer: true,
          ),
          const LeLayer(
            name: 'BOUNDARY',
            colorR: 160,
            colorG: 160,
            colorB: 160,
            hasPhysicalLayer: false,
          ),
        ]
        ..purposes = [LeLayerPurpose.boundary]
        // BOUNDARY (excluded from `layers`) is hidden, but M1 (the only
        // real layer) is still fully visible/selectable - the aggregate
        // should reflect that, not be dragged false by the excluded row.
        ..layerVisibility['BOUNDARY'] = false
        ..layerSelectability['BOUNDARY'] = false;
      final provider = LeProvider(editor: editor);

      await provider.refreshLayers();

      expect(provider.allLayersVisible, isTrue);
      expect(provider.allLayersSelectable, isTrue);
    },
  );
}
