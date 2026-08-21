TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once from api.hpp, immediately after
// `typedef struct LeHandle LeHandle;` - before anything else in the file,
// hand-written or generated, that names one of these types. A separate
// file (not part of declarations.inc, which comes much later in api.hpp)
// purely because of this ordering requirement - every one of these
// typedefs, for every TCL-readable class, now lives here uniformly (no
// more hand-written duplicates for the classes that also have CRUD -
// Library/Design/Abstract/Terminal/TerminalPort/Obstruction/Shape).
{% for klass in classes %}
/// @brief Mirrors the database's {{klass.name}}Id handle - a stable identity,
/// safe to hold onto and pass back into a later call without re-deriving
/// it. `index == UINT32_MAX` marks it invalid - never construct one by
/// hand, only copy one returned by this API.
typedef struct Le{{klass.name}}Id
{
    uint32_t index;
    uint32_t generation;
} Le{{klass.name}}Id;
{% endfor %}
"""
