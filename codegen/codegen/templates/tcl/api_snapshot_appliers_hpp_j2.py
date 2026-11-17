TEMPLATE = """#pragma once
// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd from api.cpp, input.cpp and
// edit_ops.cpp - a real standalone header (not a .inc fragment spliced
// into an existing scope like most generated/api/ files), since these free
// functions need to be callable from anywhere that already has a
// <Klass>Data value in hand: the generated edit ops' undo recording, and
// editing::MoveCommand's own commit path (input.cpp's move_click_unlocked).
//
// One apply_<snake>_snapshot(Root&, <Klass>Id, const <Klass>Data&) per
// class - see edit_ops.apply_snapshot_body() (codegen/emitters/edit_ops.py)
// for how the body is built. Reduces a *whole* field snapshot to a single real
// Root::update_<klass>() call, so it stays consistent with every index/
// uniqueness invariant update_<klass>() itself already maintains - no
// raw-pointer-overwrite shortcut that could silently corrupt a
// unique_per_parent/parent-child index.
#include "database.hpp"

namespace le
{
{% for klass in classes %}
    inline bool apply_{{klass.to_snake_case()}}_snapshot(Root &root, {{klass.name}}Id id, const {{klass.name}}Data &data)
    {
{{emit.edit_ops.apply_snapshot_body(klass)}}
    }
{% endfor %}
}
"""
