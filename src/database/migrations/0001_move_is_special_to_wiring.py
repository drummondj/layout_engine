"""Migration 0.53.0 -> 0.54.0: Route.is_special moves to its Wires and route Shapes"""

from codegen.migration import *

migration = Migration(
    from_version='0.53.0',
    to_version='0.54.0',
    description='Route.is_special moves to its Wires and route Shapes',
    ops=[
        AddField('Shape', {'name': 'is_special', 'kind': 'scalar', 'type': 'bool'}),
        AddField('Wire', {'name': 'is_special', 'kind': 'scalar', 'type': 'bool'}),
        RunCode('copy_route_is_special_to_wiring', 'copy each Route.is_special to its Wires and route Shapes'),
        RemoveField('Route', 'is_special'),
    ],
)
