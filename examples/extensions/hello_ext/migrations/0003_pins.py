"""Migration 0.3.0 -> 0.4.0: HelloPin added, owning Shapes"""

from codegen.migration import *

migration = Migration(
    extension='hello_ext',
    depends_on_core='0.50.0',  # filled in by makemigration
    from_version='0.3.0',
    to_version='0.4.0',
    description='HelloPin added, owning Shapes',
    ops=[
        AddClass({'name': 'HelloPin',
         'kind': 'pooled',
         'fields': [{'name': 'layout', 'kind': 'parent', 'type': 'Layout', 'parent_field': 'hello_pins'},
                    {'name': 'shapes', 'kind': 'child', 'type': 'Shape', 'list': True, 'owner': True}]}),
    ],
)
