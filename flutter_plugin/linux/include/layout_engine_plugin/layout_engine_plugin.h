#ifndef FLUTTER_PLUGIN_LEF_EDITOR_PLUGIN_H_
#define FLUTTER_PLUGIN_LEF_EDITOR_PLUGIN_H_

#include <flutter_linux/flutter_linux.h>

G_BEGIN_DECLS

#ifdef FLUTTER_PLUGIN_IMPL
#define FLUTTER_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define FLUTTER_PLUGIN_EXPORT
#endif

typedef struct _LayoutEnginePlugin LayoutEnginePlugin;
typedef struct {
  GObjectClass parent_class;
} LayoutEnginePluginClass;

FLUTTER_PLUGIN_EXPORT GType layout_engine_plugin_get_type();

// Called by generated_plugin_registrant.cc (see pubspec.yaml's
// `pluginClass: LayoutEnginePlugin` - Flutter's tool derives both this
// filename and this function name from that value; keep them in sync if
// it's ever renamed). Registers the "layout_engine_plugin" method channel -
// see layout_engine_plugin.cc's own top comment for the protocol, which
// mirrors ../../macos/Classes/LayoutEnginePlugin.swift exactly.
FLUTTER_PLUGIN_EXPORT void layout_engine_plugin_register_with_registrar(
    FlPluginRegistrar* registrar);

G_END_DECLS

#endif  // FLUTTER_PLUGIN_LEF_EDITOR_PLUGIN_H_
