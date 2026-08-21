"""A generator used to generate C++ based on a schema."""

from logging import Logger
from pathlib import Path
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
)

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
]


def generate(schema: Schema, output_dir: str, logger: Logger) -> int:
    """Generate C++ code based on the schema."""

    logger.info("Validating schema ...")
    errors = SchemaRuleSet().validate(schema)

    if len(errors) > 0:
        for error in errors:
            logger.error(error.message)
        return 1

    logger.info("Generating code ...")

    # Delete and fully recreate the directory, so a removed/renamed class
    # or field can't leave a stale generated file behind.
    shutil.rmtree(output_dir, ignore_errors=True)
    Path(output_dir).mkdir(parents=True, exist_ok=True)

    # Link the schema
    schema.link()
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

    # cmakelists_file = f"{output_dir}/CMakeLists.txt"
    # with open(cmakelists_file, "w") as f:
    #     f.write(cmakelists_template.render(schema=schema))

    # test_file = f"{output_dir}/test_{schema.namespace}.cpp"
    # with open(test_file, "w") as f:
    #     f.write(test_template.render(schema=schema))

    return 0
