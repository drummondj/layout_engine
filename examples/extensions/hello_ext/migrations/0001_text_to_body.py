"""Migration 0.1.0 -> 0.2.0: HelloNote.text renamed to body"""

from codegen.migration import *

migration = Migration(
    extension='hello_ext',
    depends_on_core='0.50.0',  # filled in by makemigration
    from_version='0.1.0',
    to_version='0.2.0',
    description='HelloNote.text renamed to body',
    ops=[
        RenameField('HelloNote', 'text', 'body'),
    ],
)
