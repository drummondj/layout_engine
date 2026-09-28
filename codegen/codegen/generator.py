"""A generator used to generate C++ based on a schema."""

from logging import Logger
from pathlib import Path
from typing import Optional
import shutil
import jinja2
from codegen.templates import (
    root_hpp_j2,
    struct_hpp_j2,
    index_hpp_j2,
    pool_hpp_j2,
    property_hpp_j2,
    enum_hpp_j2,
    ids_hpp_j2,
    schema_version_hpp_j2,
    native_tables_hpp_j2,
    migrations_hpp_j2,
)
from codegen import descriptor as schema_descriptor
from codegen import migration as schema_migration
import json

from codegen.schema import Schema, Klass, Field
from importlib.machinery import SourceFileLoader

from codegen.validation import SchemaRuleSet


def schema_loader(schema: str) -> Schema:
    """Load the schema module."""

    module = SourceFileLoader("schema", schema).load_module()
    return module.schema


HEADER_ONLY_CLASSES = [
    "ids",
    "pool",
    "property",
    "index",
    "root",
    "native_tables",
]


def make_migration(
    schema: Schema, history_dir: str, migrations_dir: str, name: str, logger: Logger, interactive: bool = True
) -> int:
    """
    Draft the migration from the newest earlier snapshot to the current
    schema, into the next numbered file in `migrations_dir`.
    """
    errors = SchemaRuleSet().validate(schema)
    if errors:
        for error in errors:
            logger.error(error.message)
        return 1
    schema.link()
    current = schema_descriptor.build_descriptor(schema)
    snapshots = schema_descriptor.read_snapshots(Path(history_dir))
    current_version = schema_descriptor.parse_version(current["version"])
    earlier = [v for v in snapshots if schema_descriptor.parse_version(v) < current_version]
    if not earlier:
        logger.error(f"no schema snapshot older than {current['version']} to migrate from - bump the schema version first")
        return 1
    previous = max(earlier, key=schema_descriptor.parse_version)
    for existing in schema_migration.load_migrations(Path(migrations_dir)):
        if existing.to_version == current["version"]:
            logger.error(f"{existing.label()} already migrates to {current['version']} - edit it, or delete it to redraft")
            return 1

    ask = None
    if interactive:
        import click

        def ask(kind, where, old, new):
            place = f" in {where}" if where else ""
            return click.confirm(f"Was {kind} '{old}'{place} renamed to '{new}'?", default=True)

    ops = schema_migration.draft_ops(snapshots[previous]["descriptor"], current, ask)
    Path(migrations_dir).mkdir(parents=True, exist_ok=True)
    path = schema_migration.next_migration_path(Path(migrations_dir), name)
    path.write_text(schema_migration.render_migration(previous, current["version"], name.replace("_", " "), ops))
    todos = sum(isinstance(op, schema_migration.Todo) for op in ops)
    logger.info(f"Wrote {path} ({len(ops)} ops{f', {todos} TODO to resolve' if todos else ''}) - review it, then regenerate")
    return 0


def check_migrations_only(schema: Schema, history_dir: str, migrations_dir: str, logger: Logger) -> int:
    schema.link()
    errors = schema_migration.check_migrations(schema_descriptor.build_descriptor(schema), Path(history_dir), Path(migrations_dir))
    for error in errors:
        logger.error(error)
    if not errors:
        logger.info("Migration chain OK")
    return 1 if errors else 0


def generate(
    schema: Schema,
    output_dir: str,
    logger: Logger,
    history_dir: Optional[str] = None,
    update_snapshot: bool = False,
    migrations_dir: Optional[str] = None,
) -> int:
    """
    Generate C++ code based on the schema.

    With `history_dir`, the schema is first checked against the snapshots
    there (descriptor.check_history) and a new snapshot is written when
    the version is new - a schema change without a version bump fails
    before anything is generated. Then the migration chain in
    `migrations_dir` (default: migrations/ beside `history_dir`) must take
    the oldest snapshot to the current schema (migration.check_migrations).
    """

    logger.info("Validating schema ...")
    errors = SchemaRuleSet().validate(schema)

    if len(errors) > 0:
        for error in errors:
            logger.error(error.message)
        return 1

    # Link the schema
    schema.link()

    descriptor = schema_descriptor.build_descriptor(schema)
    if history_dir is not None:
        check = schema_descriptor.check_history(descriptor, Path(history_dir), update_snapshot)
        if check.errors:
            for error in check.errors:
                logger.error(error)
            return 1
        if check.write:
            path = schema_descriptor.write_snapshot(Path(history_dir), descriptor)
            logger.info(f"{check.note}: {path}")
        if migrations_dir is None:
            migrations_dir = str(Path(history_dir).parent / "migrations")
        migration_errors = schema_migration.check_migrations(descriptor, Path(history_dir), Path(migrations_dir))
        if migration_errors:
            for error in migration_errors:
                logger.error(error)
            return 1

    logger.info("Generating code ...")

    # Delete and fully recreate the directory, so a removed/renamed class
    # or field can't leave a stale generated file behind.
    shutil.rmtree(output_dir, ignore_errors=True)
    Path(output_dir).mkdir(parents=True, exist_ok=True)

    schema.set_output_dir(output_dir)

    # Build the C++ code

    # Templates
    struct_hpp_template = jinja2.Template(struct_hpp_j2.TEMPLATE)
    enum_template = jinja2.Template(enum_hpp_j2.TEMPLATE)

    supplementary_templates = {
        "index.hpp": jinja2.Template(index_hpp_j2.TEMPLATE),
        "ids.hpp": jinja2.Template(ids_hpp_j2.TEMPLATE),
        "pool.hpp": jinja2.Template(pool_hpp_j2.TEMPLATE),
        "property.hpp": jinja2.Template(property_hpp_j2.TEMPLATE),
        "root.hpp": jinja2.Template(root_hpp_j2.TEMPLATE),
        "native_tables.hpp": jinja2.Template(native_tables_hpp_j2.TEMPLATE),
    }

    for klass in schema.get_classes_without_enums():
        hpp_file = f"{output_dir}/{klass.to_snake_case()}.hpp"
        with open(hpp_file, "w") as f:
            f.write(struct_hpp_template.render(schema=schema, klass=klass))

    for klass in schema.get_enums():
        hpp_file = f"{output_dir}/{klass.to_snake_case()}.hpp"
        with open(hpp_file, "w") as f:
            f.write(enum_template.render(schema=schema, klass=klass))

    for klass in HEADER_ONLY_CLASSES:
        header_file = f"{output_dir}/{klass}.hpp"
        with open(header_file, "w") as f:
            f.write(supplementary_templates[f"{klass}.hpp"].render(schema=schema))

    descriptor_json = schema_descriptor.descriptor_json(descriptor)
    delimiter = "LEDESC"
    if f"){delimiter}" in descriptor_json:
        raise ValueError("Schema descriptor contains the raw-string delimiter")
    with open(f"{output_dir}/schema_version.hpp", "w") as f:
        f.write(
            jinja2.Template(schema_version_hpp_j2.TEMPLATE).render(
                schema=schema,
                version=schema.version,
                fingerprint=schema_descriptor.fingerprint(descriptor),
                descriptor_json=descriptor_json,
                delimiter=delimiter,
            )
        )

    migrations = schema_migration.load_migrations(Path(migrations_dir)) if migrations_dir else []
    cpp = json.dumps  # a JSON string literal is a valid C++ one for these ASCII names
    ops = [
        {
            "to_version": cpp(to_version),
            "kind": {
                schema_migration.RUNTIME_RENAME_CLASS: "RenameClass",
                schema_migration.RUNTIME_RENAME_FIELD: "RenameField",
                schema_migration.RUNTIME_RENAME_ENUM_VALUE: "RenameEnumValue",
                schema_migration.RUNTIME_UNSUPPORTED: "Unsupported",
            }[kind],
            "klass": cpp(klass),
            "old": cpp(old),
            "new": cpp(new),
            "description": cpp(description),
        }
        for to_version, kind, klass, old, new, description in schema_migration.runtime_table(migrations)
    ]
    with open(f"{output_dir}/migrations.hpp", "w") as f:
        f.write(jinja2.Template(migrations_hpp_j2.TEMPLATE).render(schema=schema, ops=ops))

    # cmakelists_file = f"{output_dir}/CMakeLists.txt"
    # with open(cmakelists_file, "w") as f:
    #     f.write(cmakelists_template.render(schema=schema))

    # test_file = f"{output_dir}/test_{schema.namespace}.cpp"
    # with open(test_file, "w") as f:
    #     f.write(test_template.render(schema=schema))

    return 0
