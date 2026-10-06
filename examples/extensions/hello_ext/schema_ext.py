"""hello_ext's database classes, merged into Layout Engine's schema at build time."""

from codegen.schema import Field, Klass

# The schema's own version: bump it with any change below, and add a
# migration (codegen --target makemigration --extension ...).
VERSION = "0.1.0"


def extend(schema):
    schema.classes.append(
        Klass(
            name="HelloNote",
            description="A note attached to a library, from the hello_ext example extension",
            fields=[
                Field(
                    name="library",
                    description="The library the note is attached to",
                    type="Library",
                    parent="hello_notes",
                ),
                Field(
                    name="text",
                    description="The note's text",
                    type="str",
                    example="remember to route the clock first",
                ),
            ],
        )
    )
