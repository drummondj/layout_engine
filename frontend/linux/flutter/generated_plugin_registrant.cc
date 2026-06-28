//
//  Generated file. Do not edit.
//

// clang-format off

#include "generated_plugin_registrant.h"

#include <backend_plugin/backend_plugin.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) backend_plugin_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "BackendPlugin");
  backend_plugin_register_with_registrar(backend_plugin_registrar);
}
