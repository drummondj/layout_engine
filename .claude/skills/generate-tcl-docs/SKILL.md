---
name: generate-tcl-docs
description: Regenerate docs/TCL_COMMANDS.md, the Markdown reference for every TCL command (get_<type>/create_<type>/update_<type> plus the hand-written commands), from the ::command_help registry. Use after any TCL command's usage/description/options changes, or when docs/TCL_COMMANDS.md looks stale.
user-invocable: true
allowed-tools:
  - Bash
  - Read
---

# Regenerate the TCL command Markdown reference

`docs/TCL_COMMANDS.md` is generated output, not hand-edited -
`generate_command_docs` (`le_tcl_procs.tcl`) builds it from the
`::command_help` registry every `get_<type>`/`create_<type>`/
`update_<type>` and hand-written command registers itself into (see
`le_tcl_procs.tcl`'s own "Help system" section and
`le_tcl_procs_generated_tcl_j2.py`). Nothing here changes what commands
exist - only their documented output - and a rebuild regenerates the TCL
surface anyway, so rebuilding before running this is enough.

## Steps

1. **Ensure `le_tcl` is built** (see the `build-test` skill):

   ```
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
   cmake --build build --target le_tcl -j4
   ```

2. **Run the driver script** with the `tclsh8.6` CMake configured
   (`TCL_TCLSH` in `build/CMakeCache.txt`, the same one the `le_tcl_*`
   ctest tests use). Paths below are relative to the repo root:

   ```
   TCLSH=$(sed -n 's/^TCL_TCLSH:FILEPATH=//p' build/CMakeCache.txt)
   "$TCLSH" \
       src/tcl/generate_docs.tcl \
       build/le_tcl.so \
       src/tcl/le_tcl_procs.tcl \
       docs/TCL_COMMANDS.md
   ```

   `build/le_tcl.so` is the Debug tree's own module output - the same
   target `le_tcl_smoke`/`le_tcl_crud`/`le_tcl_help` already load under
   `ctest`. That tree builds the example extensions in, but the reference
   leaves out everything they add (their commands, the owner options and
   purposes they add to core commands), so the output is the core
   reference whatever `LE_EXTENSION_DIRS` the tree was configured with.
   Commands are grouped into sections: a class's `tcl_category` in
   `schema.py`, a hand-written command's entry in `::command_categories`
   (`le_tcl_procs.tcl`). A new command needs one - `le_tcl_help` fails
   without it.

3. **Commit the result.** `docs/TCL_COMMANDS.md` is a real, committed
   file (unlike the generated code, which lives in the build tree) -
   `git diff` shows exactly what changed.
