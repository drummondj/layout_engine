import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:layout_engine/providers/le_provider.dart';
import 'package:provider/provider.dart';

class LayerManager extends StatefulWidget {
  const LayerManager({super.key});

  @override
  State<LayerManager> createState() => _LayerManagerState();
}

class _LayerManagerState extends State<LayerManager> {
  late final LeProvider _provider;

  @override
  void initState() {
    super.initState();
    _provider = context.read<LeProvider>();
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        return Padding(
          padding: const EdgeInsets.all(8.0),
          child: SingleChildScrollView(
            child: Column(
              mainAxisSize: .min,
              children: [
                HierarchyRow(),
                HeaderRow(),
                AllRow(provider: provider),
                Divider(),
                AllPurposesRow(provider: provider),
                ..._provider.layerPurposes.map(
                  (purpose) =>
                      PurposeRow(provider: provider, purposeInfo: purpose),
                ),
                Divider(),
                AllLayersRow(provider: provider),
                ..._provider.layers.map(
                  (layer) => LayerRow(provider: provider, layerInfo: layer),
                ),
              ],
            ),
          ),
        );
      },
    );
  }
}

class LayerRow extends StatelessWidget {
  final LeLayerInfo layerInfo;
  final LeProvider provider;

  const LayerRow({super.key, required this.provider, required this.layerInfo});

  @override
  Widget build(BuildContext context) {
    return Row(
      spacing: 4,
      children: [
        Container(
          width: 24,
          height: 24,
          color: Color.fromARGB(
            255,
            layerInfo.layer.colorR,
            layerInfo.layer.colorG,
            layerInfo.layer.colorB,
          ),
        ),
        SizedBox(
          width: 150,
          child: Text(layerInfo.layer.name, overflow: .ellipsis),
        ),
        LayerVisibleCheckbox(provider: provider, layerInfo: layerInfo),
        LayerSelectableCheckbox(provider: provider, layerInfo: layerInfo),
      ],
    );
  }
}

class HeaderRow extends StatelessWidget {
  const HeaderRow({super.key});

  @override
  Widget build(BuildContext context) {
    return Row(
      spacing: 4,
      children: [
        SizedBox(width: 178),
        SizedBox(width: 24, child: Center(child: Text("V"))),
        SizedBox(width: 12),
        SizedBox(width: 24, child: Center(child: Text("S"))),
      ],
    );
  }
}

class AllRow extends StatelessWidget {
  final LeProvider provider;
  const AllRow({super.key, required this.provider});

  @override
  Widget build(BuildContext context) {
    return Row(
      spacing: 4,
      children: [
        SizedBox(width: 178, child: Text("All")),
        Checkbox(
          value: provider.allVisible,
          splashRadius: 0,
          onChanged: (value) => provider.setAllVisible(value),
        ),
        Checkbox(
          value: provider.allSelectable,
          splashRadius: 0,
          onChanged: (value) => provider.setAllSelectable(value),
        ),
      ],
    );
  }
}

class AllLayersRow extends StatelessWidget {
  final LeProvider provider;
  const AllLayersRow({super.key, required this.provider});

  @override
  Widget build(BuildContext context) {
    return Row(
      spacing: 4,
      children: [
        SizedBox(width: 178, child: Text("Layers")),
        Checkbox(
          value: provider.allLayersVisible,
          splashRadius: 0,
          onChanged: (value) => provider.setAllLayersVisible(value),
        ),
        Checkbox(
          value: provider.allLayersSelectable,
          splashRadius: 0,
          onChanged: (value) => provider.setAllLayersSelectable(value),
        ),
      ],
    );
  }
}

class AllPurposesRow extends StatelessWidget {
  final LeProvider provider;
  const AllPurposesRow({super.key, required this.provider});

  @override
  Widget build(BuildContext context) {
    return Row(
      spacing: 4,
      children: [
        SizedBox(width: 178, child: Text("Purposes")),
        Checkbox(
          value: provider.allPurposesVisible,
          splashRadius: 0,
          onChanged: (value) => provider.setAllPurposesVisible(value),
        ),
        Checkbox(
          value: provider.allPurposesSelectable,
          splashRadius: 0,
          onChanged: (value) => provider.setAllPurposesSelectable(value),
        ),
      ],
    );
  }
}

class PurposeRow extends StatelessWidget {
  final LePurposeInfo purposeInfo;
  final LeProvider provider;

  const PurposeRow({
    super.key,
    required this.provider,
    required this.purposeInfo,
  });

  @override
  Widget build(BuildContext context) {
    return Row(
      spacing: 4,
      children: [
        SizedBox(
          width: 178,
          child: Text(purposeInfo.purpose.name, overflow: .ellipsis),
        ),
        PurposeVisibleCheckbox(provider: provider, purposeInfo: purposeInfo),
        PurposeSelectableCheckbox(provider: provider, purposeInfo: purposeInfo),
      ],
    );
  }
}

class LayerVisibleCheckbox extends StatelessWidget {
  final LeLayerInfo layerInfo;
  final LeProvider provider;

  const LayerVisibleCheckbox({
    super.key,
    required this.provider,
    required this.layerInfo,
  });

  @override
  Widget build(BuildContext context) {
    return Checkbox(
      splashRadius: 0,
      value: layerInfo.isVisible,
      onChanged: (value) {
        provider.setLayerVisibility(layerInfo, value);
      },
    );
  }
}

class LayerSelectableCheckbox extends StatelessWidget {
  final LeLayerInfo layerInfo;
  final LeProvider provider;

  const LayerSelectableCheckbox({
    super.key,
    required this.provider,
    required this.layerInfo,
  });

  @override
  Widget build(BuildContext context) {
    return Checkbox(
      splashRadius: 0,
      value: layerInfo.isSelectable,
      onChanged: (value) {
        provider.setLayerSelectable(layerInfo, value);
      },
    );
  }
}

class PurposeVisibleCheckbox extends StatelessWidget {
  final LePurposeInfo purposeInfo;
  final LeProvider provider;

  const PurposeVisibleCheckbox({
    super.key,
    required this.provider,
    required this.purposeInfo,
  });

  @override
  Widget build(BuildContext context) {
    return Checkbox(
      splashRadius: 0,
      value: purposeInfo.isVisible,
      onChanged: (value) {
        provider.setPurposeVisible(purposeInfo, value);
      },
    );
  }
}

class PurposeSelectableCheckbox extends StatelessWidget {
  final LePurposeInfo purposeInfo;
  final LeProvider provider;

  const PurposeSelectableCheckbox({
    super.key,
    required this.provider,
    required this.purposeInfo,
  });

  @override
  Widget build(BuildContext context) {
    return Checkbox(
      splashRadius: 0,
      value: purposeInfo.isSelectable,
      onChanged: (value) {
        provider.setPurposeSelectable(purposeInfo, value);
      },
    );
  }
}

class HierarchyRow extends StatefulWidget {
  const HierarchyRow({super.key});

  @override
  State<HierarchyRow> createState() => _HierarchyRowState();
}

class _HierarchyRowState extends State<HierarchyRow> {
  final TextEditingController _controller = TextEditingController();
  late final LeProvider _provider;

  @override
  void initState() {
    super.initState();
    _provider = context.read();
    _controller.text = _provider.hierDepth.toString();
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _submit(String value) {
    final depth = int.tryParse(value);
    // le_set_hierarchy_depth itself rejects negative values (left
    // unchanged) rather than clamping - reject here too, before even
    // calling it, and reset the field back to the real current value
    // either way so it never shows a value that wasn't actually applied.
    if (depth != null && depth >= 0) {
      _provider.setHierDepth(depth);
    } else {
      _controller.text = _provider.hierDepth.toString();
    }
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        return Row(
          mainAxisSize: .min,
          mainAxisAlignment: .start,
          spacing: 10,
          children: [
            Text("Hierarchy Depth"),
            SizedBox(
              width: 100,
              child: TextField(controller: _controller, onSubmitted: _submit),
            ),
          ],
        );
      },
    );
  }
}
