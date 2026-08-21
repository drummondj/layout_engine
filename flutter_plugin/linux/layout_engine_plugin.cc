#include "include/layout_engine_plugin/layout_engine_plugin.h"

#include <flutter_linux/flutter_linux.h>

#include <cstring>
#include <string>

#include "lef_texture.h"

// Only present when CMakeLists.txt's LE_LINK_BACKEND option is ON - see
// lef_texture.cc's own comment on this same guard. le::TclBridge
// (../src/le_tcl_bridge.hpp) is plain C++, shared verbatim with macOS's
// own LeTclBridge.mm (a thin Objective-C++ wrapper around the same
// class) - see that file's comment and TCL_EXPLORATION.md's show_gui
// section for the full design rationale.
#ifdef LE_LINK_BACKEND_ENABLED
#include <limits.h>
#include <unistd.h>

#include "le_tcl_bridge.hpp"

namespace {

// le_tcl.so/le_tcl_procs.tcl are bundled into the app's own lib/
// directory alongside this plugin's .so (CMakeLists.txt's
// layout_engine_plugin_bundled_libraries - see its own comment), not
// referenced by a compile-time absolute path baked into this binary -
// that used to be this build machine's own backend/build-linux tree
// (LE_TCL_MODULE_PATH/LE_TCL_PROCS_PATH), which doesn't exist at all on
// a machine a built release bundle gets copied to. /proc/self/exe is
// Linux-specific (this whole file already is - see lef_texture.cc's own
// platform split), always resolves to the real executable path
// regardless of cwd or how the bundle was invoked (a launcher script, a
// relative ./layout_engine, a desktop file with an absolute Exec= path).
std::string ExecutableDir() {
  char buf[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len <= 0) {
    return ".";
  }
  buf[len] = '\0';
  std::string exe_path(buf);
  size_t slash = exe_path.find_last_of('/');
  // slash == 0 (executable directly under /) needs its own case - "/"
  // itself, not an empty substring(0, 0) - see render.cpp's
  // default_typeface() for the identical bug this mirrors, confirmed via
  // a real throwaway test harness placed at filesystem root.
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return exe_path.substr(0, slash);
}

}  // namespace
#endif

// Registers the "layout_engine_plugin" method channel used to bridge a
// Dart-owned LeHandle* (see ../lib/layout_engine_plugin.dart's LeEditor) into
// a native FlLeTexture (see lef_texture.h/.cc) and (see createTclConsole/
// evalTclCommand/disposeTclConsole below) an embedded Tcl console. Dart FFI
// can't reach the texture registrar itself - only platform embedder code
// can - so this channel exists purely to hand ids back and forth; the
// actual per-frame pixel pull and per-command Tcl eval never cross it.
// Mirrors ../macos/Classes/LayoutEnginePlugin.swift's protocol exactly:
//   createTexture({handleAddress: int}) -> int textureId
//   markTextureFrameAvailable({textureId: int}) -> null
//   disposeTexture({textureId: int}) -> null
//   createTclConsole({handleAddress: int}) -> int consoleId
//   evalTclCommand({consoleId: int, command: string}) -> string
//   disposeTclConsole({consoleId: int}) -> null

#define LEF_EDITOR_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), layout_engine_plugin_get_type(), LayoutEnginePlugin))

struct _LayoutEnginePlugin {
  GObject parent_instance;

  FlTextureRegistrar* texture_registrar;
  // Keyed by texture id (as returned by fl_texture_get_id) - keeps each
  // FlLeTexture alive between createTexture and disposeTexture, mirroring
  // LayoutEnginePlugin.swift's own `textures` dictionary.
  GHashTable* textures;

  // Tcl consoles (see le_tcl_bridge.hpp - the show_gui in-app console,
  // TCL_EXPLORATION.md) keyed by an id this plugin hands back to Dart,
  // same pattern as `textures` above. next_console_id mirrors how
  // fl_texture_registrar_register_texture hands back its own id - there's
  // no equivalent registry for Tcl consoles (they aren't Flutter engine
  // objects), so this plugin owns the id space itself. Value type is
  // le::TclBridge* when LE_LINK_BACKEND_ENABLED, unused (always empty)
  // otherwise - no #ifdef needed on the field itself since GHashTable's
  // value type is opaque gpointer either way.
  GHashTable* tcl_consoles;
  int64_t next_console_id;
};

G_DEFINE_TYPE(LayoutEnginePlugin, layout_engine_plugin, g_object_get_type())

static FlMethodResponse* handle_create_texture(LayoutEnginePlugin* self, FlValue* args) {
  FlValue* handle_address_value = fl_value_lookup_string(args, "handleAddress");
  if (handle_address_value == nullptr ||
      fl_value_get_type(handle_address_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "bad_args", "createTexture requires handleAddress", nullptr));
  }

  FlLeTexture* texture = fl_le_texture_new(fl_value_get_int(handle_address_value));
  if (!fl_texture_registrar_register_texture(self->texture_registrar,
                                             FL_TEXTURE(texture))) {
    g_object_unref(texture);
    return FL_METHOD_RESPONSE(
        fl_method_error_response_new("register_failed", "register_texture failed", nullptr));
  }

  int64_t texture_id = fl_texture_get_id(FL_TEXTURE(texture));
  // Table owns the reference from here on; released in handle_dispose_texture.
  g_hash_table_insert(self->textures, GINT_TO_POINTER(texture_id), texture);

  g_autoptr(FlValue) result = fl_value_new_int(texture_id);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse* handle_mark_texture_frame_available(LayoutEnginePlugin* self,
                                                              FlValue* args) {
  FlValue* texture_id_value = fl_value_lookup_string(args, "textureId");
  if (texture_id_value == nullptr || fl_value_get_type(texture_id_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "bad_args", "markTextureFrameAvailable requires textureId", nullptr));
  }

  int64_t texture_id = fl_value_get_int(texture_id_value);
  FlLeTexture* texture =
      FL_LE_TEXTURE(g_hash_table_lookup(self->textures, GINT_TO_POINTER(texture_id)));
  if (texture == nullptr) {
    return FL_METHOD_RESPONSE(
        fl_method_error_response_new("unknown_texture", "unknown textureId", nullptr));
  }

  fl_texture_registrar_mark_texture_frame_available(self->texture_registrar, FL_TEXTURE(texture));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
}

static FlMethodResponse* handle_dispose_texture(LayoutEnginePlugin* self, FlValue* args) {
  FlValue* texture_id_value = fl_value_lookup_string(args, "textureId");
  if (texture_id_value == nullptr || fl_value_get_type(texture_id_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(
        fl_method_error_response_new("bad_args", "disposeTexture requires textureId", nullptr));
  }

  int64_t texture_id = fl_value_get_int(texture_id_value);
  FlLeTexture* texture =
      FL_LE_TEXTURE(g_hash_table_lookup(self->textures, GINT_TO_POINTER(texture_id)));
  if (texture != nullptr) {
    fl_texture_registrar_unregister_texture(self->texture_registrar, FL_TEXTURE(texture));
    g_hash_table_remove(self->textures, GINT_TO_POINTER(texture_id));
  }
  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
}

#ifdef LE_LINK_BACKEND_ENABLED
static void delete_tcl_bridge(gpointer data) {
  delete reinterpret_cast<le::TclBridge*>(data);
}
#endif

static FlMethodResponse* handle_create_tcl_console(LayoutEnginePlugin* self, FlValue* args) {
#ifdef LE_LINK_BACKEND_ENABLED
  FlValue* handle_address_value = fl_value_lookup_string(args, "handleAddress");
  if (handle_address_value == nullptr ||
      fl_value_get_type(handle_address_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "bad_args", "createTclConsole requires handleAddress", nullptr));
  }

  int64_t console_id = self->next_console_id++;
  const std::string exe_dir = ExecutableDir();
  le::TclBridge* bridge = new le::TclBridge(fl_value_get_int(handle_address_value),
                                             exe_dir + "/lib/le_tcl.so",
                                             exe_dir + "/lib/le_tcl_procs.tcl");
  g_hash_table_insert(self->tcl_consoles, GINT_TO_POINTER(console_id), bridge);

  g_autoptr(FlValue) result = fl_value_new_int(console_id);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
#else
  return FL_METHOD_RESPONSE(fl_method_error_response_new(
      "backend_not_linked", "built without LE_LINK_BACKEND - see this plugin's CLAUDE.md",
      nullptr));
#endif
}

static FlMethodResponse* handle_eval_tcl_command(LayoutEnginePlugin* self, FlValue* args) {
#ifdef LE_LINK_BACKEND_ENABLED
  FlValue* console_id_value = fl_value_lookup_string(args, "consoleId");
  FlValue* command_value = fl_value_lookup_string(args, "command");
  if (console_id_value == nullptr || fl_value_get_type(console_id_value) != FL_VALUE_TYPE_INT ||
      command_value == nullptr || fl_value_get_type(command_value) != FL_VALUE_TYPE_STRING) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "bad_args", "evalTclCommand requires consoleId and command", nullptr));
  }

  int64_t console_id = fl_value_get_int(console_id_value);
  le::TclBridge* bridge =
      reinterpret_cast<le::TclBridge*>(g_hash_table_lookup(self->tcl_consoles, GINT_TO_POINTER(console_id)));
  if (bridge == nullptr) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "unknown_console", "no Tcl console with that id", nullptr));
  }

  const std::string eval_result = bridge->evalTcl(fl_value_get_string(command_value));
  g_autoptr(FlValue) result = fl_value_new_string(eval_result.c_str());
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
#else
  return FL_METHOD_RESPONSE(fl_method_error_response_new(
      "backend_not_linked", "built without LE_LINK_BACKEND - see this plugin's CLAUDE.md",
      nullptr));
#endif
}

static FlMethodResponse* handle_dispose_tcl_console(LayoutEnginePlugin* self, FlValue* args) {
#ifdef LE_LINK_BACKEND_ENABLED
  FlValue* console_id_value = fl_value_lookup_string(args, "consoleId");
  if (console_id_value == nullptr || fl_value_get_type(console_id_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(
        fl_method_error_response_new("bad_args", "disposeTclConsole requires consoleId", nullptr));
  }

  int64_t console_id = fl_value_get_int(console_id_value);
  // Destroy func (delete_tcl_bridge) frees the le::TclBridge itself.
  g_hash_table_remove(self->tcl_consoles, GINT_TO_POINTER(console_id));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
#else
  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
#endif
}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                           gpointer user_data) {
  LayoutEnginePlugin* self = LEF_EDITOR_PLUGIN(user_data);

  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);

  g_autoptr(FlMethodResponse) response = nullptr;
  if (args == nullptr || fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
    response = FL_METHOD_RESPONSE(
        fl_method_error_response_new("bad_args", "expected a map of arguments", nullptr));
  } else if (strcmp(method, "createTexture") == 0) {
    response = handle_create_texture(self, args);
  } else if (strcmp(method, "markTextureFrameAvailable") == 0) {
    response = handle_mark_texture_frame_available(self, args);
  } else if (strcmp(method, "disposeTexture") == 0) {
    response = handle_dispose_texture(self, args);
  } else if (strcmp(method, "createTclConsole") == 0) {
    response = handle_create_tcl_console(self, args);
  } else if (strcmp(method, "evalTclCommand") == 0) {
    response = handle_eval_tcl_command(self, args);
  } else if (strcmp(method, "disposeTclConsole") == 0) {
    response = handle_dispose_tcl_console(self, args);
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  g_autoptr(GError) error = nullptr;
  if (!fl_method_call_respond(method_call, response, &error)) {
    g_warning("Failed to send layout_engine_plugin response: %s", error->message);
  }
}

static void layout_engine_plugin_dispose(GObject* object) {
  LayoutEnginePlugin* self = LEF_EDITOR_PLUGIN(object);
  g_clear_pointer(&self->textures, g_hash_table_unref);
  g_clear_pointer(&self->tcl_consoles, g_hash_table_unref);
  g_clear_object(&self->texture_registrar);
  G_OBJECT_CLASS(layout_engine_plugin_parent_class)->dispose(object);
}

static void layout_engine_plugin_class_init(LayoutEnginePluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = layout_engine_plugin_dispose;
}

static void layout_engine_plugin_init(LayoutEnginePlugin* self) {
  self->textures = g_hash_table_new_full(g_direct_hash, g_direct_equal, nullptr, g_object_unref);
  // delete_tcl_bridge is only defined when LE_LINK_BACKEND_ENABLED - fine
  // to pass nullptr (no destroy needed) otherwise, since
  // handle_create_tcl_console never actually inserts anything into this
  // table in that build configuration.
  self->tcl_consoles = g_hash_table_new_full(
      g_direct_hash, g_direct_equal, nullptr,
#ifdef LE_LINK_BACKEND_ENABLED
      delete_tcl_bridge
#else
      nullptr
#endif
  );
  self->next_console_id = 0;
}

void layout_engine_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  LayoutEnginePlugin* plugin =
      LEF_EDITOR_PLUGIN(g_object_new(layout_engine_plugin_get_type(), nullptr));
  plugin->texture_registrar =
      FL_TEXTURE_REGISTRAR(g_object_ref(fl_plugin_registrar_get_texture_registrar(registrar)));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar), "layout_engine_plugin", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb, g_object_ref(plugin),
                                            g_object_unref);

  g_object_unref(plugin);
}
