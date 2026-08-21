"""Generates the TCL/SWIG property-reading and search surface (backend/src/
api's generated_tcl/ fragments, backend/src/tcl's generated/ files) from a
schema - a separate generation target from generator.py's database
codegen, invoked via `cmg --target tcl`.
"""

from logging import Logger
from pathlib import Path
import shutil

import jinja2

from codegen.templates.tcl import (
    api_declarations_inc_j2,
    api_filter_tables_inc_j2,
    api_handle_fields_inc_j2,
    api_ids_inc_j2,
    api_property_accessors_internal_inc_j2,
    api_property_accessors_public_inc_j2,
    api_search_inc_j2,
    api_snapshot_appliers_hpp_j2,
    le_api_generated_i_j2,
    le_tcl_procs_generated_tcl_j2,
    le_tcl_shim_generated_hpp_j2,
    le_tcl_shim_generated_inc_j2,
)
from codegen.schema import Schema
from codegen.tcl_scope import (
    compute_search_scope,
    render_default_scope_cpp,
    render_of_check_cpp,
)
from codegen.validation import SchemaRuleSet


def get_generated_classes(schema: Schema):
    """
    Every class the TCL generator owns: pool-backed and tcl_readable
    (defaults to has_pool - see Klass.is_tcl_readable()). Property-reading
    and search are generated uniformly for all of them.
    """
    return [
        klass
        for klass in schema.classes
        if not klass.is_enum and klass.has_pool and klass.is_tcl_readable()
    ]


def get_current_access_classes(schema: Schema):
    return [klass for klass in schema.classes if klass.has_current_access]


def get_search_scopes(classes, current_access_classes) -> dict:
    """Klass.name -> SearchScope, computed once and reused across every
    template that needs it (api_search, le_tcl_shim search wrappers,
    le_tcl_procs search wrappers)."""
    return {
        klass.name: compute_search_scope(klass, current_access_classes)
        for klass in classes
    }


def generate(schema: Schema, output_dir: str, logger: Logger) -> int:
    """Generate the TCL property-reading/search surface. `output_dir` is
    the backend's src/ directory - files land under api/generated_tcl/
    and tcl/generated/ beneath it."""

    logger.info("Validating schema ...")
    errors = SchemaRuleSet().validate(schema)
    if len(errors) > 0:
        for error in errors:
            logger.error(error.message)
        return 1

    schema.link()

    classes = get_generated_classes(schema)
    current_access_classes = get_current_access_classes(schema)
    search_scopes = get_search_scopes(classes, current_access_classes)
    logger.info(
        f"Generating TCL property-reading/search surface for {len(classes)} classes "
        f"({len(current_access_classes)} with current-instance access): "
        f"{', '.join(k.name for k in classes)}"
    )

    api_dir = Path(output_dir) / "api" / "generated_tcl"
    tcl_dir = Path(output_dir) / "tcl" / "generated"
    shutil.rmtree(api_dir, ignore_errors=True)
    shutil.rmtree(tcl_dir, ignore_errors=True)
    api_dir.mkdir(parents=True, exist_ok=True)
    tcl_dir.mkdir(parents=True, exist_ok=True)

    files = [
        (api_dir / "ids.inc", api_ids_inc_j2.TEMPLATE),
        (api_dir / "declarations.inc", api_declarations_inc_j2.TEMPLATE),
        (api_dir / "handle_fields.inc", api_handle_fields_inc_j2.TEMPLATE),
        (
            api_dir / "property_accessors_internal.inc",
            api_property_accessors_internal_inc_j2.TEMPLATE,
        ),
        (
            api_dir / "property_accessors_public.inc",
            api_property_accessors_public_inc_j2.TEMPLATE,
        ),
        (api_dir / "filter_tables.inc", api_filter_tables_inc_j2.TEMPLATE),
        (api_dir / "search.inc", api_search_inc_j2.TEMPLATE),
        (api_dir / "snapshot_appliers.hpp", api_snapshot_appliers_hpp_j2.TEMPLATE),
        (tcl_dir / "le_tcl_shim_generated.hpp", le_tcl_shim_generated_hpp_j2.TEMPLATE),
        (tcl_dir / "le_tcl_shim_generated.inc", le_tcl_shim_generated_inc_j2.TEMPLATE),
        (tcl_dir / "le_api_generated.i", le_api_generated_i_j2.TEMPLATE),
        (
            tcl_dir / "le_tcl_procs_generated.tcl",
            le_tcl_procs_generated_tcl_j2.TEMPLATE,
        ),
    ]
    for path, template_str in files:
        content = jinja2.Template(template_str).render(
            schema=schema,
            classes=classes,
            readable_classes=classes,
            current_access_classes=current_access_classes,
            search_scopes=search_scopes,
            render_of_check_cpp=render_of_check_cpp,
            render_default_scope_cpp=render_default_scope_cpp,
        )
        with open(path, "w") as f:
            f.write(content)

    return 0
