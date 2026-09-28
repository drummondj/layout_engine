# Extension Mechanism — Architecture Research (NEW_FEATURES item 30)

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
| Packaging | **Superbuild.** The customer repo includes layout_engine as a git submodule (never edited) and lists extension directories in `LE_EXTENSION_DIRS`. |
| Build | layout_engine provides an `le_add_extension()` CMake function. Each extension builds as up to three static libraries (`_core`, `_tcl`, `_gui`) that are linked into the existing host targets. |
| Registration | CMake generates an init file that calls `le_ext_<name>_register(Registry&)` for each extension. This is explicit; nothing relies on static self-registration. |
| Schema | Extension classes are **merged into the core schema at build time**. `schema_ext.py` defines `extend(schema)`, and CMake runs codegen into the build directory. Extension objects get pools in `Root`, undo/redo, the change log and TCL CRUD commands for free. |
| C++ | A narrow `le::extension_sdk` interface target containing `le/extension.hpp` (`ExtensionContext`). The contract is the source API; ABI is not a concern because everything is compiled together. |
| TCL | The same three layers as core: shim, SWIG `.i`, and a procs `.tcl` file that calls `register_command_help`. The extension's `.i` files are `%include`d into `le_tcl`, and its procs are sourced after the core procs. |
| GUI | Extensions register their windows in the registry, and the core windows move onto the same list. Also: a new main menu bar, toolbar/key/overlay hooks, and extension-owned sections in both settings files. |
| Stability | A version check on `LE_EXTENSION_API_VERSION` (`static_assert`), plus an in-tree `examples/extensions/hello_ext` that CI builds. |

**Rejected:**
- Runtime `dlopen` plugins. There is no stable ABI for a templated, header-only database whose `Root` layout changes whenever the schema does.
- Maintaining a fork or patch set, because every upgrade becomes a merge.

---

## 1. Where things stand today

Layout Engine is a closed world today: there are no extension points anywhere. Relevant facts, with paths relative to `backend/`:

**Schema and codegen**
- `src/database/schema.py` is a Python DSL: `Schema(name="layout_engine", namespace="le", classes=[Klass(...), ...])`.
- `codegen` (`../codegen/codegen/cli.py`) accepts a single `--schema` and two targets, `database` and `tcl`. It loads the schema with `SourceFileLoader(...).load_module()` (`../codegen/codegen/generator.py:23`). Because that is plain Python, a schema file can already `import` another schema and append to it.
- Generated code (`src/database/generated/`, `src/api/generated_tcl/`, `src/tcl/generated/`) is **not** committed: it is `.gitignore`d and each developer regenerates it by hand (the `regen-database` / `regen-tcl` skills). CMake never runs codegen; `add_library(database INTERFACE)` is at `CMakeLists.txt:504`.
- Codegen produces a single `class Root` (`generated/root.hpp:223`) with one `Pool` per class, plus a closed `enum class ChangeKlass` (`root.hpp:93`). The undo/redo templates in `src/editing/command.hpp` and the TCL CRUD surface are also generated per class.
- There is **no native persistence format**. Data comes in and goes out only through LEF, DEF and Verilog.

**Build**
- The top-level `CMakeLists.txt` is flat. It has no `add_subdirectory` of project code, no install/export rules, and no `LE_EXTRA_*` options.
- `api` is a static library (`:631`) that keeps its internals `PRIVATE`.
- `le_tcl` is a SWIG-built Tcl module (`:729`) that links `api` privately.
- `le_shell` (`:1027`) is both the Tcl shell and the GUI app, and also links `api` (`:1030`). The process therefore contains **two copies of `api`**, one in the executable and one in the loaded module, and they share the `LeHandle*` passed through `set_session_handle`. This matters for where extension state may live (§3).

**TCL**
- A command passes through four layers:
  1. the C API in `src/api/api.hpp`
  2. the shim in `src/tcl/le_tcl_shim.cpp`, which reaches the handle through `session()`
  3. the SWIG interface `src/tcl/le_api.i`, which already ends with `%include "generated/le_api_generated.i"` (`:211`)
  4. the procs in `src/tcl/le_tcl_procs.tcl`, which parse flags and call `register_command_help` (`:135`)
- The `::command_help` dict drives `help`, `man`, tab completion and `TCL_COMMANDS.md`.
- `le_shell.cpp`'s `app_init` runs `load {module} le_tcl` (`:229`), then `set_session_handle`, then `Tcl_EvalFile(procs)` (`:249`).

**GUI** (`src/gui/le_gui.cpp`)
- Panels are hard-coded `Begin` / `draw_xxx(provider)` / `End` calls inside the frame loop, and their dock slots are fixed in `DockBuilder` code (`:867`+).
- There is **no menu bar**.
- The toolbars are hard-coded, and so is the key table (`kKeyMappings`, `:304`).
- `GuiProvider` (`gui_provider.hpp:32`) is the only way into `LeHandle`, and it deliberately exposes no raw handle.

**Rendering and settings**
- Overlays are a fixed list of `draw_*_overlay` calls in `ComposeStage::compute` (`src/pipelines/stages/compose_stage.hpp:134-139`).
- `ViewLayerPurpose` is a closed enum (`src/view_style/view_style.hpp:17`), mirrored by hand in `layer_manager.cpp` and `le_tcl_procs.tcl`.
- Settings are fixed JSON keys (`src/api/api.cpp:707`, `settings_to_json`).
- Window state is saved in imgui.ini, using a custom `ImGuiSettingsHandler` (`le_gui.cpp:689`). That same mechanism would work for extensions.

---

## 2. Packaging: a superbuild with extension directories

```
acme_le/                          <- customer's repo (proprietary)
  CMakeLists.txt
  layout_engine/                  <- git submodule, never modified
  ext/
    acme_router/
      le_extension.cmake
      schema_ext.py
      include/acme_router/...
      src/router.cpp              <- core C++ (no Tcl / ImGui deps)
      tcl/acme_router.i
      tcl/acme_router_shim.cpp
      tcl/acme_router_procs.tcl
      gui/acme_router_window.cpp
      tests/...
```

The customer's top-level `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(acme_le CXX)
set(LE_EXTENSION_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/ext/acme_router CACHE STRING "" FORCE)
add_subdirectory(layout_engine/backend)
```

Upgrading is `git -C layout_engine checkout vX.Y && cmake --build build`. No customer file lives inside layout_engine, and no layout_engine file is edited.

Plain configure-time use works too, for teams that would rather not use a superbuild repo:

```
cmake -S layout_engine/backend -B build -DLE_EXTENSION_DIRS="/path/ext/acme_router;/path/ext/acme_drc"
```

### `le_extension.cmake`

layout_engine includes each directory's `le_extension.cmake`, and that file calls one function:

```cmake
le_add_extension(acme_router
    PREFIX        Acme                      # required class / command prefix
    SCHEMA        schema_ext.py             # optional
    CORE_SOURCES  src/router.cpp
    CORE_INCLUDE  include
    TCL_SWIG      tcl/acme_router.i         # optional
    TCL_SOURCES   tcl/acme_router_shim.cpp
    TCL_PROCS     tcl/acme_router_procs.tcl
    GUI_SOURCES   gui/acme_router_window.cpp  # optional
    TESTS         tests/router_test.cpp       # optional, added to backend_tests or own gtest exe
    LINK          Boost::graph              # customer's own third-party deps
)
```

For each extension, `le_add_extension()`:
1. Builds `acme_router_core`, a static library linked against `le::extension_sdk`. It contains no Tcl or ImGui dependency, so it can be unit-tested in isolation.
2. Builds `acme_router_tcl` and links it into **`le_tcl`**.
3. Builds `acme_router_gui` (linked to `gui`) and links it into **`le_shell`**.
4. Links `acme_router_core` into **both** `le_tcl` and `le_shell`, the same way `api` is linked today.
5. Records the extension's name, schema path, `.i` files and procs paths in global properties. Later steps (codegen, init-file generation, the SWIG include list, the procs list) read them from there.

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
                Field(name="net",   type="Net", parent="acme_route_guides"),  # owned by a core Net
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
2. **CMake:** when any extension declares a `SCHEMA`, CMake adds a custom command that runs `codegen --target database` and `codegen --target tcl` over the core schema plus all extension schemas. The output goes to `${CMAKE_BINARY_DIR}/le_generated/`, and those include directories are placed **ahead of** the in-tree `generated/` directories. The layout_engine tree is never written to. Without extensions the build is unchanged and uses the in-tree output. Since generated code is already a local, uncommitted artifact, a natural follow-up is to have CMake run codegen in every build, which removes the manual regen step for everyone.
   - The textual `#include "generated/..."` sites in `api.cpp`, `le_tcl_shim.cpp`, `le_api.i` and `le_tcl_procs.tcl` need to find the build-dir copy first.
   - The cleanest way to do that is to make them include-path-relative, which is a one-time mechanical change.

**What extension objects get for free, because they are ordinary `Root` classes:**
- Pool storage and `AcmeRouteGuideId` handles
- Index lookups and parent/child navigation (`get_net_acme_route_guides`)
- Delete cascades: deleting a `Net` deletes its guides
- Undo/redo, transactions and change-log entries (`ChangeKlass` is regenerated to include them)
- `get_acme_route_guides`, `create_acme_route_guide`, `update_…`, `delete_…` TCL commands, with help text
- Display in the GUI's generic property viewer (via `to_properties()`)

**Adding fields to core classes.** An extension *can* do this through `schema.klass("Net").fields.append(...)`, and it works mechanically. It is also the most upgrade-fragile thing an extension can do. The recommendation is to allow it but emit a codegen warning, and to steer customers towards owning objects (`AcmeNetInfo` with `parent="…"` onto `Net`) instead.

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

Layout Engine has no native database file today; everything goes out through DEF and Verilog. Extension objects will therefore **not survive a save/load round-trip** unless:
- (a) the extension writes its own sidecar file, or
- (b) a TCL "dump" produces a script of `create_acme_*` commands, or
- (c) core grows a native format. If it does, it should be schema-driven so that extension classes are included automatically.

This is a core decision worth taking before customers depend on the mechanism. **See [NATIVE_FILE_FORMAT_RESEARCH.md](NATIVE_FILE_FORMAT_RESEARCH.md)** for a proposed option (c).

### Extension schema migrations

Under that proposal, an extension evolves its schema the same way core does. Next to `schema_ext.py`, it keeps:
- its own `schema_history/` snapshots
- its own `migrations/` chain, with its own version number, drafted by `codegen makemigration --extension acme_router`
- its own golden files

Core and extension migrations share **one timeline**:
- Each extension migration records the core version it was written against (`depends_on_core`, filled in automatically).
- When loading, it runs right after that core migration and before the next one.
- Core migrations also rewrite extension data that refers to core classes: references follow renamed classes, removed parents cascade, and fields the extension added to a core class travel with their rows.

So most core schema changes need **no extension migration**. At most the build names the `schema_ext.py` lines to update. Only a core change that removes or reshapes something the extension relies on needs a new extension migration. The full rules are in NATIVE_FILE_FORMAT_RESEARCH.md §4.8.

Add to `le_add_extension()`:

```cmake
    SCHEMA          schema_ext.py
    MIGRATIONS      migrations/       # optional until the extension's first schema change
    GOLDEN_FILES    tests/golden/     # loaded by the extension's tests through the merged plan
```

---

## 5. C++ modules: a narrow extension SDK

Add an INTERFACE target, `le::extension_sdk`, that exposes the headers under `src/extension/le/` and the generated database headers:

```cpp
namespace le::ext {
class ExtensionContext {
public:
    ReadView  read();                 // shared lock; const Root&, view_layers, selection
    WriteView write();                // HandleWriteLock; Root&, notifies render on scope exit
    Transaction begin_transaction(std::string_view label);   // groups edits into one undo step
    template <class T> T &extension_data();
    spdlog::logger &log();
};
}
```

- The rest of `api` (pipelines, LeHandle internals, io) stays `PRIVATE`. The smaller the SDK, the fewer upgrade breaks.
- Everything is compiled together, so **ABI does not matter**. The contract is source-level only, and a breaking change bumps `LE_EXTENSION_API_VERSION`. The `static_assert` above makes the break show up as a clear compile-time message rather than confusing template errors.
- Customers link their own third-party dependencies through `LINK`. Those dependencies do not leak into core targets.

---

## 6. TCL commands

Extensions follow the three-layer pattern the core uses, so their commands look and behave exactly like built-ins.

1. **Shim** (`tcl/acme_router_shim.cpp`) contains plain C++ free functions. Each gets the handle through `session()` (exported from `le_tcl_shim.hpp`) and calls into `acme_router_core`.
2. **SWIG.** CMake generates `le_api_extensions.i`, containing one `%include "/abs/path/acme_router.i"` per extension. `le_api.i` gets a single `%include "le_api_extensions.i"` next to its existing generated include (`le_api.i:211`). The extension's wrappers compile into the one `le_tcl` module, so no second module or second session handle is needed.
3. **Procs.** CMake bakes the list of extension procs files into `le_shell` (as it already does for `LE_TCL_PROCS_DEFAULT_PATH`), and `app_init` sources them **after** `le_tcl_procs.tcl`. Extension procs call `register_command_help` like core procs do, so the following all include extension commands automatically:
   - `help`
   - `man`
   - tab completion
   - `generate_command_docs` / `TCL_COMMANDS.md`

**Conventions:**
- Commands use the extension prefix (`acme_route_nets`). Optionally they can also live in a Tcl namespace, exported to global.
- Overriding a core proc is technically possible ("last definition wins"), but it should be documented as unsupported.

**Escape hatch:** a `Registry::add_tcl_init` hook receives the `Tcl_Interp*` for raw `Tcl_CreateObjCommand` use. This suits commands that don't fit SWIG well, such as those taking callbacks or streams.

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
- The default-layout `DockBuilder` code (`le_gui.cpp:833-887`) docks each extension window into the slot it asked for.
- The window's open/closed state is persisted through the existing `ImGuiSettingsHandler` mechanism (`le_gui.cpp:689`).
- **Recommended cleanup:** convert the built-in panels (Browser, Properties, Layers, Settings, Info) onto the same `GuiWindow` list. That makes the list the one code path for all panels, rather than a side channel for extensions.

### Menu bar

There is no menu bar today. Add one, with:
- **Window**: toggles every registered panel, core and extension alike. This is useful on its own, since a closed panel currently can't be re-opened.
- **Extensions**: items registered through `add_menu_item`.

### `ExtGuiContext`

`ExtGuiContext` is a narrow facade built on `GuiProvider`. It never exposes `LeHandle`:
- `state()`: the per-frame snapshot, covering mode, selection count, layers and settings.
- `selected_objects()`
- `run_tcl_command(std::string)`: **the recommended way to mutate state from the GUI.** Commands then appear in console history, in undo, and in any journaling, which matches how the core GUI already works.
- `read()`: a guarded read-only view of `Root`. It returns empty while `state().is_rendering`, following `GuiProvider`'s existing locking rule.
- `extension_data<T>()`

### Smaller hooks

- **Toolbar buttons** in the mode toolbar, reusing `draw_tool_button` (`components/mode_toolbar.cpp:70`).
- **Key bindings.** Extra entries are appended to the key-mapping table and dispatched to a registry callback, or to a TCL command string, when the Layout view is hovered.
- **Fonts and icons.** A hook runs during font atlas construction, before `ImGui_ImplOpenGL3_Init` (`le_gui.cpp:985-1029`).
- **Settings.** An `extensions.<name>` object in `settings.json`. `settings_to_json` and its loader call the registered save/load callbacks, and the Settings panel draws a collapsible section per extension.

---

## 8. Drawing extension objects in the layout view

**Near term: compose overlays.**
- `ComposeStage::compute` draws a fixed list of overlays today (`compose_stage.hpp:134-139`). After those, it would loop over `registry.overlays()`.
- Each overlay callback receives the Blend2D context, the view transform (DBU to pixel) and a `ReadView`.
- Stages recompute only when their options change, so `ViewRenderOptions` gains an `extension_overlay_version`. The extension bumps it through `ExtensionContext`.

**Later: extension layers and purposes.** Extension objects could render like core shapes, with layer colours, visibility toggles and selection. To get there:
- `ViewLayerPurpose` must stop being a closed enum. That means a table-driven purpose registry that replaces the three hand-maintained mirrors.
- `HierarchyResolverStage` needs a hook for emitting extension shapes into the render tree.

This is the largest GUI-side change and should come last.

**Available today without any work.** Extensions can create free shapes on the `CUSTOM_SHAPE` or `DEBUG` purposes through the existing `create_shape` / `shape_*` commands. That is good enough for prototypes.

---

## 9. Upgrade and stability story

- **Customers:** bump the submodule, then rebuild.
  - Breaking SDK changes fail at `static_assert(LE_EXTENSION_API_VERSION == N)`, and the changelog entry for N+1 says what to change.
  - Core schema changes are checked by the merged migration replay. It either passes, names the `schema_ext.py` lines to update, or says an extension migration is needed (see "Extension schema migrations" in §4).
  - The extension's golden files then prove existing user files still load.
- **layout_engine:**
  - `examples/extensions/hello_ext` exercises all four points: one schema class, one C++ function, one TCL command, and one window plus overlay.
  - It also carries at least one extension migration and golden files, so a core migration that mishandles extension references fails layout_engine's CI, not a customer's.
  - CI configures with `-DLE_EXTENSION_DIRS=examples/extensions/hello_ext` and runs its tests, so an accidental SDK break is caught before release.
- **Surface area discipline:** anything reachable through `le/extension.hpp`, `Registry`, `ExtGuiContext`, the codegen `extend()` hook and the TCL helper procs (`register_command_help`) is public API. Everything else is not.

---

## 10. Phased rollout

Each phase is independently useful:

| Phase | Work | Unlocks |
|---|---|---|
| 1 | `LE_EXTENSION_DIRS`, `le_add_extension()`, `Registry`, generated `register_all()`, `LeHandle` extension-data slots, `le::extension_sdk` | Customer C++ modules |
| 2 | Generated `le_api_extensions.i`, extension procs sourced in `app_init`, `add_tcl_init` | Customer TCL commands |
| 3 | Menu bar with Window/Extensions menus, `GuiWindow` list (core panels migrated), `ExtGuiContext` | Customer GUI windows |
| 4 | Codegen `--extension`, CMake-driven codegen into the build dir when extensions declare schemas, prefix and collision validation | Customer schema objects |
| 5 | Compose overlays, toolbar/key/font hooks, settings sections | Richer GUI integration |
| 6 | `hello_ext` example and CI job, extension-SDK changelog | Upgrade safety |
| later | Table-driven purposes; native, schema-driven persistence with extension migration chains (NATIVE_FILE_FORMAT_RESEARCH.md §10) | Full first-class rendering and save/load |

---

## 11. Open questions

1. **Persistence.** Adopt the native format and migration chain in NATIVE_FILE_FORMAT_RESEARCH.md, or keep extension data in sidecar files? The native format is recommended, since it gives extensions migrations for free.
2. **Core-class fields.** Should extensions be allowed to add fields to core classes, with a warning, or be restricted to owning objects of their own?
3. **Multiple vendors.** Must extensions from different vendors coexist in one build? If yes, the prefix rule becomes mandatory, and cross-extension dependencies (`DEPENDS acme_router`) need ordering in `le_add_extension`.
4. **Binary distribution.** Will a customer ever need to ship its extension *without* source to a third party? If so, a prebuilt static library plus headers still works under this design, but only against the exact layout_engine version it was built with.
