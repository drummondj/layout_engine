---
name: regen-tcl
description: Regenerate the TCL/SWIG property-reading, search, create_<type>, update_<type>, and delete_<type> surface (written by the build into build/generated/api/ and build/generated/tcl/) from src/database/schema.py via the codegen fork's `tcl` target. Use whenever schema.py changes a TCL-readable class, a has_current_access flag, a create/update-field's optionality/type, or a class's tcl_child_list_fields() (delete_<type>'s own cascade shape), or when the generated TCL surface looks out of sync (missing class, stale field, stale friendly-id lookup, wrong get_<type> default scope, stale create_<type>/update_<type>/delete_<type> signature).
user-invocable: true
allowed-tools:
  - Bash
  - Read
---

# Regenerate the TCL property-reading/search surface from schema.py

`src/database/schema.py` is also the source of truth for this surface -
`Klass.tcl_readable`/`Klass.tcl_id_field`/`Klass.has_current_access`
(codegen/codegen/schema.py) control which classes get a generated TCL
property table, how their friendly id is built, and how every readable
class's `get_<type>` default scope (`-of` omitted) is derived. The build
runs this target alongside the database one whenever `schema.py` or
`codegen/` changes, into `<build>/generated/api/` and
`<build>/generated/tcl/` - never edit them; rebuild instead.

This is a **separate generation target** from `regen-database` - it
covers property *reading*, `get_<type>` *search*, `create_<type>`,
`update_<type>`, and `delete_<type>` for every TCL-readable class,
uniformly (~35 today, including Shape's own `-rects`/`-polygons`/
`-paths` flags, which take a *list* of a flattenable embedded struct, not
just one - see `Field.list_compound_kind()`). `update_<type>` is the
*only* way a field is mutated after creation - there is no per-field
setter reachable from TCL, generated or hand-written. `delete_<type>` cascades to every owned pool-backed
child reachable through `Klass.tcl_child_list_fields()`, however many
schema-graph levels deep that goes for a given class - see
`Klass.delete_api_body()`'s own docstring for the recursive-at-codegen-
time cascade mechanism and the deepest-first/self-last undo/redo
recording order it depends on; a class with no such fields (most of the
~35) gets a trivial, non-cascading delete instead. `read_lef`, session/
viewport/design-selection, the coordinate-list SWIG typemap itself
(`le_api.i`, hand-written since it needs real Tcl_Interp access - the
*generated* `%apply` lines that reuse it for each `list_compound_kind()`
field live in `le_api_generated_i_j2.py`), Shape's own
`remove_shape_rect`/`_polygon`/`_path` (removing one geometry entry by
index - not per-class flag-driven CRUD in the same sense `delete_<type>`
is), and the filter-expression evaluator itself (`filter.hpp`) all stay
hand-written - none of that is per-class flag-driven CRUD, so it doesn't
belong in a generator. See
`create_api_body()`/`update_api_body()`/`delete_api_body()`'s own
docstrings (`codegen/codegen/schema.py`) for exactly what each covers
(field scope, required-vs-optional, compound-field flattening, the
multi-parent exactly-one check, the single-parent-only reparent flag,
the delete cascade plan).

## Steps

1. **Rebuild** (see the `build-test` skill). There's no manual codegen
   step. To run the target by hand, e.g. to inspect its output:

   ```
   PYTHONPATH=codegen python3 -m codegen.cli --schema src/database/schema.py \
       --output build/generated --target tcl
   ```

   It writes two subdirectories beneath `--output`: `api/` and
   `tcl/`.

2. **Diff the output if needed.** Generated code lives in the build tree, so
   `git diff` won't show it - copy `build/generated/` aside before
   rebuilding if you need a baseline. codegen recreates both directories on
   every run, so a stale file from a since-renamed class can't survive.

3. **Run the tests** - `le_tcl_smoke`, `le_tcl_crud` and `le_tcl_shell`
   exercise this surface directly.

## Adding a new TCL-readable class

Every pool-backed class (`has_pool=True`) is TCL-readable by default
(`Klass.tcl_readable` defaults to `has_pool` - see
`codegen/codegen/schema.py`'s `Klass.is_tcl_readable()`), so a brand new
`Klass` in `schema.py` needs no extra step to show up here. To opt a
class *out*, pass `tcl_readable=False` to its `Klass(...)` call.

Its friendly id auto-derives to the field with `index=True` if one exists
(name-based, `"type:NAME"`), else a numeric packed id (`"type:N"`) - pass
`tcl_id_field="<field>"` to override which field backs the friendly id
(used for glob-based `name_expression` search). If that field is `index=True`
but also `unique_per_parent=True` (e.g. `Terminal`'s `name`, unique only
per-Abstract, not globally - see `Field.unique_per_parent`'s own docstring),
`Klass.tcl_indexed_id_field()` returns `None` for it even though
`tcl_friendly_id_field()` still does - the generator then skips the
Root-backed by-name lookup pair (`le_X_by_<field>`/`le_X_<field>_by_id`) and
the friendly-id resolve/format pair (`resolve_X_id`/`format_X_id`) for that
class, since the generated `Root::get_X_by_<field>()` for a
`unique_per_parent` field takes an extra parent-id parameter, not the plain
single-value lookup this pair assumes; a class in that situation needs its
own hand-written `resolve_X_id`/`format_X_id`/`le_X_by_<field>` (see
`Terminal`'s in `le_tcl_shim.cpp`/`api.cpp` for the pattern - current-
abstract-scoped name lookup, not a flat Root index). `create_<type>`/
`update_<type>` are unaffected either way - neither ever uses
`tcl_indexed_id_field()`/`tcl_friendly_id_field()`, only
`Klass.get_create_fields()`. Note the current-view scoping this implies
for `Terminal` specifically: `update_terminal <id> -abstract <token>`
moves the Terminal, but `<id>`'s own hand-written `resolve_terminal_id`
stays scoped to whatever Abstract is *currently selected* - a caller that
needs to address the just-reparented Terminal again by its own friendly
id (e.g. to delete it) must switch the current view to its new Abstract
first (`open_design`/`current_abstract <token>`), the same way
`get_terminals` already requires for search.

## `has_current_access` and the `get_<type>` default-scope algorithm

`Klass.has_current_access = True` (today: `Technology`, `Abstract`,
`Schematic`) marks a class with a generated "current instance" concept -
each gets independent `LeHandle` state (`current_X_id`), one TCL command
(`current_X ?id?` - reads it back with no argument, selects then returns
it when given a friendly-id token; `set_current_X_cmd` still exists one
layer down, as the shim/SWIG-level command `current_X`'s own generated
Tcl wrapper calls when an id is given, but is not itself a top-level TCL
command a script would call directly), and every *other* readable
class's `get_<type>` default scope (when every `-of` is omitted)
is *derived automatically* from where that class sits in the schema graph
relative to the nearest `has_current_access` anchor - no hand-picked
per-class table. See `codegen/codegen/tcl_scope.py`'s own module
docstring for the full 4-case algorithm (self / descendant-of-anchor,
unioning multiple `is_child` paths / ancestor-of-anchor, falling back to a
flat scan if unset / no relation, flat scan). `-of <parent>` parameters
are independent of this and always generated one-per-parent-field
(`Klass.get_parent_fields()`, matched against a *sibling* `is_child`
field - list or scalar, e.g. `Design.abstract` - on the parent class);
the default-scope case only supplies the fallback used when every `-of`
was omitted or invalid.

This generated `current_X` state is separate from the GUI's
`LeHandle::current_abstract()`. A TCL script must select an Abstract
(`current_abstract <token>`, or `open_design`, which does it for the
script) before `get_terminals`/`get_shapes`/etc.'s default scope resolves
to anything. Selecting a Design (`le_set_current_design_abstract`/
`le_set_current_design_abstract_by_id`, `api.cpp`) moves both together,
so the GUI opening a Design and a script's `open_design` mean the same
thing. A script that builds an `Abstract` from scratch and calls
`current_abstract <id>` directly, with no `Design` to select, moves only
the generated state.

## The generated-code injection points

Each of these hand-written files gains one or more `#include`/`%include`/
`source` lines pointing at generated output - added once, never touched
again on subsequent regenerations:

- `api.hpp` - **two** injection points: `#include "generated/api/ids.inc"`
  immediately after `typedef struct LeHandle LeHandle;` (every `LeXId`
  typedef - has to come before anything else in the file, hand-written or
  generated, that names one of these types), and
  `#include "generated/api/declarations.inc"` further down (friendly-id-
  by-name lookups, property-table declarations, `is_child` enumeration,
  current-instance access, `get_<type>` search declarations, and
  `le_create_<type>`/`le_update_<type>`/`le_delete_<type>` declarations).
- `api.cpp` - **five** injection points: `#include "generated/api/snapshot_appliers.hpp"`
  near the top of the file with the rest of its ordinary top-level
  `#include`s - unlike every other generated/api/
  file below, this one is a real standalone header (`#pragma once`, its
  own `namespace le { ... }`), not a `.inc` fragment spliced into a
  specific existing scope, since `apply_<snake>_snapshot(Root&, <Klass>Id,
  const <Klass>Data&)` needs to be callable from both the generic
  create/update recording hook (below) and `editing::MoveCommand`'s own
  commit path; `#include "generated/api/property_accessors_internal.inc"`
  *inside* the file's anonymous namespace (internal helpers -
  `build_X_properties`, `to_c`/`from_c` overloads - never called from
  another translation unit); `#include "generated/api/property_accessors_public.inc"`
  *inside* `extern "C" { ... }` (the real `le_X_property_count/_at/_path`,
  friendly-id-by-name lookups, `le_current_X`/`le_set_current_X`, and
  `le_create_X`/`le_update_X`/`le_delete_X` - external C linkage required
  since `le_tcl_shim.cpp` calls
  them; a function defined
  inside an anonymous namespace has internal linkage regardless of
  `extern "C"`, so putting these there produces unresolved-symbol link
  errors in `le_tcl.so` - don't merge the internal/public fragments back
  into one; `le_create_X`/`le_update_X`'s generated bodies also each
  record themselves into `handle->command_history`'s currently-recording
  transaction, if any, using `apply_<snake>_snapshot` from
  `snapshot_appliers.hpp` above; `le_delete_X`
  records itself the same way, but doesn't need `apply_<snake>_snapshot`
  at all - a delete's own undo is a plain `create_x(snapshot)` replay, not
  a field-by-field apply, see `Klass.delete_api_body()`'s own docstring,
  `codegen/codegen/schema.py`);
  `#include "generated/api/search.inc"` right after it, also
  inside `extern "C" { ... }` (`le_get_X`/`le_search_result_X_at`, same
  linkage reasoning). `filter_field_tables()`'s body is also generated -
  `= \n#include "generated/api/filter_tables.inc"` replaces its old
  hand-written initializer list (the `FilterFieldTable` struct itself and
  the functions that consume the table stay hand-written).
- `api.hpp` also includes `generated/api/object_kinds.inc` (the
  `LeObjectKind` enum, one kind per TCL-readable class) where that enum is
  documented, and `api.cpp` includes `generated/api/object_dispatch.inc`
  in its anonymous namespace after the `id_from_ref`/`ref_from_id`/
  `invalid_object_ref` helpers (`build_object_properties`,
  `object_ref_parent`, and the `kObjectKinds` name table behind
  `le_object_kind_name`/`le_object_kind_is_named`).
- `le_handle.hpp` - `#include "generated/api/handle_fields.inc"` inside
  `struct LeHandle`'s body (per-class property-table caches,
  search-result caches, `current_X_id` fields).
- `le_tcl_shim.hpp` - `#include "generated/tcl/le_tcl_shim_generated.hpp"`.
- `le_tcl_shim.cpp` - `#include "generated/tcl/le_tcl_shim_generated.inc"`,
  placed right after the file's own anonymous namespace closes (so
  `session()`/`pack`/`unpack`/`return_string`/`format_property_value`/
  `resolve_numeric_friendly_id`/`format_numeric_friendly_id` are already
  in scope).
- `le_api.i` - `%include "generated/tcl/le_api_generated.i"`.
- `le_tcl_procs.tcl` - sources `le_tcl_procs_generated.tcl`: the build
  tree's copy (its path is compiled into `le_tcl`, returned by
  `generated_procs_default_path_command`), else the one beside
  `le_tcl_procs.tcl` in a release bundle
  (`property_accessors_for_token`, `current_X`, every `get_<type>`
  proc - `parse_get_args`/`check_of_prefixes`/`default_to_unset` stay
  hand-written, shared/class-agnostic helpers the generated procs call
  into).

Never hand-write a `create_X`/`update_X`/`delete_X` or a per-field
setter for a TCL-readable class in any of the files above. A duplicate C
symbol fails the `le_tcl.so` link, and a same-named Tcl `proc` fails
silently: Tcl redefines a `proc` without error, and `le_tcl_procs.tcl`
sources `le_tcl_procs_generated.tcl` partway through the file,
so whichever definition comes later wins. `create_shape
-terminal_port|-obstruction` is one generated command (see
`create_api_body()`'s exactly-one-parent check).

A field with `create_excluded=True` (e.g. `Abstract.boundary`) gets no
create/update flag, so it can't be set from TCL; `Field.create_excluded`'s
docstring lists them. `delete_<type>` expands the schema graph through
`Klass.tcl_child_list_fields()` however deep a class's subtree goes
(e.g. `Technology` -> `non_default_rules` -> `vias` -> `layers`), so no
owned descendant is orphaned. The only hand-written CRUD is `Shape`'s
`remove_shape_rect`/`_polygon`/`_path` (removing one geometry entry by
index), which stays out of this generator.
