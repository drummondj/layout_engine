"""Generates the TCL/SWIG property-reading and search surface (C API
fragments for src/api, SWIG/shim/procs files for src/tcl) from a
schema - a separate generation target from generator.py's database
codegen, invoked via `codegen --target tcl`.
"""

from logging import Logger
from pathlib import Path
import shutil

import jinja2

from codegen.templates.tcl import (
    api_declarations_inc_j2,
    api_edit_ops_decls_inc_j2,
    api_edit_ops_defs_inc_j2,
    api_edit_types_hpp_j2,
    api_extension_edit_decls_inc_j2,
    api_extension_edit_defs_inc_j2,
    api_extension_current_decls_inc_j2,
    api_extension_current_defs_inc_j2,
    api_filter_tables_inc_j2,
    api_handle_fields_inc_j2,
    api_id_conversions_hpp_j2,
    api_ids_inc_j2,
    api_object_dispatch_inc_j2,
    api_object_kinds_inc_j2,
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
    """Generate the TCL property-reading/search surface into `output_dir`'s
    api/ (C API fragments) and tcl/ (SWIG, shim and procs) subdirectories,
    each recreated from scratch."""

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

    api_dir = Path(output_dir) / "api"
    tcl_dir = Path(output_dir) / "tcl"
    shutil.rmtree(api_dir, ignore_errors=True)
    shutil.rmtree(tcl_dir, ignore_errors=True)
    api_dir.mkdir(parents=True, exist_ok=True)
    tcl_dir.mkdir(parents=True, exist_ok=True)

    files = [
        (api_dir / "ids.inc", api_ids_inc_j2.TEMPLATE),
        (api_dir / "id_conversions.hpp", api_id_conversions_hpp_j2.TEMPLATE, True),
        (api_dir / "extension_current_decls.inc", api_extension_current_decls_inc_j2.TEMPLATE, True),
        (api_dir / "extension_current_defs.inc", api_extension_current_defs_inc_j2.TEMPLATE, True),
        (api_dir / "edit_types.hpp", api_edit_types_hpp_j2.TEMPLATE, True),
        (api_dir / "edit_ops_decls.inc", api_edit_ops_decls_inc_j2.TEMPLATE, True),
        (api_dir / "edit_ops_defs.inc", api_edit_ops_defs_inc_j2.TEMPLATE, True),
        (api_dir / "extension_edit_decls.inc", api_extension_edit_decls_inc_j2.TEMPLATE, True),
        (api_dir / "extension_edit_defs.inc", api_extension_edit_defs_inc_j2.TEMPLATE, True),
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
        (api_dir / "object_kinds.inc", api_object_kinds_inc_j2.TEMPLATE, True),
        (api_dir / "object_dispatch.inc", api_object_dispatch_inc_j2.TEMPLATE, True),
        (tcl_dir / "le_tcl_shim_generated.hpp", le_tcl_shim_generated_hpp_j2.TEMPLATE),
        (tcl_dir / "le_tcl_shim_generated.inc", le_tcl_shim_generated_inc_j2.TEMPLATE),
        (tcl_dir / "le_api_generated.i", le_api_generated_i_j2.TEMPLATE),
        (
            tcl_dir / "le_tcl_procs_generated.tcl",
            le_tcl_procs_generated_tcl_j2.TEMPLATE,
        ),
    ]
    by_name = {k.name: k for k in classes}
    # Each class's parent fields paired with the parent class, for the
    # generic parent dispatch - only parents that are TCL-readable have a kind.
    parent_fields = {
        k.name: [(f, by_name[f.type]) for f in k.get_parent_fields() if f.type in by_name] for k in classes
    }
    for path, template_str, *trim in files:
        # The newer templates are written for trim_blocks/lstrip_blocks.
        trimmed = bool(trim and trim[0])
        content = jinja2.Template(template_str, trim_blocks=trimmed, lstrip_blocks=trimmed).render(
            parent_fields=parent_fields,
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
