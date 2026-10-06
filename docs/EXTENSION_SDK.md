# Layout Engine Extension SDK

How to write an extension: your own C++, Tcl commands and data files, kept
in your own repository and built into Layout Engine without editing it.
The design and the reasons behind it are in
[EXTENSION_MECHANISM_RESEARCH.md](EXTENSION_MECHANISM_RESEARCH.md); this page
is the reference.

**Extension API version: 1** (Layout Engine 0.3.x, the first with extension support). See the [changelog](#changelog).

Not available yet: drawing on the design view (Compose overlays, #76) and
drawing extension objects (#77).
Projects install extensions with the `le` package manager
(`tools/le/README.md`); while developing one, build it in with
`LE_EXTENSION_DIRS`, as below.

## Two kinds of extension

| | Script | Compiled |
|---|---|---|
| Contains | Tcl procs, data files | C++ (and usually Tcl too) |
| Needs a compiler | no | yes: Layout Engine is rebuilt with it |
| Manifest `[contents]` | `tcl_procs`, `resources`, `tcl_tests` | adds `cmake` |
| Example | `examples/extensions/hello_script` | `examples/extensions/hello_ext` |

A compiled extension is compiled into `le_shell` and the `le_tcl` module.
There's no binary plugin interface, because extensions are built from source
together with Layout Engine.

## Quick start

1. Copy `examples/extensions/hello_script` (or `hello_ext`) into your own
   repository and rename it. The directory name doesn't matter; the
   manifest's `name` does.
2. Build Layout Engine with it. Paths are absolute, or relative to the
   Layout Engine source tree; separate several with `;` and quote them:

   ```
   cmake -S layout_engine -B build -DLE_EXTENSION_DIRS="/path/to/my_ext;/path/to/other_ext"
   cmake --build build -j
   ctest --test-dir build -R my_ext
   ./build/le_shell
   ```

   Configure prints `-- Extension my_ext 0.1.0 (compiled, /path/to/my_ext)`
   for each extension, or every problem it found with the manifests.
3. To ship a self-contained copy, install the bundle (extensions included):

   ```
   cmake --install build --component bundle --prefix /path/to/bundle
   ```

## Directory layout

```
my_ext/
  le_extension.toml          manifest (required)
  le_extension.cmake         build details (compiled extensions only)
  include/my_ext/...         public headers
  src/...                    core C++: no Tcl or GUI dependency
  tcl/my_ext.i               SWIG declarations of the Tcl commands
  tcl/my_ext_tcl.cpp         the commands' C++
  tcl/my_ext.tcl             procs: flags, help, calls into the commands
  gui/my_ext_gui.cpp         windows and menu items (Dear ImGui)
  tests/...                  GoogleTest sources and Tcl test scripts
  data/...                   resources
```

Nothing outside this directory is referenced. Every manifest path must stay
inside it.

## The manifest: `le_extension.toml`

```toml
[extension]
name        = "my_ext"           # required
version     = "1.2.0"            # required
prefix      = "MyExt"            # required
description = "What it does"     # optional

[compatibility]
layout_engine = ">=0.3, <0.4"    # optional, recommended
extension_api = 1                # required

[dependencies]                   # optional
other_ext = ">=2.0"

[contents]                       # all optional
cmake      = "le_extension.cmake"
schema     = "schema_ext.py"
migrations = "migrations/"       # default: migrations/ beside the schema
tcl_procs  = ["tcl/my_ext.tcl"]
tcl_tests  = ["tests/my_ext_test.tcl"]
resources  = ["data/"]
```

| Key | Meaning |
|---|---|
| `name` | snake_case (`[a-z][a-z0-9_]*`), unique among the extensions in a build. It names your C++ symbols (`le_ext_<name>_register`), targets (`<name>_core`) and install directory (`ext/<name>/`). |
| `version` | `MAJOR.MINOR.PATCH`: your package's version. |
| `prefix` | PascalCase, unique among the extensions in a build. Start your class names with it; start your Tcl commands with its snake_case form (`MyExt` → `my_ext_...`). |
| `layout_engine` | Comma-separated constraints (`>=`, `>`, `<=`, `<`, `==`, `!=`; a bare version means `==`; omitted parts are 0, so `>=0.2` means `>=0.2.0`). The build refuses a Layout Engine version outside it. |
| `extension_api` | The SDK version you wrote against. The build refuses any other. |
| `[dependencies]` | Other extensions, each with a constraint. They must be in the same build, and they load before you. |
| `cmake` | Your build file; its presence makes the extension compiled. |
| `schema` | Your database classes (see [Database classes](#database-classes)); also makes the extension compiled. |
| `migrations` | Your schema migrations; defaults to `migrations/` beside the schema. |
| `tcl_procs` | Tcl files sourced at startup, after Layout Engine's own procs and your dependencies', in the order listed. |
| `tcl_tests` | Tcl scripts run as ctests (see [Testing](#testing)). |
| `resources` | Files or directories installed into the bundle under `ext/<name>/`, at the same relative paths. |

Extensions load in dependency order; extensions that don't depend on each
other load in name order. Configure reports every manifest problem at once:
bad names, a clashing name or prefix, an incompatible version, a missing
dependency or a cycle.

## `le_extension.cmake`

A compiled extension's build file calls `le_add_extension()` once, with
paths relative to the extension directory:

```cmake
le_add_extension(my_ext
    CORE_SOURCES  src/my_ext.cpp          # required; must define le_ext_my_ext_register
    CORE_INCLUDE  include                 # your public include directories
    TESTS         tests/my_ext_test.cpp   # GoogleTest, built as my_ext_tests
    TCL_SWIG      tcl/my_ext.i            # SWIG declarations of your Tcl commands
    TCL_SOURCES   tcl/my_ext_tcl.cpp      # their C++ (needs Tcl; le_tcl only)
    TCL_INIT                              # TCL_SOURCES define le_ext_my_ext_init_tcl
    GUI_SOURCES   gui/my_ext_gui.cpp      # windows and menus; define le_ext_my_ext_register_gui
    LINK          Boost::graph            # your own third-party dependencies
)
```

This builds `my_ext_core` (a static library linked against
`le::extension_sdk`), with Tcl `my_ext_tcl` (linked into the `le_tcl`
module only), and with `GUI_SOURCES` `my_ext_gui` (linked into `le_shell`
only). Your core library must not depend on Tcl or the GUI. Its tests
can run without either.

## C++: `<le/extension.hpp>`

### Registration

Every compiled extension defines exactly this function, and states the API
version it targets:

```cpp
#include <le/extension.hpp>

static_assert(LE_EXTENSION_API_VERSION == 1, "my_ext targets extension API v1");

void le_ext_my_ext_register(le::ext::Registry &registry)
{
}
```

At startup, `le::ext::register_all()` adds each extension's name and version
to the registry, then calls its register function, in dependency order and
once. In API 1 the registry only lists extensions
(`le::ext::registry().extensions()`, or `le_extension_count`/
`le_extension_name`/`le_extension_version` from the C API); windows and
menus register separately, from `GUI_SOURCES` (below).

### `ExtensionContext`: working with a session

A session is one `LeHandle`, the database and view state that `le_shell`'s
console and GUI share. Make a context for it with your extension's name:

```cpp
le::ext::ExtensionContext ctx(handle, "my_ext");
```

| Member | Use |
|---|---|
| `handle()` | The `LeHandle *`, for any C API call (`le_*`, `api.hpp`). |
| `read()` | A `ReadView`: `view.root()` is a `const le::Root &`, held under a shared lock for the view's lifetime. |
| `write()` | A `WriteView`: `view.root()` is a writable `le::Root &`, held under an exclusive lock. When it ends, it bumps the database's mutation version and wakes the renderer. |
| `transaction(label)` | A `Transaction`: the C API edits made while it lives become one undo step named `label`. Call `fail()` if the operation failed. It does nothing if a transaction is already open, e.g. inside a typed Tcl command, which already is one. |
| `data<T>()` | Your state of type `T` for this session, default-constructed on first use and destroyed with the session. |

**Undo.** Only the C API's create, update and delete calls
(`le_create_<type>`, `le_update_<type>`, `le_delete_<type>`, ...) record undo
steps. For an undoable edit, call them through `handle()` inside a
`transaction()`:

```cpp
bool add_thing(le::ext::ExtensionContext &ctx, const std::string &name)
{
    le::ext::Transaction transaction = ctx.transaction("my_ext_add " + name);
    if (le_create_library(ctx.handle(), name.c_str()).index == UINT32_MAX)
    {
        transaction.fail();
        return false;
    }
    return true;
}
```

Edits through `write()` are not undoable. Use it for bulk work, such as
importing data, where undo isn't wanted.

**Locks.** Don't call `le_*` functions while you hold a `ReadView` or
`WriteView`: they take the same lock. Keep views short, because a view
blocks rendering (`write()`) or edits (`read()`).

**State.** Keep per-session state in `data<T>()`, never in globals or
statics. `le_shell` and the `le_tcl` module each link their own copy of your
extension, so a global would exist twice and the two copies would diverge.
State is per extension and per type: `data<State>()` in `my_ext` and in
`other_ext` are different objects.

## Database classes

`schema_ext.py` adds classes to Layout Engine's database. They become
ordinary database classes: pools and `<Type>Id` handles, parent/child
navigation, delete cascades, undo, the change log, `.led` files, the
property viewer, and generated `get_`/`create_`/`update_`/`delete_<type>`
Tcl commands with help.

```python
from codegen.schema import Field, Klass

VERSION = "0.1.0"   # this schema's version, separate from the package's

def extend(schema):
    schema.classes.append(Klass(
        name="MyExtNote",
        description="A note on a library",
        fields=[
            Field(name="library", description="Its library", type="Library", parent="my_ext_notes"),
            Field(name="text", description="The text", type="str", example="hi"),
        ],
    ))
```

- **Names:** every class starts with your prefix (`MyExt...`), and no
  class or generated name may collide with another class's.
- **Classes you don't own are read-only.** You can't add, change or
  remove fields of core classes or another extension's classes. Keep
  per-object data in a class of your own with `parent=` on theirs. The
  parent's child list (`Library.my_ext_notes`, `get_library_my_ext_notes`)
  is generated for you, because a child list isn't stored.
- **History:** the build writes `schema_history/<VERSION>.json` beside
  `schema_ext.py`. Commit it. Changing the schema without bumping
  `VERSION` fails the build, as for core.
- **Migrations:** once a version has shipped, changing the schema means
  bumping `VERSION` and adding a migration, so files written by the old
  version still load. Draft it with

  ```
  PYTHONPATH=layout_engine/codegen python3 -m codegen.cli --schema layout_engine/src/database/schema.py \
      --extension /path/to/my_ext --target makemigration --migrate-extension my_ext --name what_changed
  ```

  It diffs your newest snapshot against `schema_ext.py`, asks about
  renames, and writes `migrations/NNNN_what_changed.py`, recording the
  core schema version it was written against (`depends_on_core`). Review
  it, then build. The rules:
  - your ops may only change your own classes;
  - when loading a file, each of your migrations runs right after the core
    migration it depends on, so the names it uses are the ones it was
    written against;
  - a core migration that renames a class you refer to carries your data
    along: the build names the `schema_ext.py` line to update, and no
    migration of yours is needed.
- **Files:** a `.led` file records each extension it holds objects of,
  with its package and schema versions. A build without that extension, or
  with an older schema of it, refuses the file and names what's missing. A
  file holding none of your objects doesn't mention you, so it opens
  anywhere.
- **Golden files:** keep a `.led` file per schema version and load them
  all in your tests (old ones go through your migrations), as `hello_ext`'s `EverySchemaVersionsGoldenFileStillLoads`
  does, so a later change that breaks old files fails your CI.

## GUI: `<le/extension_gui.hpp>`

`GUI_SOURCES` define one more function, which adds windows and Extensions
menu items. Draw with Dear ImGui (`<imgui.h>`):

```cpp
#include <le/extension_gui.hpp>
#include <imgui.h>

namespace
{
    void draw_window(le::ext::ExtGuiContext &ctx)
    {
        if (const le::ext::ReadView view = ctx.read(); view.valid())
            ImGui::Text("Libraries: %zu", view.root().get_library_ids().size());
        if (ImGui::Button("Add one"))
            ctx.run_tcl_command("my_ext_add_thing");
    }
}

void le_ext_my_ext_register_gui(le::ext::GuiRegistry &registry)
{
    registry.add_window({.title = "My Ext", .dock = le::ext::Dock::RIGHT, .draw = draw_window});
    registry.add_menu_item({.label = "Do it", .action = [](le::ext::ExtGuiContext &ctx) { ctx.run_tcl_command("my_ext_do_it"); }});
}
```

- **Windows** dock `LEFT`, `RIGHT`, `BOTTOM` or `CENTER` (a tab beside the
  design view) in the default layout, get a close button, and are listed in
  the Window menu with the core panels. Whether each is open is saved with
  the window layout; `open_by_default = false` starts one closed.
- **Menu items** go in your extension's own submenu of the Extensions menu
  (Extensions → my_ext → Do it).
- **Toolbar buttons** (`add_toolbar_button`) follow the core ones in the
  design view's toolbar, in the modes you choose (`TOOLBAR_SELECT`,
  `TOOLBAR_EDIT`, `TOOLBAR_RULER`; all by default).
- **Shortcuts** (`add_key_binding`: an `ImGuiKey` plus Ctrl/Shift/Alt)
  work while the mouse is over the design view and no text field has
  focus. Keys Layout Engine uses (Z F D S E R M, 0-9, the arrows, Escape,
  Delete, with any modifiers) are refused, and so is a combination another
  extension took first; each refusal is logged.
- **Fonts:** `add_icon_glyphs(file, ranges)` merges glyphs into the icon
  fonts, so a toolbar `icon` can be one of them; `add_font(name, file,
  size)` adds a font for your windows, fetched with `ctx.font(name)`.
  Files are relative to your extension directory: list them under
  `resources` so the bundle installs them.
- **Settings:** register a section with `Registry::add_settings` (in
  `le_ext_my_ext_register`): a format `version` and `save`/`load`
  callbacks over `nlohmann::json` (the SDK provides it). It's saved in
  `settings.json` as `"extensions": {"my_ext": {"version": 1, ...}}`, and
  the Settings panel's Save/Load and unsaved-changes check include it. The
  callbacks run with the session locked: use only `ctx.data<T>()`.
  `load` gets the version the section was written at, so migrating an
  older one is up to you. A build without your extension keeps your
  section as it was. `GuiRegistry::add_settings_panel` adds a collapsible
  section to the Settings panel to edit them.
- **`ExtGuiContext`** is valid for one draw or menu call:
  - `read()` never waits. It returns an invalid view while an edit holds
    the database, so keep what you need in `data<T>()` and draw that instead.
  - `run_tcl_command()` queues a command for the console, as if typed. It's
    the way to change the design from the GUI: the command shows in the
    console's history and is one undo step.
  - `data<T>()` is the same per-session state your Tcl commands see.
  - `is_busy()`, `selection_count()` and `selected_object(i)` cover the
    rest.

## Tcl commands

Extension commands follow the same three layers as Layout Engine's own, so
they get `help`, `man`, tab completion and `TCL_COMMANDS.md` entries for
free.

1. **C++** (`TCL_SOURCES`): plain functions. Get the session with
   `le::ext::tcl_session()` from `<le/extension_tcl.hpp>`:

   ```cpp
   #include <le/extension_tcl.hpp>

   int my_ext_thing_count_cmd()
   {
       le::ext::ExtensionContext ctx(le::ext::tcl_session(), "my_ext");
       return my_ext::thing_count(ctx);
   }
   ```

2. **SWIG** (`TCL_SWIG`): declare them, including the header the wrapper
   needs:

   ```
   %{
   #include "my_ext/my_ext_tcl.hpp"
   %}
   int my_ext_thing_count_cmd();
   ```

   Keep parameters and results to plain C types (`int`, `double`,
   `const char *`, `bool`) and return status codes rather than throwing.

3. **Procs** (`tcl_procs`): the user-facing command. It parses flags, calls
   the `_cmd` function, raises Tcl errors, and registers its help right
   after its definition:

   ```tcl
   proc my_ext_thing_count {args} {
       if {[lsearch -exact $args "-help"] >= 0} {
           return "my_ext_thing_count \[-help\] - Returns how many things there are"
       }
       return [my_ext_thing_count_cmd]
   }
   register_command_help my_ext_thing_count \
       "my_ext_thing_count \[-help\]" \
       "Returns how many things the session has." \
       {
           {-help {type flag required 0 description {Show this usage message and return immediately}}}
       }
   ```

   An options entry is `{-flag {type T required R description {D}}}`, or
   `{<name> {...}}` for a positional argument. `type` is a label shown in
   help (`str`, `int`, `double`, `bool`, `flag`, `token`, `file`). A
   positional argument of type `file` also gets filename completion.

A command typed in the console is already one undo step, so a
`transaction()` inside it simply joins it.

**The escape hatch.** For a command SWIG can't express, such as one taking a
script or a callback, add `TCL_INIT` and define:

```cpp
void le_ext_my_ext_init_tcl(Tcl_Interp *interp)
{
    Tcl_CreateObjCommand(interp, "my_ext_raw", my_raw_command, nullptr, nullptr);
}
```

It runs once, when Tcl loads the `le_tcl` module.

**Names.** Start every command with your prefix's snake_case form. Don't
redefine Layout Engine's commands: it may work, because the last definition
wins, but it isn't supported.

## At run time

`le_shell` loads extensions from an index, `extensions.json`. The build
writes one in the build directory, and `cmake --install` writes the bundle's
own beside `le_shell`, with your procs and resources under `ext/<name>/`.
`le_shell` uses the first of:
1. `-extensions <file>`
2. the `LE_EXTENSIONS_PATH` environment variable
3. the `extensions.json` beside the executable
4. the build directory's index

It refuses an index written for another Layout Engine version or API
version, or one that doesn't match the extensions compiled into it, and
names the problem. In a session, `$::le_extensions_index` shows which index
was used.

## Testing

- **C++:** `TESTS` sources build into `<name>_tests` and run under `ctest`
  as `<name>.<Suite>.<Test>`. They link every extension, so
  `le::ext::register_all()` works in them.
  Make sessions with `le_create()`/`le_destroy()`, as `hello_ext`'s tests do.
- **Tcl:** each `tcl_tests` script runs as a ctest (`<name>.<script>`)
  through the build's `le_shell`, with every extension loaded. It passes if
  the script finishes, and fails if it raises an error:

  ```tcl
  if {[my_ext_thing_count] != 0} {
      error "expected no things, got [my_ext_thing_count]"
  }
  ```

## What's public

Only these are the SDK. Anything else in Layout Engine may change in any
release without notice.
- `<le/extension.hpp>`, `<le/extension_tcl.hpp>` and `<le/extension_gui.hpp>`
- `le_add_extension()` and the `le_extension.toml` format
- `schema_ext.py`'s `VERSION`/`extend()` contract and the `codegen.schema`
  `Klass`/`Field` it uses
- the C API (`api.hpp`) and the database classes (`le::Root`, the
  `<Type>Data` structs and `<Type>Id` handles, generated from `schema.py`)
- the Tcl helpers `register_command_help`, `help` and `man`
- the `extensions.json` format and `le_shell`'s `-extensions`/`LE_EXTENSIONS_PATH`

The database classes follow Layout Engine's schema, which changes between
versions; renamed or removed fields there show up as compile errors.

**Versioning.** `LE_EXTENSION_API_VERSION` goes up by one whenever any of
the above changes in a way that can break a correct extension. Each bump is
listed below with what to change. Additions that can't break anything, such
as new functions or new optional manifest keys, don't bump it.

## Changelog

### API 1: Layout Engine 0.3

The first version.
- `le_extension.toml` with `[extension]`, `[compatibility]`,
  `[dependencies]` and `[contents]` (`cmake`, `tcl_procs`, `tcl_tests`,
  `resources`, `schema`, `migrations`).
- `le_add_extension()` with `CORE_SOURCES`, `CORE_INCLUDE`, `TESTS`,
  `TCL_SWIG`, `TCL_SOURCES`, `TCL_INIT`, `GUI_SOURCES` and `LINK`.
- `le::ext::Registry`, `register_all()`, `ExtensionContext` (`read`,
  `write`, `transaction`, `data`), `ReadView::valid()`, `tcl_session()`,
  `init_tcl()`.
- GUI: `GuiRegistry` (`add_window`, `add_menu_item`, `add_toolbar_button`,
  `add_key_binding`, `add_settings_panel`, `add_icon_glyphs`, `add_font`),
  `GuiWindow`, `Dock`, `GuiMenuItem`, `GuiToolbarButton`, `ToolbarModes`,
  `GuiKeyBinding`, `ExtGuiContext` (with `font`), `register_all_gui()`.
- Settings: `Registry::add_settings`, `SettingsSection`; `ExtensionInfo::directory`
  and `Registry::resource`.
- Database classes: `schema_ext.py` (`VERSION`, `extend(schema)`), its
  `schema_history/` and `migrations/` (`Migration(extension=...,
  depends_on_core=...)`, `--migrate-extension`), and the `"extensions"`
  entry in `.led` files.
- C API: `le_extension_count`, `le_extension_name`, `le_extension_version`.
- `extensions.json` format 1.
