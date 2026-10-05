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

from codegen.schema import Schema
from codegen.templates.render import view_layer_purpose_hpp_j2

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


def generate(schema: Schema, output_dir: str, logger: Logger) -> int:
    """Write view_layer_purpose.hpp into `output_dir`, recreated from scratch."""
    errors = validate_purposes(schema)
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
    return 0
