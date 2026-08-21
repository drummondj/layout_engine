from codegen.schema import Schema, Klass, Field

schema = Schema(
    name="ValidationFailures",
    description="A schema for validation failures",
    namespace="validation_failures",
    version="0.0.1_test",
    classes=[
        Klass(
            name="Root",
            description="The root object that contains all validation failures",
            fields=[
                Field(
                    name="incorrect_primitive",
                    description="A field with an incorrect primitive type",
                    type="string",
                ),
                Field(
                    name="incorrect_class",
                    description="A field with an incorrect class type",
                    type="InvalidClass",
                ),
                Field(
                    name="incorrect_example",
                    description="A field with an incorrect example",
                    type="str",
                    example=1,
                ),
                Field(
                    name="incorrect_default",
                    description="A field with an incorrect default",
                    type="str",
                    default=1,
                    example="default",
                ),
                Field(
                    name="incorrect_parent",
                    description="A field with an incorrect parent",
                    type="NoParent",
                    is_child=True,
                ),
            ],
        ),
        Klass(
            name="BadChild",
            description="A child class",
            fields=[
                Field(
                    name="root",
                    description="The root object",
                    type="Rootz",
                    parent="children",
                ),
                Field(
                    name="root2",
                    description="Another root object",
                    type="Root",
                    parent="childrenz",
                ),
            ],
        ),
        Klass(
            name="BadChild",
            description="A unique failure",
            fields=[
                Field(
                    name="name",
                    description="The name of the class",
                    type="str",
                    example="BadChild",
                ),
                Field(
                    name="name",
                    description="A unique field failure",
                    type="str",
                    example="BadChild",
                ),
            ],
        ),
        Klass(
            name="NoParent",
            description="A child klass with a parent reference",
            fields=[
                Field(
                    name="name",
                    description="The name",
                    type="str",
                ),
            ],
        ),
    ],
)
