import sys

import click
import logging

from codegen import generator, tcl_generator

"""
CLI for the cmg package.

Example usage:

cmg --schema <path to schema file> --output <path to output directory>
cmg --schema <path to schema file> --output <backend src dir> --target tcl
"""


@click.command()
@click.option("-s", "--schema", required=True, help="Path to schema file")
@click.option("-o", "--output", required=True, help="Path to output directory")
@click.option(
    "-t",
    "--target",
    type=click.Choice(["database", "tcl"]),
    default="database",
    help="'database' (default): the object-pool database (structs/pools/root) into --output directly. "
    "'tcl' - the generated TCL/SWIG property-reading surface into {output}/api/generated_tcl and "
    "{output}/tcl/generated - point --output at the backend's src/ directory for this target.",
)
def cli(schema: str, output: str, target: str):
    """
    Generate code from a schema file.
    """
    logging.basicConfig(
        level=logging.INFO,
        format="%(levelname)s: %(message)s",
    )
    logger: logging.Logger = logging.getLogger("cmg")
    logger.info(f"Generating {target} code from schema {schema} to output directory {output}")
    if target == "tcl":
        exit_code = tcl_generator.generate(generator.schema_loader(schema), output, logger)
    else:
        exit_code = generator.generate(generator.schema_loader(schema), output, logger)
    if exit_code != 0:
        logger.error("Code generation failed.")
    else:
        logger.info("Code generation complete.")

    sys.exit(exit_code)


if __name__ == "__main__":
    cli()
