"""Example of an EDA database schema"""

from codegen.schema import Schema, Klass, Field

schema = Schema(
    name="EDA",
    description="A schema for an EDA database",
    namespace="eda",
    version="0.0.1_test",
    classes=[
        Klass(
            name="Root",
            description="The root object that contains all EDA objects",
            fields=[
                Field(
                    name="designs",
                    description="The designs",
                    type="Design",
                    is_list=True,
                    is_child=True,
                ),
            ],
        ),
        Klass(
            name="Design",
            description="A design",
            fields=[
                Field(
                    name="root",
                    description="The root object",
                    type="Root",
                    parent="designs",
                ),
                Field(
                    name="name",
                    description="The name of the design",
                    type="str",
                    example="Design",
                ),
                Field(
                    name="modules",
                    description="The modules in the design",
                    type="Module",
                    is_child=True,
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="Module",
            description="A module",
            fields=[
                Field(
                    name="design",
                    description="The design object",
                    type="Design",
                    parent="modules",
                ),
                Field(
                    name="name",
                    description="The name of the module",
                    type="str",
                    example="Module",
                ),
                Field(
                    name="instances",
                    description="The instances in the module",
                    type="Instance",
                    is_child=True,
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="Instance",
            description="An instance",
            fields=[
                Field(
                    name="module",
                    description="The parent module object",
                    type="Module",
                    parent="instances",
                ),
                Field(
                    name="name",
                    description="The name of the instance",
                    type="str",
                    example="Instance",
                ),
                Field(
                    name="reference",
                    description="The reference to the module",
                    type="Module",
                    is_optional=True,
                ),
            ],
        ),
    ],
)
