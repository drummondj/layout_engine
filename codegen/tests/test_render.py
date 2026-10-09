import logging
import tempfile
import unittest
from pathlib import Path

from codegen.render_generator import generate, validate_purposes
from codegen.schema import Purpose, Schema


def _schema(purposes):
    return Schema(name="s", description="", namespace="le", version="1.0.0", classes=[], purposes=purposes)


class TestRender(unittest.TestCase):
    def test_valid_purposes_pass(self):
        schema = _schema([Purpose("ROUTE", "route", "Routed wires"), Purpose("ROW", "row", "Rows", visible_by_default=False)])
        self.assertEqual(validate_purposes(schema), [])

    def test_problems_are_reported(self):
        schema = _schema(
            [
                Purpose("route", "route", "lower-case name"),
                Purpose("ROW", "Row", "capitalised label"),
                Purpose("ROW", "rowAgain", "duplicate name"),
                Purpose("DEBUG", "route", "duplicate label"),
                Purpose("EMPTY", "empty", " "),
            ]
        )
        errors = "\n".join(validate_purposes(schema))
        self.assertIn("UPPER_SNAKE_CASE", errors)
        self.assertIn("camelCase", errors)
        self.assertIn("declared twice", errors)
        self.assertIn("used twice", errors)
        self.assertIn("one non-empty line", errors)
        self.assertEqual(validate_purposes(_schema([])), ["the schema declares no purposes"])

    def test_generates_enum_and_table_in_declaration_order(self):
        schema = _schema(
            [
                Purpose("ROUTE", "route", "Routed wires", has_selectable_objects=True),
                Purpose("GRID", "grid", "A grid", visible_by_default=False, selectable_by_default=False, under_placements=True),
            ]
        )
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(schema, out, logging.getLogger("test")), 0)
            text = (Path(out) / "view_layer_purpose.hpp").read_text()
        self.assertLess(text.index("ROUTE, // Routed wires"), text.index("GRID, // A grid"))
        self.assertIn("kViewLayerPurposeCount = 2;", text)
        self.assertIn('{ViewLayerPurpose::ROUTE, "route", true, true, true, false},', text)
        self.assertIn('{ViewLayerPurpose::GRID, "grid", false, false, false, true},', text)

    def test_invalid_purposes_fail_generation(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(_schema([]), out, logging.getLogger("test")), 1)


if __name__ == "__main__":
    unittest.main()


class TestRenderableClasses(unittest.TestCase):
    """render= on a class: renderable_classes.hpp, and what it requires."""

    def schema(self, marker_fields, render=True):
        from codegen.schema import Field, Klass, Render

        owns_shapes = any(f.name == "shapes" for f in marker_fields)
        shape_owner = [Field(name="marker", description="Owner", type="Marker", parent="shapes", owner=True)] if owns_shapes else []
        return Schema(
            name="s",
            description="",
            namespace="le",
            version="1.0.0",
            purposes=[Purpose("ROUTE", "route", "Routed wires")],
            classes=[
                Klass(name="Top", description="Root", fields=[Field(name="layouts", description="x", type="Layout", is_list=True, is_child=True)]),
                Klass(
                    name="Layout",
                    description="A layout",
                    fields=[
                        Field(name="root", description="x", type="Top", parent="layouts"),
                        Field(name="markers", description="x", type="Marker", is_list=True, is_child=True),
                    ],
                ),
                Klass(
                    name="Marker",
                    description="A marker",
                    render=Render(Purpose("MARKER", "marker", "Markers", has_selectable_objects=True)) if render else None,
                    fields=marker_fields,
                ),
                Klass(name="Shape", description="A shape", fields=shape_owner + [Field(name="size", description="x", type="int", example=1)]),
            ],
        )

    def fields(self, layout=True, shapes=True):
        from codegen.schema import Field

        fields = [Field(name="layout", description="x", type="Layout", parent="markers")] if layout else []
        if shapes:
            fields.append(Field(name="shapes", description="x", type="Shape", is_list=True, is_child=True))
        return fields

    def test_renderable_classes_hpp_describes_each_class(self):
        schema = self.schema(self.fields())
        schema.purposes.append(schema.get_klass("Marker").render.purpose)
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(schema, out, logging.getLogger("test")), 0)
            text = (Path(out) / "renderable_classes.hpp").read_text()
        self.assertIn("struct MarkerRender", text)
        self.assertIn("static constexpr ViewLayerPurpose purpose = ViewLayerPurpose::MARKER;", text)
        self.assertIn('static constexpr std::string_view owner_option = "marker";', text)
        self.assertIn("return root.get_layout_markers(layout);", text)
        self.assertIn("return root.get_marker_shapes(id);", text)
        self.assertIn("return object ? object->layout : LayoutId{};", text)
        self.assertIn("inline constexpr std::size_t kCount = 1;", text)
        self.assertIn("static constexpr bool tiled = false;", text)
        self.assertIn("static constexpr bool per_layer = false;", text)
        self.assertIn("inline constexpr std::size_t kPerLayerCount = 0;", text)

    def test_tiled_is_passed_through(self):
        schema = self.schema(self.fields())
        schema.get_klass("Marker").render.tiled = True
        schema.purposes.append(schema.get_klass("Marker").render.purpose)
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(schema, out, logging.getLogger("test")), 0)
            self.assertIn("static constexpr bool tiled = true;", (Path(out) / "renderable_classes.hpp").read_text())

    def test_per_layer_is_passed_through_and_counted(self):
        schema = self.schema(self.fields())
        self.assertFalse(schema.get_klass("Marker").render.per_layer)
        schema.get_klass("Marker").render.per_layer = True
        schema.purposes.append(schema.get_klass("Marker").render.purpose)
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(schema, out, logging.getLogger("test")), 0)
            text = (Path(out) / "renderable_classes.hpp").read_text()
        self.assertIn("static constexpr bool per_layer = true;", text)
        self.assertIn("inline constexpr std::size_t kPerLayerCount = 1;", text)

    def test_a_renderable_class_needs_a_layout_parent_and_owned_shapes(self):
        from codegen.render_generator import renderables

        schema = self.schema(self.fields(layout=False))
        schema.link()
        self.assertTrue(any("no parent field of type Layout" in e for e in renderables(schema)[1]))
        schema = self.schema(self.fields(shapes=False))
        schema.link()
        self.assertTrue(any("owns no Shapes" in e for e in renderables(schema)[1]))

    def test_without_render_nothing_is_renderable(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(generate(self.schema(self.fields(), render=False), out, logging.getLogger("test")), 0)
            self.assertIn("kCount = 0;", (Path(out) / "renderable_classes.hpp").read_text())
