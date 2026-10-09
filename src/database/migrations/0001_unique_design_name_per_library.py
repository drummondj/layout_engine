"""Migration 0.50.0 -> 0.51.0: unique design name per library"""

from codegen.migration import *

migration = Migration(
    from_version='0.50.0',
    to_version='0.51.0',
    description='Design.name is unique within its Library; Library, Design, Layer, Site, Via, ViaRule and NonDefaultRule names are unique',
    ops=[
        AlterField('Design', {'name': 'name', 'kind': 'scalar', 'type': 'str', 'index': True, 'unique_per_parent': True}),
    ],
)
