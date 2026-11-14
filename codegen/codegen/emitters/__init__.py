"""C++ code emitters over the schema model (codegen.schema's Klass/Field).

Each module emits one target's code; the templates call them through the
`emit` namespace. The model holds only the schema and its derived queries."""

from codegen.emitters import c_api, edit_ops, root, tcl_procs
