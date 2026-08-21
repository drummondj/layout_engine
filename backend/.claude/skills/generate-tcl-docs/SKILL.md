---
name: generate-tcl-docs
description: Regenerate backend/TCL_COMMANDS.md, the Markdown reference for every TCL command (get_<type>/create_<type>/update_<type> plus the hand-written commands), from the ::command_help registry (UPDATES.md item 20). Use after any TCL command's usage/description/options changes, or when TCL_COMMANDS.md looks stale.
user-invocable: true
allowed-tools:
  - Bash
  - Read
---

# Regenerate the TCL command Markdown reference

`backend/TCL_COMMANDS.md` is generated output, not hand-edited -
`generate_command_docs` (`le_tcl_procs.tcl`) builds it from the
`::command_help` registry every `get_<type>`/`create_<type>`/
`update_<type>` and hand-written command registers itself into (see
`le_tcl_procs.tcl`'s own "Help system" section and
`le_tcl_procs_generated_tcl_j2.py`). Nothing here changes what commands
exist - only their documented output - so a plain rebuild is enough, no
`regen-tcl` step required unless a command's own usage/description/flags
actually changed (in which case run `regen-tcl` first, then this).

## Steps

1. **Ensure `le_tcl` is built** (see the `build-test` skill):

   ```
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
   cmake --build build --target le_tcl -j
   ```

2. **Run the driver script** via `tclsh8.6` (matching every other Tcl
   test's own bootstrap - `load` the built module, `source`
   `le_tcl_procs.tcl`). Paths below are relative to `backend/` - run from
   there, or adjust accordingly:

   ```
   /opt/homebrew/opt/tcl-tk@8/bin/tclsh8.6 \
       src/tcl/generate_docs.tcl \
       build/le_tcl.so \
       src/tcl/le_tcl_procs.tcl \
       TCL_COMMANDS.md
   ```

   `build/le_tcl.so` is the Debug tree's own module output - the same
   target `le_tcl_smoke`/`le_tcl_crud`/`le_tcl_help` already load under
   `ctest`.

3. **Commit the result.** `backend/TCL_COMMANDS.md` is a real, committed
   file (unlike `generated/`/`generated_tcl/`, which are `.gitignore`d) -
   `git diff` shows exactly what changed.
