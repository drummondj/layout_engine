TEMPLATE = """#pragma once
// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once from api.cpp, alongside the
// other generated_tcl/*.inc includes (UPDATES.md item 21) - a real
// standalone header (not a .inc fragment spliced into an existing scope
// like every other generated_tcl/ file), since these free functions need
// to be callable from anywhere that already has a <Klass>Data value in
// hand - not just the generic create_api_body()/update_api_body()
// recording hook these back, but also editing::MoveCommand's own commit
// path (api.cpp's move_click_unlocked).
//
// One apply_<snake>_snapshot(Root&, <Klass>Id, const <Klass>Data&) per
// class - see Klass.apply_snapshot_body() (schema.py) for how the body
// is built. Reduces a *whole* field snapshot to a single real
// Root::update_<klass>() call, so it stays consistent with every index/
// uniqueness invariant update_<klass>() itself already maintains - no
// raw-pointer-overwrite shortcut that could silently corrupt a
// unique_per_parent/parent-child index.
#include "../../database/database.hpp"

namespace le
{
{% for klass in classes %}
    inline bool apply_{{klass.to_snake_case()}}_snapshot(Root &root, {{klass.name}}Id id, const {{klass.name}}Data &data)
    {
{{klass.apply_snapshot_body()}}
    }
{% endfor %}
}
"""
