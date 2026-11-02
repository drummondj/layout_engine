"""Migration 0.54.0 -> 0.55.0: Design.base_design links a parameter variant to its module definition"""

from codegen.migration import *

migration = Migration(
    from_version='0.54.0',
    to_version='0.55.0',
    description='Design.base_design links a parameter variant to its module definition',
    ops=[
        AddField('Design', {'name': 'base_design', 'kind': 'ref', 'type': 'Design', 'optional': True}),
    ],
)
