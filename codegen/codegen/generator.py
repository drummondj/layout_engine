"""A generator used to generate C++ based on a schema."""

from logging import Logger
from pathlib import Path
from typing import List, Optional
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
from codegen import extension_schema
from codegen import migration as schema_migration
import json

from codegen.schema import Schema, Klass, Field
from importlib.machinery import SourceFileLoader

from codegen.validation import SchemaRuleSet


def schema_loader(schema: str) -> Schema:
    """Load the schema module."""

    module = SourceFileLoader("schema", schema).load_module()
    return module.schema


def _core_context(schema: Schema, history_dir: Optional[str], migrations_dir: Optional[str]) -> schema_migration.ExtensionChainContext:
    """What checking or drafting an extension's migrations needs from core."""
    descriptor = schema_descriptor.build_descriptor(schema)
    names = {k["name"] for k in descriptor["classes"]}
    if history_dir is not None:
        for snapshot in schema_descriptor.read_snapshots(Path(history_dir)).values():
            names |= {k["name"] for k in snapshot["descriptor"]["classes"]}
    core_migrations = schema_migration.load_migrations(Path(migrations_dir)) if migrations_dir else []
    return schema_migration.ExtensionChainContext(schema.version, core_migrations, frozenset(names))


def _aligned(core: schema_migration.ExtensionChainContext, snapshot: dict) -> dict:
    """An extension snapshot's descriptor with core classes renamed since it was taken followed."""
    return schema_migration.retarget(snapshot["descriptor"], schema_migration.core_renames_since(core.core_migrations, snapshot.get("core_version")))


HEADER_ONLY_CLASSES = [
    "ids",
    "pool",
    "property",
    "index",
    "root",
    "native_tables",
]


def make_migration(
    schema: Schema,
    history_dir: str,
    migrations_dir: str,
    name: str,
    logger: Logger,
    interactive: bool = True,
    extension: Optional[extension_schema.ExtensionSchema] = None,
) -> int:
    """
    Draft the migration from the newest earlier snapshot to the current
    schema, into the next numbered file in `migrations_dir` - or, with
    `extension`, that extension's next migration, into its own directory,
    recording the current core version as depends_on_core.
    """
    errors = SchemaRuleSet().validate(schema)
    if errors:
        for error in errors:
            logger.error(error.message)
        return 1
    schema.link()
    core = None
    if extension is not None:
        core = _core_context(schema, history_dir, migrations_dir)
        current = schema_descriptor.build_extension_descriptor(schema, extension.name, extension.version)
        history_dir, migrations_dir = str(extension.history_dir), str(extension.migrations_dir)
    else:
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

    before = _aligned(core, snapshots[previous]) if core is not None else snapshots[previous]["descriptor"]
    ops = schema_migration.draft_ops(before, current, ask)
    Path(migrations_dir).mkdir(parents=True, exist_ok=True)
    path = schema_migration.next_migration_path(Path(migrations_dir), name)
    path.write_text(
        schema_migration.render_migration(
            previous,
            current["version"],
            name.replace("_", " "),
            ops,
            extension=extension.name if extension is not None else None,
            depends_on_core=schema.version if extension is not None else None,
        )
    )
    todos = sum(isinstance(op, schema_migration.Todo) for op in ops)
    logger.info(f"Wrote {path} ({len(ops)} ops{f', {todos} TODO to resolve' if todos else ''}) - review it, then regenerate")
    return 0


def check_migrations_only(
    schema: Schema, history_dir: str, migrations_dir: str, logger: Logger, extensions: Optional[List[extension_schema.ExtensionSchema]] = None
) -> int:
    schema.link()
    errors = schema_migration.check_migrations(schema_descriptor.build_descriptor(schema), Path(history_dir), Path(migrations_dir))
    core = _core_context(schema, history_dir, migrations_dir)
    for ext in extensions or []:
        ext_descriptor = schema_descriptor.build_extension_descriptor(schema, ext.name, ext.version)
        errors += [
            f"extension {ext.name}: {e}"
            for e in schema_migration.check_extension_migrations(ext_descriptor, ext.history_dir, ext.migrations_dir, ext.name, ext.prefix, core)
        ]
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
    extensions: Optional[List[extension_schema.ExtensionSchema]] = None,
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

    # Each extension schema has its own history and migration chain, in its
    # own directory, checked the same way as core's.
    extension_info = []
    core = _core_context(schema, history_dir, migrations_dir)
    for ext in extensions or []:
        ext_descriptor = schema_descriptor.build_extension_descriptor(schema, ext.name, ext.version)
        if history_dir is not None:
            check = schema_descriptor.check_history(
                ext_descriptor,
                ext.history_dir,
                update_snapshot,
                bump_hint=f"Bump VERSION in {ext.schema_path}.",
                align=lambda snapshot: _aligned(core, snapshot),
            )
            errors = [f"extension {ext.name}: {e}" for e in check.errors]
            if not errors:
                if check.write:
                    path = schema_descriptor.write_snapshot(ext.history_dir, ext_descriptor, core_version=schema.version)
                    logger.info(f"extension {ext.name}: {check.note}: {path}")
                errors = [
                    f"extension {ext.name}: {e}"
                    for e in schema_migration.check_extension_migrations(ext_descriptor, ext.history_dir, ext.migrations_dir, ext.name, ext.prefix, core)
                ]
            if errors:
                for error in errors:
                    logger.error(error)
                return 1
        extension_info.append(
            {
                "name": ext.name,
                "package_version": ext.package_version,
                "version": ext.version,
                "fingerprint": schema_descriptor.fingerprint(ext_descriptor),
                "descriptor_json": schema_descriptor.descriptor_json(ext_descriptor),
            }
        )

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
    if any(f"){delimiter}" in text for text in [descriptor_json] + [e["descriptor_json"] for e in extension_info]):
        raise ValueError("Schema descriptor contains the raw-string delimiter")
    with open(f"{output_dir}/schema_version.hpp", "w") as f:
        f.write(
            jinja2.Template(schema_version_hpp_j2.TEMPLATE).render(
                schema=schema,
                version=schema.version,
                fingerprint=schema_descriptor.fingerprint(descriptor),
                descriptor_json=descriptor_json,
                delimiter=delimiter,
                extensions=extension_info,
            )
        )

    extension_chains = [(ext.name, schema_migration.load_migrations(ext.migrations_dir)) for ext in extensions or []]
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
            "extension": cpp(extension),
            "depends_on_core": cpp(depends_on_core),
        }
        for to_version, kind, klass, old, new, description, extension, depends_on_core in schema_migration.merged_runtime_table(
            core.core_migrations, schema.version, extension_chains
        )
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
