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
from typing import List, Optional

import jinja2

from codegen.schema import Klass, Schema, to_snake_case
from codegen.templates.render import renderable_classes_hpp_j2, view_layer_purpose_hpp_j2

_NAME = re.compile(r"^[A-Z][A-Z0-9_]*$")
# le::FillPattern's enumerators (pipelines/render_style.hpp).
FILL_PATTERNS = ("NONE", "DIAGONAL_STRIPES_NE", "DIAGONAL_STRIPES_NW", "CROSS", "BRICK", "DOTS")
# The alpha a fill derived from outline_color gets, as the palette's renderable rows.
_DERIVED_FILL_ALPHA = 80
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


def _color(klass: Klass, attribute: str, errors: List[str]) -> Optional[tuple]:
    """render=`attribute` as (r, g, b, a), or None if unset or invalid (with an error)."""
    value = getattr(klass.render, attribute)
    if value is None:
        return None
    if (
        not isinstance(value, (tuple, list))
        or len(value) not in (3, 4)
        or not all(isinstance(c, int) and not isinstance(c, bool) and 0 <= c <= 255 for c in value)
    ):
        errors.append(f"{klass.name}'s render= {attribute} {value!r} isn't (r, g, b) or (r, g, b, a) with each 0-255")
        return None
    return tuple(value) if len(value) == 4 else (*value, 255)


def _style(klass: Klass, errors: List[str]) -> dict:
    """The default style render= declares: outline/fill as (r, g, b, a) or None, and the fill pattern or None."""
    outline = _color(klass, "outline_color", errors)
    fill = _color(klass, "fill_color", errors)
    if outline is None and fill is not None:
        outline = (*fill[:3], 255)
    elif fill is None and outline is not None:
        fill = (*outline[:3], _DERIVED_FILL_ALPHA)
    pattern = klass.render.fill_pattern
    if pattern is not None and pattern not in FILL_PATTERNS:
        errors.append(f"{klass.name}'s render= fill_pattern {pattern!r} isn't one of {', '.join(FILL_PATTERNS)}")
        pattern = None
    return {"outline_color": outline, "fill_color": fill, "fill_pattern": pattern}


def renderables(schema: Schema) -> tuple[list, List[str]]:
    """
    What renderable_classes.hpp needs for each class with render=, and every
    problem: it needs a Layout parent and a list of Shapes it owns, a
    label_field must name one of its str fields, and colors and the fill
    pattern must be valid.
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
        style = _style(klass, errors)
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
                **style,
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
