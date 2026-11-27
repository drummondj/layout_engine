"""Migration 0.55.0 -> 0.56.0: a SignalUse enum for Terminal, PhysicalPort and Route use, and on schematic Port and Net"""

from codegen.migration import *

migration = Migration(
    from_version='0.55.0',
    to_version='0.56.0',
    description='a SignalUse enum for Terminal, PhysicalPort and Route use, and on schematic Port and Net',
    ops=[
        AddClass({'name': 'SignalUse',
         'kind': 'enum',
         'values': [{'name': 'SIGNAL', 'value': 0},
                    {'name': 'POWER', 'value': 1},
                    {'name': 'GROUND', 'value': 2},
                    {'name': 'CLOCK', 'value': 3},
                    {'name': 'ANALOG', 'value': 4},
                    {'name': 'TIEOFF', 'value': 5},
                    {'name': 'SCAN', 'value': 6},
                    {'name': 'RESET', 'value': 7}]}),
        AddField('Port', {'name': 'use', 'kind': 'enum', 'type': 'SignalUse', 'optional': True, 'presence': True}),
        AddField('Net', {'name': 'use', 'kind': 'enum', 'type': 'SignalUse', 'optional': True, 'presence': True}),
        AlterField('Terminal', {'name': 'use', 'kind': 'enum', 'type': 'SignalUse', 'optional': True, 'presence': True}),
        AlterField('PhysicalPort', {'name': 'use', 'kind': 'enum', 'type': 'SignalUse', 'optional': True, 'presence': True}),
        AlterField('Route', {'name': 'use', 'kind': 'enum', 'type': 'SignalUse', 'optional': True, 'presence': True}),
    ],
)
