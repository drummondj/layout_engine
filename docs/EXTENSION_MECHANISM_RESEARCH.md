# Extension Mechanism — Architecture Research

Companion documents: [NATIVE_FILE_FORMAT_RESEARCH.md](NATIVE_FILE_FORMAT_RESEARCH.md) (how extension data is saved and migrated) and [PACKAGE_MANAGER_RESEARCH.md](PACKAGE_MANAGER_RESEARCH.md) (how users install extensions into a project). The package manager decides how an extension is described and found, so §2 and §6 follow its design.

## Goal

Let a customer add their own proprietary features to Layout Engine:

1. new objects in the schema
2. their own C++ modules
3. TCL commands that call those modules
4. GUI windows with their own ImGui widgets

Their code must live **completely outside** the layout_engine tree, yet be **compiled together** with it. Upgrading to a newer layout_engine should mean "bump a version and rebuild", not "re-apply a patch set".

## Summary of the recommendation

| Concern | Recommendation |
|---|---|
| Packaging | **An extension is a directory with a declarative manifest, `le_extension.toml`** (name, version, prefix, compatible layout_engine versions, dependencies, what it contains). The package manager reads it to resolve and fetch; CMake reads it to build. **Superbuild:** layout_engine's CMake takes extension directories in `LE_EXTENSION_DIRS`. The package manager generates that list for a project, and a hand-made superbuild (layout_engine as a git submodule) still works. |
| Tiers | **Script extensions** (Tcl procs and data files only) need no compiler and install into a prebuilt release. **Compiled extensions** (C++, schema, GUI) need a project build of `le_shell`. |
| Build | layout_engine provides an `le_add_extension()` CMake function. Each extension builds as up to three static libraries (`_core`, `_tcl`, `_gui`) that are linked into the existing host targets. |
| Registration | CMake generates an init file that calls `le_ext_<name>_register(Registry&)` for each extension. This is explicit; nothing relies on static self-registration. |
| Schema | Extension classes are **merged into the core schema at build time**. `schema_ext.py` defines `extend(schema)`, and CMake runs codegen into the build directory. Extension objects get pools in `Root`, undo/redo, the change log and TCL CRUD commands for free. |
| C++ | A narrow `le::extension_sdk` interface target containing `le/extension.hpp` (`ExtensionContext`). The contract is the source API; ABI is not a concern because everything is compiled together. |
| TCL | The same three layers as core: shim, SWIG `.i`, and a procs `.tcl` file that calls `register_command_help`. The extension's `.i` files are `%include`d into `le_tcl`, and its procs are sourced after the core procs. |
| GUI | Extensions register their windows in the registry, and the core windows move onto the same list. Also: a new main menu bar, toolbar/key/overlay hooks, and extension-owned sections in both settings files. |
| Rendering | **Codegen-driven.** A `render=Render(...)` declaration on a `Klass` generates its purposes, render-tree chunk, hit-testing and property-viewer entry; core's closed purpose/object-kind lists become generated tables (§8). A C++ `emit` hook is added only for synthesized geometry. |
| Stability | A version check on `LE_EXTENSION_API_VERSION` (`static_assert`), plus an in-tree `examples/extensions/hello_ext` that CI builds. |

**Rejected:**
- Runtime `dlopen` plugins. There is no stable ABI for a templated, header-only database whose `Root` layout changes whenever the schema does.
- Maintaining a fork or patch set, because every upgrade becomes a merge.

---

## 1. Where things stand today

Layout Engine is a closed world today: there are no extension points anywhere. Relevant facts, with paths relative to the repo root:

**Schema and codegen**
- `src/database/schema.py` is a Python DSL: `Schema(name="layout_engine", namespace="le", version="0.50.0", classes=[Klass(...), ...])`.
- `codegen` is vendored in-tree (`codegen/`, this project's fork of cmg). `codegen/codegen/cli.py` accepts a single `--schema` and four targets: `database`, `tcl`, `makemigration` and `checkmigrations`. It loads the schema with `SourceFileLoader(...).load_module()` (`codegen/codegen/generator.py:33`). Because that is plain Python, a schema file can already `import` another schema and append to it.
- Generated code is **not** committed: CMake runs codegen (`le_codegen` target, `CMakeLists.txt`) into `<build>/generated/` whenever the schema, its history or codegen changes. `add_library(database INTERFACE)` depends on it.
- Codegen produces a single `class Root` (generated `root.hpp`) with one `Pool` per class, plus a closed `enum class ChangeKlass`. The undo/redo templates in `src/editing/command.hpp` and the TCL CRUD surface are also generated per class.
- The native `.led` format (`src/io/native_format.*`, `write_db`/`read_db`) is driven by codegen's `native_tables.hpp`, so classes merged into the schema are persisted without hand-written code.

**Build**
- The top-level `CMakeLists.txt` (at the repo root) is flat. It has no `add_subdirectory` of project code, no install/export rules, and no `LE_EXTRA_*` options.
- `api` is a static library (`:583`) that keeps its internals `PRIVATE`.
- `le_tcl` is a SWIG-built Tcl module (`swig_add_library`, `:679`) that links `api` privately.
- `le_shell` (`:859`) is both the Tcl shell and the GUI app, and also links `api` (`:862`). The process therefore contains **two copies of `api`**, one in the executable and one in the loaded module, and they share the `LeHandle*` passed through `set_session_handle`. On macOS the module hides its duplicate symbols (`src/tcl/le_tcl_unexported_symbols.txt`) so each image binds to its own copy. This matters for where extension state may live (§3).
- Releases are a flat, prebuilt bundle (`Dockerfile.linux-release`): `le_shell`, `le_tcl.so`, the procs files, `fonts/` and `libtbb`. Resources are found with `find_resource` (`src/core/resource_path.hpp`): the compile-time build-tree path first, else a path relative to the executable.

**TCL**
- A command passes through four layers:
  1. the C API in `src/api/api.hpp`
  2. the shim in `src/tcl/le_tcl_shim.cpp`, which reaches the handle through `session()`
  3. the SWIG interface `src/tcl/le_api.i`, which already ends with `%include "generated/tcl/le_api_generated.i"` (`:213`)
  4. the procs in `src/tcl/le_tcl_procs.tcl`, which parse flags and call `register_command_help` (`:114`)
- The `::command_help` dict drives `help`, `man`, tab completion and `TCL_COMMANDS.md`.
- `le_shell.cpp`'s `app_init` (`:171`) runs `load {module} le_tcl` (`:178`), then `set_session_handle` (`:191`), then `Tcl_EvalFile(procs)` (`:198`).

**GUI** (`src/gui/le_gui.cpp`)
- Panels are hard-coded `Begin` / `draw_xxx(provider)` / `End` calls inside the frame loop (`:1162-1226`), with no close button, and their dock slots are fixed in `DockBuilder` code (`:797-821`).
- There is **no menu bar**.
- The toolbars are hard-coded, and so is the key table (`kKeyMappings`, `:256`).
- `GuiProvider` (`src/gui/gui_provider.hpp:25`) is the only way into `LeHandle`, and it deliberately exposes no raw handle.

**Rendering and settings**
- Overlays are a fixed list of `draw_*_overlay` calls in `ComposeStage::compute` (`src/pipelines/stages/compose_stage.hpp:141-147`).
- `ViewLayerPurpose` is a closed enum (`src/pipelines/view_style.hpp:17`), mirrored by hand in `layer_manager.cpp` and `le_tcl_procs.tcl`.
- Settings are fixed JSON keys (`settings_to_json`, `src/api/api.cpp:734`). The format is versioned (`kSettingsVersion`, with one migration step per version), and top-level keys this build doesn't know are kept and written back (`LeHandle::unknown_settings_json`). So an extension's settings section already survives a session in a build without that extension.
- Window state is saved in `~/.layout_engine/window_layout.ini`, using a custom `ImGuiSettingsHandler` (`le_gui.cpp:627`). That same mechanism would work for extensions.

---

## 2. Packaging: a superbuild with extension directories

An extension is one directory. It never refers to paths inside layout_engine, so the same directory works as a package-manager install, in a hand-made superbuild, and in layout_engine's own CI.

```
acme_router/                      <- the extension (its own repo, proprietary)
  le_extension.toml               <- manifest: identity, compatibility, contents
  le_extension.cmake              <- build: sources, libraries (compiled tier only)
  schema_ext.py
  include/acme_router/...
  src/router.cpp                  <- core C++ (no Tcl / ImGui deps)
  tcl/acme_router.i
  tcl/acme_router_shim.cpp
  tcl/acme_router_procs.tcl
  gui/acme_router_window.cpp
  migrations/  schema_history/    <- once its schema has changed (§4)
  tests/...
```

**With the package manager** (the normal route), the user lists `acme_router` in their project's `le_project.toml`. `le install` fetches layout_engine and the extension at the locked versions and configures a build with `LE_EXTENSION_DIRS` set. See [PACKAGE_MANAGER_RESEARCH.md](PACKAGE_MANAGER_RESEARCH.md).

**By hand**, a superbuild repo includes layout_engine as a git submodule:

```cmake
cmake_minimum_required(VERSION 3.25)
project(acme_le CXX)
set(LE_EXTENSION_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/ext/acme_router CACHE STRING "" FORCE)
add_subdirectory(layout_engine)
```

or passes the list at configure time:

```
cmake -S layout_engine -B build -DLE_EXTENSION_DIRS="/path/ext/acme_router;/path/ext/acme_drc"
```

Either way, no extension file lives inside layout_engine, and no layout_engine file is edited.

### `le_extension.toml`

The manifest is data, not code, so the package manager can resolve dependencies without running CMake or Python from an untrusted extension:

```toml
[extension]
name        = "acme_router"          # unique across every extension a project installs
version     = "1.4.0"                # the package version (semver)
prefix      = "Acme"                 # required class / command prefix (§4, §6)
description = "Global routing guides"

[compatibility]
layout_engine = ">=0.9, <0.11"       # resolver checks this before anything is built
extension_api = 1                    # must equal LE_EXTENSION_API_VERSION

[dependencies]
acme_common = ">=2.0"                # other extensions, ordered before this one

[contents]                           # all optional; any C++/schema/GUI entry makes it "compiled"
schema     = "schema_ext.py"
migrations = "migrations"
cmake      = "le_extension.cmake"
tcl_procs  = ["tcl/acme_router_procs.tcl"]
resources  = ["data/"]               # copied to ext/acme_router/ in the bundle
```

The package version and the extension's **schema version** (its migration chain, §4) are separate numbers, just as layout_engine's release version and `Schema.version` are. A release that changes no schema bumps only the package version.

### `le_extension.cmake`

For the compiled tier, layout_engine includes the directory's `le_extension.cmake`, after reading the manifest. That file calls one function with the build details; identity, prefix, schema and procs come from the manifest:

```cmake
le_add_extension(acme_router
    CORE_SOURCES  src/router.cpp
    CORE_INCLUDE  include
    TCL_SWIG      tcl/acme_router.i         # optional
    TCL_SOURCES   tcl/acme_router_shim.cpp
    GUI_SOURCES   gui/acme_router_window.cpp  # optional
    TESTS         tests/router_test.cpp       # optional, added to backend_tests or own gtest exe
    LINK          Boost::graph              # customer's own third-party deps
)
```

CMake has no TOML parser. Configuration therefore runs a small Python helper (shipped with codegen, using the standard library's `tomllib`) that turns each manifest into CMake variables. Python is already a build requirement once CMake runs codegen (§4).

For each extension, `le_add_extension()`:
1. Builds `acme_router_core`, a static library linked against `le::extension_sdk`. It contains no Tcl or ImGui dependency, so it can be unit-tested in isolation.
2. Builds `acme_router_tcl` and links it into **`le_tcl`**.
3. Builds `acme_router_gui` (linked to `gui`) and links it into **`le_shell`**.
4. Links `acme_router_core` into **both** `le_tcl` and `le_shell`, the same way `api` is linked today.
5. Records the extension's name, schema path, `.i` files and procs paths in global properties. Later steps (codegen, init-file generation, the SWIG include list, the procs list) read them from there.

Extensions are processed in dependency order (from `[dependencies]`, ties broken by name), so `register_all()`, schema `extend()` calls and procs sourcing all see a dependency before its dependents. A cycle or a missing dependency is a configure error.

A bundled library needs `-Wl,--whole-archive` (or an explicit reference) only if it relies on static constructors. The registration design in §3 avoids that.

---

## 3. Registration and runtime state

### Explicit, generated registration

It is tempting to use static self-registration, the "`static Registrar r(...)`" pattern. With static libraries it breaks silently, because the linker drops any object file that nothing references. Instead, CMake writes `${CMAKE_BINARY_DIR}/le_extensions_init.cpp`:

```cpp
// generated — do not edit
#include "le/extension.hpp"
void le_ext_acme_router_register(le::ext::Registry &);
void le_ext_acme_drc_register(le::ext::Registry &);
namespace le::ext {
void register_all(Registry &r) {
    le_ext_acme_router_register(r);
    le_ext_acme_drc_register(r);
}
}
```

`le_shell`'s `main` calls `register_all()` once, before it starts the Tcl thread and the GUI loop. `le_tcl`'s module init calls its own `register_all()` for the TCL-side hooks.

An extension's register function:

```cpp
#include <le/extension.hpp>
static_assert(LE_EXTENSION_API_VERSION == 1, "acme_router targets extension API v1");

void le_ext_acme_router_register(le::ext::Registry &r) {
    r.add_tcl_init([](Tcl_Interp *interp, le::ext::ExtensionContext &ctx) { /* optional raw commands */ });
    r.add_window({.title = "Acme Router", .dock = le::ext::Dock::Right,
                  .draw = &acme::draw_router_window, .open_by_default = false});
    r.add_menu_item({.menu = "Extensions/Acme", .label = "Route selected nets",
                     .tcl = "acme_route_nets [get_selection]"});
    r.add_overlay(&acme::draw_congestion_overlay);
    r.add_settings_section("acme_router", &acme::save_settings, &acme::load_settings);
}
```

The `Registry` holds:
- TCL init hooks
- GUI windows
- menu items
- toolbar buttons
- key bindings
- compose overlays
- settings sections
- font-loading hooks

Nothing else about layout_engine needs to know that an extension exists.

### Where extension state lives

Because the process contains **two `api` images** (`le_shell` and the `le_tcl` module), a C++ global declared in an extension would exist **twice**: the Tcl copy and the GUI copy would silently diverge. State must therefore hang off the one object both sides share, `LeHandle`:

```cpp
auto &state = ctx.extension_data<acme::RouterState>();   // one slot per extension, owned by LeHandle
```

This is implemented as a small type-erased map on `LeHandle` (`std::unordered_map<std::string_view, std::unique_ptr<void, Deleter>>`) keyed by extension name. It is created lazily, and it is guarded by the handle's existing `shared_mutex`. Schema objects (§4) live in `Root`, which is already on `LeHandle`, so this problem doesn't arise for them.

---

## 4. Schema objects: merge at build time (recommended)

### How it works

`schema_ext.py`:

```python
from codegen.schema import Klass, Field

def extend(schema):
    schema.classes += [
        Klass(
            name="AcmeRouteGuide",
            tcl_readable=True,
            fields=[
                Field(name="net",   type="Net", parent="acme_route_guides"),  # owned by a core Net (Net's child list is synthesized)
                Field(name="layer", type="Layer"),
                Field(name="box",   type="rect"),
                Field(name="weight", type="double", is_optional=True),
            ],
        ),
    ]
```

The changes in layout_engine are small:
1. **codegen:** add a repeatable `--extension path/to/schema_ext.py` option to `cli.py`. After `schema_loader()` has run, codegen imports each extension module, calls `extend(schema)`, and then runs the normal validation. New validation rules:
   - every extension class name starts with the extension's declared `PREFIX`
   - class, header and TCL names don't collide with core names
   - an extension adds no field to a class it doesn't own (core or another extension's); see below
2. **CMake:** CMake already runs `codegen --target database` and `--target tcl` into `${CMAKE_BINARY_DIR}/generated/` (the `le_codegen` custom command). Extensions add their schema paths to that command's arguments and dependencies; the layout_engine tree is never written to.

**What extension objects get for free, because they are ordinary `Root` classes:**
- Pool storage and `AcmeRouteGuideId` handles
- Index lookups and parent/child navigation (`get_net_acme_route_guides`)
- Delete cascades: deleting a `Net` deletes its guides
- Undo/redo, transactions and change-log entries (`ChangeKlass` is regenerated to include them)
- `get_acme_route_guides`, `create_acme_route_guide`, `update_…`, `delete_…` TCL commands, with help text
- Display in the GUI's generic property viewer (via `to_properties()`)

**Classes an extension doesn't own are read-only.** An extension may not add, change or remove fields on core classes or on another extension's classes; codegen fails the build if `extend()` does. Per-object extension data goes in an object of its own, owned by the core object (`AcmeNetInfo` with `parent="acme_net_info"` onto `Net`). This doesn't modify `Net`'s stored data: a parent's child list is derived (rebuilt in `Root::index_`, never stored in `NetData` or the file), and codegen synthesizes it from the child's `parent=`, so `extend()` never touches the core class. The rule gives every stored table exactly one owner, which keeps upgrades and migrations simple (see "Extension schema migrations" below, and NATIVE_FILE_FORMAT_RESEARCH.md §4.8).

### Alternative considered: a separate extension `Root`

This option runs codegen on the extension schema alone. That produces an `AcmeRoot` with its own pools, which reference core objects through foreign `NetId`s.

**Pros**
- Strong isolation: core generated code is identical with or without extensions.
- In-tree core output stays authoritative.

**Cons**
- Needs a foreign-type import feature in codegen.
- Needs a second change log and a second undo stack, or a merged one.
- Deleting a core `Net` has to cascade into `AcmeRoot`, which requires a cross-root hook in every delete path.
- A second set of index structures.
- Codegen validation requires exactly one parentless class, so extensions need a synthetic root.

This is substantially more engineering, and every cost lands on the parts of the codebase that are hardest to get right (undo and cascades). It is not recommended unless isolation of generated code becomes a hard requirement.

### Open issue: persistence

The native `.led` format is schema-driven (codegen's `native_tables.hpp`), so classes merged in by `extend()` are written and read like core classes. What remains open is the extension migration chain below; see [NATIVE_FILE_FORMAT_RESEARCH.md](NATIVE_FILE_FORMAT_RESEARCH.md).

### Extension schema migrations

Under that proposal, an extension evolves its schema the same way core does. Next to `schema_ext.py`, it keeps:
- its own `schema_history/` snapshots
- its own `migrations/` chain, with its own version number, drafted by `codegen makemigration --extension acme_router`
- its own golden files

Core and extension migrations share **one timeline**:
- Each extension migration records the core version it was written against (`depends_on_core`, filled in automatically).
- When loading, it runs right after that core migration and before the next one.
- Core migrations also rewrite extension data that refers to core classes: references follow renamed classes and removed parents cascade. Core migrations never change an extension class's fields, and extension migrations may only write the extension's own classes.

So most core schema changes need **no extension migration**. At most the build names the `schema_ext.py` lines to update. Only a core change that removes or reshapes something the extension relies on needs a new extension migration. The full rules are in NATIVE_FILE_FORMAT_RESEARCH.md §4.8.

Add to `le_add_extension()`:

```cmake
    SCHEMA          schema_ext.py
    MIGRATIONS      migrations/       # optional until the extension's first schema change
    GOLDEN_FILES    tests/golden/     # loaded by the extension's tests through the merged plan
```

---

## 5. C++ modules: a narrow extension SDK

*Built:* an INTERFACE target, `le::extension_sdk`, exposes `src/extension/le/extension.hpp`, the C API and the database headers:

```cpp
namespace le::ext {
class ExtensionContext {
public:
    ExtensionContext(LeHandle *handle, std::string_view extension_name);
    LeHandle *handle() const;               // for C API calls (le_*)
    ReadView read() const;                  // shared lock; const Root&
    WriteView write();                      // exclusive lock; Root&; bumps the mutation version and wakes the renderer when it ends
    Transaction transaction(const std::string &label);  // groups C API edits into one undo step
    template <class T> T &data();           // per-session, per-extension state
};
}
```

- **Undo:** only the C API's generated `le_create_<type>`/`le_update_<type>`/`le_delete_<type>` record undo steps. So an undoable edit calls those through `handle()` inside a `transaction()`. `write()` is for bulk edits that aren't undoable, such as a reader.
- `log()` was left out until an extension needs it; spdlog is reachable through the database headers anyway.
- The rest of `api` (pipelines, LeHandle internals, io) stays `PRIVATE`. The smaller the SDK, the fewer upgrade breaks.
- Everything is compiled together, so **ABI does not matter**. The contract is source-level only, and a breaking change bumps `LE_EXTENSION_API_VERSION`. The `static_assert` above makes the break show up as a clear compile-time message rather than confusing template errors.
- Customers link their own third-party dependencies through `LINK`. Those dependencies do not leak into core targets.

---

## 6. TCL commands

*Built.* Extensions follow the three-layer pattern the core uses, so their commands look and behave exactly like built-ins.

1. **C++** (`TCL_SOURCES`, linked into the `le_tcl` module only): plain functions that get the session from `le::ext::tcl_session()` (`le/extension_tcl.hpp`) and call into the extension's core library.
2. **SWIG** (`TCL_SWIG`): CMake generates `generated/extensions/le_api_extensions.i`, with one `%include` per extension `.i` file, and `le_api.i` includes it after its own generated surface. The wrappers compile into the one `le_tcl` module, so there's no second module or second session handle.
3. **Procs** (`tcl_procs` in the manifest): listed, in dependency order, in an `extensions.json` index. The build tree's copy points at the extensions' source directories. The installed bundle's copy points at `ext/<name>/` beside `le_shell`, where `cmake --install` copies each extension's procs and resources. `app_init` reads the index (`-extensions` flag, `LE_EXTENSIONS_PATH`, else beside the executable, else the build tree's) and checks its layout_engine version and extension API. Every compiled extension must also be both in the binary and in the index. It then sources each extension's procs after `le_tcl_procs.tcl`, then the optional `startup` script. `::le_extensions_index` records which index was used. Extension procs call `register_command_help` like core procs do, so the following all include extension commands automatically:
   - `help`
   - `man`
   - tab completion
   - `generate_command_docs` / `TCL_COMMANDS.md`

**Script extensions** need only the manifest and their procs. They appear in the index with tier `script`, and nothing is compiled.

**Tests:** a manifest's `tcl_tests` scripts each run as a ctest through the build tree's `le_shell`, with every configured extension loaded. A script fails by raising an error.

**Conventions:**
- Commands use the extension prefix (`acme_route_nets`). Optionally they can also live in a Tcl namespace, exported to global.
- Overriding a core proc is technically possible ("last definition wins"), but it should be documented as unsupported.

**Escape hatch:** `le_add_extension(... TCL_INIT)` declares that the extension's Tcl sources define `le_ext_<name>_init_tcl(Tcl_Interp *)`. A generated `le::ext::init_tcl()` calls each one when Tcl loads the module, for raw `Tcl_CreateObjCommand` use such as commands taking callbacks or streams. This is a convention rather than a `Registry` hook, because an extension's core library has no Tcl dependency.

---

## 7. GUI windows

### Windows

```cpp
struct GuiWindow {
    const char *title;
    Dock dock;                                   // Left / Right / Bottom / Center(tab with Layout)
    void (*draw)(ExtGuiContext &);
    bool open_by_default;
};
```

- In `le_gui.cpp`'s frame loop, after the built-in panels, loop over `registry.windows()` and call `Begin(title, &open)` / `draw` / `End`.
- The default-layout `DockBuilder` code (`le_gui.cpp:797-821`) docks each extension window into the slot it asked for.
- The window's open/closed state is persisted through the existing `ImGuiSettingsHandler` mechanism (`le_gui.cpp:627`).
- **Recommended cleanup:** convert the built-in panels (Browser, Properties, Layers, Settings, Info) onto the same `GuiWindow` list. That makes the list the one code path for all panels, rather than a side channel for extensions.

### Menu bar

There is no menu bar today. Add one, with:
- **Window**: toggles every registered panel, core and extension alike. This is useful on its own, since panels currently can't be closed at all.
- **Extensions**: items registered through `add_menu_item`.

### `ExtGuiContext`

`ExtGuiContext` is a narrow facade built on `GuiProvider`. It never exposes `LeHandle`:
- `state()`: the per-frame snapshot, covering mode, selection count, layers and settings.
- `selected_objects()`
- `run_tcl_command(std::string)`: **the recommended way to mutate state from the GUI.** Commands then appear in console history, in undo, and in any journaling, which matches how the core GUI already works.
- `read()`: a guarded read-only view of `Root`. It returns empty while `state().is_rendering`, following `GuiProvider`'s existing locking rule.
- `extension_data<T>()`

### Smaller hooks

- **Toolbar buttons** in the mode toolbar, reusing `draw_tool_button` (`src/gui/components/mode_toolbar.cpp:69`).
- **Key bindings.** Extra entries are appended to the key-mapping table and dispatched to a registry callback, or to a TCL command string, when the Layout view is hovered.
- **Fonts and icons.** A hook runs during font atlas construction, before `ImGui_ImplOpenGL3_Init` (`le_gui.cpp:903-959`). Extension font files are resources, found under `ext/<name>/` with `find_resource`.
- **Settings.** An `extensions.<name>` object in `settings.json`. `settings_to_json` and its loader call the registered save/load callbacks, and the Settings panel draws a collapsible section per extension. Each section carries its own `version` and migrates itself, the way `kSettingsVersion` works for core keys. Because unknown keys are already preserved, a project that drops an extension doesn't lose its settings.

---

## 8. Drawing, selecting and inspecting extension objects

The goal: an extension adds a `Klass` that owns geometry (e.g. `AcmeRouteGuide` with a `shapes` list), and its objects:

1. render in the Layout/Abstract view,
2. on purposes of the extension's own (per physical layer or as a pseudo-row),
3. can be clicked and drag-selected,
4. show their fields in the property viewer,

with **no hand edit to core layout_engine code**.

### 8.1 What blocks this today

Every one of the four is a closed, hand-maintained list:

| Concern | Where it's closed | Mirrors |
|---|---|---|
| Purposes | `enum class ViewLayerPurpose` (`src/pipelines/view_style.hpp:17`); `purpose_has_selectable_objects` (`:78`); `ViewLayerSet::build_for_technology` (`:177`) adds each column/pseudo-row | `kPurposeNames` (`src/gui/components/layer_manager.cpp:25`), `::purpose_names` (`src/tcl/le_tcl_procs.tcl:922`), defaults in `LeHandle::purpose_visible_`/`purpose_selectable_` |
| Rendering | `HierarchyResolverStage` (`src/pipelines/stages/hierarchy_resolver_stage.hpp`): `LayoutChunk` (`:270`, 4 fixed chunks: `DIEAREA_BLOCKAGES`, `PORTS`, `FREE_SHAPES`, `ROWS_TRACKS_GCELLS_REGIONS`) and `collect_layout_chunk` name each owner; routes/placements are tiled separately (`collect_route_tile`/`collect_placement_tile`); `collect_abstract_content` for Abstracts | `collect_dirty` (`:1136`) maps each `ChangeKlass` to the chunk it dirties |
| Selection | `for_each_layout_hit_shape`/`hit_test_abstract_*` (`src/api/hit_test.hpp`) walk named owners; `LeHandle::SelectedObject` is a fixed `std::variant` (`le_handle.hpp:405`); whole-object kinds (Row/Placement/Region) each have their own hit-test | `purpose_has_selectable_objects` |
| Properties | `LeObjectKind` (`api.hpp:1617`) and the `build_object_properties`/`le_object_parent` switches (`api.cpp:2282`) | Property *content* is already generated (`to_properties`) |

Geometry storage is the other constraint. `ShapeData` has nine typed owner ids (`terminal_port`, `obstruction`, `physical_port_segment`, `blockage`, `route`, `layout`, `abstract`, `in_abstract`, `in_layout`), and exactly one is set. An extension `Klass` owning `Shape`s would need a tenth, which the read-only rule (§4) forbids. §8.2 D item 2 replaces the nine with one owner reference that any class, core or extension, can fill.

### 8.2 Options

**A. Free shapes (works today, no core change).**
The extension mirrors its geometry into `Layout.free_shapes` on `CUSTOM_SHAPE`/`DEBUG`.
- Pros: zero work.
- Cons:
  - The extension's object has no identity on screen: a click selects a `Shape`, not an `AcmeRouteGuide`, and the property viewer shows Shape fields.
  - There are no purposes of its own.
  - The extension must keep two copies of the data in sync, including through undo.

Good for prototypes only.

**B. Compose overlay (the earlier near-term proposal).**
A registered callback draws with Blend2D after the built-in overlays.
- Pros:
  - small core change (one loop in `ComposeStage`)
  - arbitrary drawing
- Cons:
  - Redrawn every frame, so there's no render-tree caching, culling index or per-node rasterization. Fine for hundreds of objects; not for millions.
  - Not selectable, no purposes, no property viewer.

Right for transient visualisation (congestion maps, highlights), not for objects.

**C. C++ render hooks per extension.**
`Registry` gains `add_render_source({.klass, .purpose_names, .emit, .hit_test, .properties, .dirty})`. The resolver calls `emit(root, owner, ShapeSink&)` for each Layout/Abstract node, `api.cpp` calls `hit_test`, and the property viewer calls `properties`.
- Pros:
  - Full flexibility, including synthesized geometry (like Row's footprint) and custom labels.
  - No codegen work.
- Cons:
  - Resolver internals (chunks, `ChunkSources`, change-log dirtiness) become public API, and they are the most performance-sensitive and most frequently refactored code in the tree.
  - Every extension re-implements the same boilerplate (walk my objects, emit shapes, hit-test them, list properties), and it is easy to get incrementality wrong. A missed `dirty` mapping shows stale geometry after an edit.
  - It doesn't remove the closed purpose enum; that still needs the registry described under D.

**D. Codegen-driven rendering (recommended).**
The schema declares *what* renders; codegen generates the glue that is hand-written today. An extension writes only `schema_ext.py`:

```python
Klass(
    name="AcmeRouteGuide",
    fields=[
        Field(name="layout", type="Layout", parent="acme_route_guides"),
        Field(name="name",   type="str", index=True),
        Field(name="weight", type="double", is_optional=True),
        Field(name="shapes", type="Shape", is_list=True, is_child=True, owner=True),
    ],
    render=Render(
        purpose="ACME_GUIDE",      # new purpose, declared by the extension
        per_layer=True,            # a column under each Shape.layer's row; False = own pseudo-row
        selectable=True,           # click/drag selects the AcmeRouteGuide, not the Shape
        label_field="name",        # optional text label
        visible_by_default=True,
        tiled=False,               # True: spatially tiled like routes, for large counts
    ),
)
```

The extension declares the child list on its own class only; `Shape` is untouched (item 2).

What codegen generates from `render=`, replacing each closed list in 8.1:

1. **Purpose registry.** *Built:* `schema.py` declares `purposes=[Purpose(...)]` (name, label, description, default visibility/selectability, selectable-objects flag). `codegen --target render` emits `generated/pipelines/view_layer_purpose.hpp`: the enum, a `kViewLayerPurposes` table, `purpose_has_selectable_objects` and `purpose_from_label`. The C API gains `le_purpose_kind_count`/`le_purpose_name`/`le_purpose_visible_by_default`, so the Layers panel and the Tcl `::purpose_names` read labels at runtime instead of mirroring them, and `LeHandle`'s defaults are seeded from the table. Still to add for extensions: appending an extension's purposes, and a `per_layer` flag once something reads it.
2. **One polymorphic owner on `Shape`.** *Built:* the nine typed owner ids are one stored owner reference:

   ```cpp
   enum class ShapeOwnerKind : uint8_t { None, TerminalPort, Obstruction, ..., InLayout };  // generated
   struct ShapeOwner { ShapeOwnerKind kind; uint32_t index, generation; };              // 12 bytes
   ```

   In the schema, each of Shape's nine parent fields is marked `owner=True` (`Field.owner`); codegen stores them together in one `owner` member, with one kind per field (so `Layout.diearea` and `Layout.free_shapes` stay distinct). Generated code reads `shape.route()` (an invalid id unless the owner is a Route) and builds owners with `ShapeOwner::route(id)`; `Root::set_shape_owner` moves a Shape between owners' child lists. Child lists, delete cascades, undo, change-log slots, Tcl flags (`create_shape -route ...`) and `-filter` hops (`.route.name`) are unchanged, keyed by the field names as before.
   - **Memory** (`native_format_profile`, `aes_scaling_8x8`, 2.87M Shapes): a Shape slot shrank from 424 to 360 bytes, and RSS after the DEF read from 4762 MB to 4579 MB (-3.8%). See `docs/BENCHMARKS.md`.
   - **Invariant.** "At most one owner" is true by construction.
   - **File format.** The owner is stored as the owner field's **name** plus the parent's row (NATIVE_FILE_FORMAT_RESEARCH.md §3), so it doesn't depend on which extensions are built in.
   - **Migration.** None: the schema was re-baselined to 0.50.0 (NATIVE_FILE_FORMAT_RESEARCH.md §4.1).
   - **Everything Shape-based works for extension geometry:** rendering, hit-testing, `ShapePiece` selection, the `shape_*` commands, Resize handles and path/via snapping. Classes that keep geometry as embedded `Rect`s (like `Region.rects`) are supported too, with whole-object selection.
   - **Still to do for extensions:** an extension can't add an `owner=True` field to `Shape` (the read-only rule), so extension schemas (#74) need codegen to synthesize one from an owner-side declaration, e.g. `Field(name="shapes", type="Shape", is_list=True, is_child=True, owner=True)` on `AcmeRouteGuide`. The stored value is still just a kind name in Shape's column, so this stays within the rule.
3. **Render tree.** A generated `renderable_classes.inc` lists, per renderable class: how to enumerate a Layout's/Abstract's instances (the class's nearest Layout/Abstract ancestor is known from the parent graph, the same climb `collect_dirty` does by hand), its purpose and its label field. `HierarchyResolverStage` gets one generic chunk per renderable class after its fixed chunks, or tiles for `tiled=True`. The core is written once, against the generated list. `collect_dirty` maps the class's `ChangeKlass` to that chunk through the same table, so incremental updates work without extension code.
4. **Selection.** `for_each_layout_hit_shape`/`hit_test_abstract_*` iterate the generated list as well as the core owners. `SelectedObject` stays `ShapePiece | ...`: a piece of an extension-owned Shape is selectable exactly as a core one is. With `selectable=True` the property viewer resolves the piece to its owning object through `Shape.owner`, which replaces `le_object_parent`'s per-owner switch.
5. **Properties.** *Built:* `LeObjectKind` is generated (one kind per TCL-readable class, `generated/api/object_kinds.inc`), and so are `build_object_properties`, the `le_object_parent` dispatch (from each class's parent fields) and a kind-name table behind `le_object_kind_name`/`le_object_kind_is_named` (`generated/api/object_dispatch.inc`), so an extension class appears in the property viewer automatically. Still hand-written: `le_select_object_ref` and the GUI's child listing, which encode which objects are selectable and how children are grouped; they move to the generated tables with `render=` (item 4).

- Pros:
  - No core hand edits per extension, matching the "no core change" requirement.
  - Incremental updates, culling, per-node rasterization, visibility/selectability toggles, Tcl `set_purpose_*` commands and the Layers panel all work for extension objects for free, because they ride the same path as routes.
  - Removes three hand-maintained mirrors in core today, which pays for part of the refactor on its own.
  - Declarative, so it is reviewable and versioned with the schema; `codegen` validates it (purpose name collisions, `per_layer` objects whose shapes have no `layer`).
- Cons:
  - A one-time core refactor: purposes, `LeObjectKind` and the resolver's chunk list move to generated tables, and `Shape` gets its polymorphic owner. It touches the render hot path, so it needs before/after numbers from `resolver_profile` and `pipeline_benchmarks`.
  - Only stored geometry is supported. Synthesized geometry (Row-style footprints, track grids) still needs a C++ hook, as in C.
  - Purpose ordinals depend on which extensions are built in. Anything persisted must key purposes by name. Settings already do: `layer_colors` is keyed by row name, and purpose visibility isn't saved. The `LeHandle` visibility maps would need the same if they're ever saved. The C API already passes ordinals only within one process, so this is just a rule to follow.
  - Codegen learns a rendering concept (`Render`), so the `database` target is no longer purely about storage. The generated tables can live in a separate `render` codegen target to keep that boundary.

**E. D plus a C++ escape hatch (the recommendation in full).**
Implement D for the declarative case. Add C's `add_render_source` hook only when a real extension needs synthesized geometry, restricted to `emit` plus a `dirty_on` list of `ChangeKlass` values; hit-testing and properties still come from D's tables. Until that need exists, B covers transient drawing.

### 8.3 Comparison

| | A free shapes | B overlay | C C++ hooks | D codegen |
|---|---|---|---|---|
| Core change per extension | none | none | none | none |
| One-time core change | none | small | medium | large |
| Extension code to write | sync logic | draw callback | emit + hit-test + properties + dirty | schema only |
| Own purposes / Layers panel | no | no | needs D's registry | yes |
| Selectable as its own object | no | no | yes | yes |
| Property viewer | Shape only | no | yes | yes |
| Scales to millions | yes | no | if done right | yes (tiled) |
| Incremental after edits | yes | n/a | extension's job | yes |
| Synthesized geometry | no | yes | yes | no (needs E) |

### 8.4 Suggested order

1. Generated purpose registry plus `le_purpose_name`. Removes the hand mirrors and is independently useful.
2. Generated `LeObjectKind`/property and parent dispatch.
3. Polymorphic `Shape.owner` (item 2). *Done.*
4. Generic renderable-class chunks and hit-testing (`render=` without `tiled`), benchmarked against `aes_scaling_*`.
5. `tiled=True`.
6. The C++ `emit` hook, when an extension needs it.

Steps 1–3 are refactors of existing core behaviour, with no extension API yet, so they can land and be benchmarked before anything is promised to customers. Step 3 must land before the first release, while it can still re-baseline instead of needing a data migration.

---

## 9. Upgrade and stability story

- **Customers:** `le update layout_engine` (or bump the submodule), then rebuild.
  - The resolver refuses first if an installed extension's `[compatibility]` range excludes the new version, naming the extension. This catches most breaks before anything is compiled.
  - Breaking SDK changes fail at `static_assert(LE_EXTENSION_API_VERSION == N)`, and the changelog entry for N+1 says what to change.
  - Core schema changes are checked by the merged migration replay. It either passes, names the `schema_ext.py` lines to update, or says an extension migration is needed (see "Extension schema migrations" in §4).
  - The extension's golden files then prove existing user files still load.
- **layout_engine:**
  - `examples/extensions/hello_ext` exercises all four points: one schema class, one C++ function, one TCL command, and one window plus overlay.
  - It also carries at least one extension migration and golden files, so a core migration that mishandles extension references fails layout_engine's CI, not a customer's.
  - CI configures with `-DLE_EXTENSION_DIRS=examples/extensions/hello_ext` and runs its tests, so an accidental SDK break is caught before release. A second, script-only `examples/extensions/hello_script` is installed into the release bundle by the smoke test, covering the no-compiler path.
  - The same two examples are the package manager's test fixtures.
- **Surface area discipline:** anything reachable through `le/extension.hpp`, `Registry`, `ExtGuiContext`, the codegen `extend()` hook, the TCL helper procs (`register_command_help`) and the `le_extension.toml` schema is public API. Everything else is not.
- **Version identity.** Compatibility ranges need layout_engine releases with meaningful versions. *Built (#67):* releases are signed `vX.Y.Z` tags that must match `project(... VERSION)`, starting at 0.2.0 (`docs/RELEASING.md`).

---

## 10. Phased rollout

Each phase is independently useful:

Phase 0 is groundwork that the package manager also needs. Package-manager phases are in [PACKAGE_MANAGER_RESEARCH.md](PACKAGE_MANAGER_RESEARCH.md) §11, and slot in after phase 2.

| Phase | Work | Unlocks |
|---|---|---|
| 0 | CMake runs codegen into the build dir; tagged, versioned releases; `le_extension.toml` schema and its CMake reader | Reproducible builds from a clean checkout |
| 1 ✅ | `LE_EXTENSION_DIRS`, `le_add_extension()`, dependency ordering, `Registry`, generated `register_all()`, `LeHandle` extension-data slots, `le::extension_sdk` | Customer C++ modules |
| 2 ✅ | Generated `le_api_extensions.i`, the `extensions.json` index read by `app_init`, `TCL_INIT` hooks, `tcl_tests` | Customer TCL commands; script extensions in a prebuilt release |
| 3 | Menu bar with Window/Extensions menus, `GuiWindow` list (core panels migrated), `ExtGuiContext` | Customer GUI windows |
| 4 | Codegen `--extension`, prefix and collision validation | Customer schema objects |
| 5 | Compose overlays, toolbar/key/font hooks, settings sections | Richer GUI integration |
| 6 | `hello_ext` and `hello_script` examples and CI jobs, extension-SDK changelog | Upgrade safety |
| 7 | Generated purpose registry, `LeObjectKind` dispatch, polymorphic `Shape.owner` (§8.4 steps 1-3) | Removes core's hand mirrors; smaller Shapes; prerequisite for extension rendering |
| 8 | `render=` declarations: renderable-class chunks, hit-testing, then `tiled=True` (§8.4 steps 4-5) | Extension objects drawn, selectable and inspectable |
| later | C++ `emit` hook for synthesized geometry; extension migration chains (NATIVE_FILE_FORMAT_RESEARCH.md §10, phase 5) | Custom geometry; upgrade-safe extension data |

---

## 11. Decisions

1. **Extension migrations.** Designed in NATIVE_FILE_FORMAT_RESEARCH.md §4.8; extension migrations may write only their own classes. What remains is implementation (native-format phase 5).
2. **Core-class fields.** Extensions may not add fields to core classes or other extensions' classes; codegen enforces it (§4).
3. **Multiple vendors.** Extensions from different vendors coexist in one project, so the prefix rule is mandatory, names are unique per project, and `[dependencies]` ordering is built in (§2).
4. **Binary distribution.** Not supported: extensions ship as source.
5. **Purpose identity.** Purpose ordinals depend on the set of extensions built in, so anything persisted keys purposes by name (§8.2 D).
6. **A C-API-only compiled tier.** Rejected, as too complicated for its benefit. Extensions are either script or compiled.

No questions remain open.
