"""hello_ext's database classes, merged into Layout Engine's schema at build time."""

from codegen.schema import Field, Klass, Purpose, Render

# The schema's own version: bump it with any change below, and add a
# migration (`le makemigration hello_ext --name what_changed`).
VERSION = "0.4.0"


def extend(schema):
    schema.classes.append(
        Klass(
            name="HelloPin",
            description="A pin in a layout, drawn by its shapes, from the hello_ext example extension - there can be many",
            # Many per layout, so tiled: an edit redraws only the pins near it.
            render=Render(
                purpose=Purpose(
                    name="HELLO_PIN",
                    label="helloPin",
                    description="hello_ext's pins",
                    has_selectable_objects=True,
                ),
                tiled=True,
            ),
            fields=[
                Field(
                    name="layout",
                    description="The layout the pin is in",
                    type="Layout",
                    parent="hello_pins",
                ),
                Field(
                    name="shapes",
                    description="The pin's geometry",
                    type="Shape",
                    is_list=True,
                    is_child=True,
                    owner=True,
                ),
            ],
        )
    )
    schema.classes.append(
        Klass(
            name="HelloMarker",
            description="A named marker in a layout, drawn by its shapes, from the hello_ext example extension",
            # Drawn in the Layout view on its own row, and selectable.
            render=Render(
                purpose=Purpose(
                    name="HELLO_MARKER",
                    label="helloMarker",
                    description="hello_ext's markers",
                    has_selectable_objects=True,
                )
            ),
            fields=[
                Field(
                    name="layout",
                    description="The layout the marker is in",
                    type="Layout",
                    parent="hello_markers",
                ),
                Field(
                    name="name",
                    description="The marker's name",
                    type="str",
                    example="clock_root",
                ),
                Field(
                    name="shapes",
                    description="The marker's geometry",
                    type="Shape",
                    is_list=True,
                    is_child=True,
                    owner=True,
                ),
            ],
        )
    )
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
                    name="body",
                    description="The note's text",
                    type="str",
                    example="remember to route the clock first",
                ),
            ],
        )
    )
