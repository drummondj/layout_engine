"""
Generates the renderer's purpose registry (view_layer_purpose.hpp) from the
schema's `purposes` - a separate target (`codegen --target render`) from the
database and TCL codegen, since purposes are a rendering concept rather than
stored data.
"""

from logging import Logger
from pathlib import Path
import re
import shutil
from typing import List

import jinja2

from codegen.schema import Klass, Schema, to_snake_case
from codegen.templates.render import renderable_classes_hpp_j2, view_layer_purpose_hpp_j2

_NAME = re.compile(r"^[A-Z][A-Z0-9_]*$")
_LABEL = re.compile(r"^[a-z][A-Za-z0-9]*$")


def validate_purposes(schema: Schema) -> List[str]:
    """Every problem with the schema's purposes, as messages."""
    errors = []
    if not schema.purposes:
        errors.append("the schema declares no purposes")
    names, labels = set(), set()
    for purpose in schema.purposes:
        if not _NAME.match(purpose.name):
            errors.append(f"purpose {purpose.name}: name must be UPPER_SNAKE_CASE")
        if not _LABEL.match(purpose.label):
            errors.append(f"purpose {purpose.name}: label {purpose.label!r} must be camelCase")
        if "\n" in purpose.description or not purpose.description.strip():
            errors.append(f"purpose {purpose.name}: description must be one non-empty line")
        if purpose.name in names:
            errors.append(f"purpose {purpose.name} is declared twice")
        if purpose.label in labels:
            errors.append(f"purpose label {purpose.label!r} is used twice")
        names.add(purpose.name)
        labels.add(purpose.label)
    return errors


def renderables(schema: Schema) -> tuple[list, List[str]]:
    """
    What renderable_classes.hpp needs for each class with render=, and every
    problem: it needs a Layout parent and a list of Shapes it owns, and a
    label_field must name one of its str fields.
    """
    found, errors = [], []
    shape = next((k for k in schema.classes if k.name == "Shape"), None)
    for klass in schema.classes:
        if klass.render is None:
            continue
        layout_parent = next((f for f in klass.get_parent_fields() if f.type == "Layout"), None)
        shapes_list = next((f for f in klass.fields if f.is_child and f.is_list and f.type == "Shape"), None)
        owner_option = None
        if shape is not None and shapes_list is not None:
            owner_option = next((f for f in shape.get_owner_fields() if f.type == klass.name and f.parent == shapes_list.name), None)
        label_field = None
        if klass.render.label_field is not None:
            label_field = next((f for f in klass.fields if f.name == klass.render.label_field), None)
            if label_field is None or label_field.type != "str" or label_field.is_list:
                errors.append(f"{klass.name}'s render= label_field {klass.render.label_field!r} isn't a str field of {klass.name}")
                label_field = None
        if layout_parent is None:
            errors.append(f"{klass.name} has render= but no parent field of type Layout")
        if owner_option is None:
            errors.append(f"{klass.name} has render= but owns no Shapes (a Shape list with owner=True)")
        if layout_parent is None or owner_option is None:
            continue
        found.append(
            {
                "klass": klass,
                "snake": to_snake_case(klass.name),
                "purpose": klass.render.purpose,
                "layout_list": layout_parent.parent,
                "layout_field": layout_parent.name,
                "shapes_list": shapes_list.name,
                "owner_option": owner_option.name,
                "label_field": label_field,
            }
        )
    return found, errors


def generate(schema: Schema, output_dir: str, logger: Logger) -> int:
    """Write view_layer_purpose.hpp and renderable_classes.hpp into `output_dir`, recreated from scratch."""
    schema.link()
    renderable, render_errors = renderables(schema)
    errors = validate_purposes(schema) + render_errors
    if errors:
        for error in errors:
            logger.error(error)
        return 1

    logger.info(f"Generating {len(schema.purposes)} view-layer purposes")
    out = Path(output_dir)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    text = jinja2.Template(view_layer_purpose_hpp_j2.TEMPLATE, trim_blocks=True, lstrip_blocks=True).render(
        namespace=schema.namespace, purposes=schema.purposes
    )
    (out / "view_layer_purpose.hpp").write_text(text)
    (out / "renderable_classes.hpp").write_text(
        jinja2.Template(renderable_classes_hpp_j2.TEMPLATE, trim_blocks=True, lstrip_blocks=True).render(
            namespace=schema.namespace, renderables=renderable
        )
    )
    return 0
