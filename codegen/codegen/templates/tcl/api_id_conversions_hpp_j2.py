TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). Converts between the C API's id structs
// (api.hpp) and the database's ids (database.hpp): the same {index,
// generation}, so an invalid id stays invalid. Public through
// <le/extension.hpp>; api.cpp uses the same functions.
#pragma once

#include "api.hpp"
#include "database.hpp"

namespace le::ext
{
{% for klass in classes %}
    inline Le{{klass.name}}Id to_c({{klass.name}}Id id) { return Le{{klass.name}}Id{.index = id.index, .generation = id.generation}; }
    inline {{klass.name}}Id from_c(Le{{klass.name}}Id id) { return {{klass.name}}Id{.index = id.index, .generation = id.generation}; }
{% endfor %}
}
"""
