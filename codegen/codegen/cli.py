import sys
from pathlib import Path

import click
import logging

from codegen import generator, render_generator, tcl_generator

"""
CLI for the codegen package (the `codegen` command).

Example usage:

codegen --schema <path to schema file> --output <path to output directory>
codegen --schema <path to schema file> --output <backend src dir> --target tcl
"""


@click.command()
@click.option("-s", "--schema", required=True, help="Path to schema file")
@click.option("-o", "--output", default=None, help="Path to output directory ('database'/'tcl' targets)")
@click.option(
    "-t",
    "--target",
    type=click.Choice(["database", "tcl", "render", "makemigration", "checkmigrations"]),
    default="database",
    help="'database' (default): the object-pool database (structs/pools/root) into --output directly. "
    "'tcl' - the generated TCL/SWIG property-reading surface into {output}/api and {output}/tcl. "
    "'render' - the renderer's purpose registry (view_layer_purpose.hpp) into --output directly. "
    "'makemigration' - draft the migration to the current schema version (needs --name). "
    "'checkmigrations' - only check the migration chain.",
)
@click.option(
    "--history",
    default=None,
    help="'database' target: schema-history snapshot directory (default: schema_history/ next to the "
    "schema file). A schema change without a version bump fails; a new version gets a snapshot.",
)
@click.option("--no-history", is_flag=True, help="'database' target: skip the schema-history check.")
@click.option(
    "--update-snapshot",
    is_flag=True,
    help="'database' target: overwrite the current version's snapshot - only for a version that has not "
    "been committed or released yet.",
)
@click.option(
    "--migrations",
    default=None,
    help="Migration directory (default: migrations/ next to the schema file).",
)
@click.option("--name", default=None, help="'makemigration' target: what changed, e.g. layer_kind_rename.")
@click.option("--non-interactive", is_flag=True, help="'makemigration' target: leave possible renames as TODOs instead of asking.")
def cli(
    schema: str,
    output: str | None,
    target: str,
    history: str | None,
    no_history: bool,
    update_snapshot: bool,
    migrations: str | None,
    name: str | None,
    non_interactive: bool,
):
    """
    Generate code from a schema file.
    """
    logging.basicConfig(
        level=logging.INFO,
        format="%(levelname)s: %(message)s",
    )
    logger: logging.Logger = logging.getLogger("codegen")
    logger.info(f"Generating {target} code from schema {schema} to output directory {output}")
    schema_dir = Path(schema).resolve().parent
    history_dir = history or str(schema_dir / "schema_history")
    migrations_dir = migrations or str(schema_dir / "migrations")
    if target in ("database", "tcl", "render") and not output:
        raise click.UsageError(f"--output is required for the '{target}' target")
    if target == "makemigration":
        if not name:
            raise click.UsageError("--name is required for makemigration")
        exit_code = generator.make_migration(
            generator.schema_loader(schema), history_dir, migrations_dir, name, logger, interactive=not non_interactive
        )
    elif target == "checkmigrations":
        exit_code = generator.check_migrations_only(generator.schema_loader(schema), history_dir, migrations_dir, logger)
    elif target == "tcl":
        exit_code = tcl_generator.generate(generator.schema_loader(schema), output, logger)
    elif target == "render":
        exit_code = render_generator.generate(generator.schema_loader(schema), output, logger)
    else:
        exit_code = generator.generate(
            generator.schema_loader(schema),
            output,
            logger,
            history_dir=None if no_history else history_dir,
            update_snapshot=update_snapshot,
            migrations_dir=migrations_dir,
        )
    if exit_code != 0:
        logger.error(f"codegen {target} failed.")
    elif target in ("database", "tcl", "render"):
        logger.info("Code generation complete.")

    sys.exit(exit_code)


if __name__ == "__main__":
    cli()
