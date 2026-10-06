"""Migration 0.2.0 -> 0.3.0: HelloMarker added, owning Shapes"""

from codegen.migration import *

migration = Migration(
    extension='hello_ext',
    depends_on_core='0.50.0',  # filled in by makemigration
    from_version='0.2.0',
    to_version='0.3.0',
    description='HelloMarker added, owning Shapes',
    ops=[
        AddClass({'name': 'HelloMarker',
         'kind': 'pooled',
         'fields': [{'name': 'layout', 'kind': 'parent', 'type': 'Layout', 'parent_field': 'hello_markers'},
                    {'name': 'name', 'kind': 'scalar', 'type': 'str'},
                    {'name': 'shapes', 'kind': 'child', 'type': 'Shape', 'list': True, 'owner': True}]}),
    ],
)
