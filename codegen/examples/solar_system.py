"""Example of a Solar System schema"""

from codegen.schema import Schema, Klass, Field

schema = Schema(
    name="Solar System",
    description="A schema for the Solar System",
    namespace="solar_system",
    version="0.0.1_test",
    classes=[
        Klass(
            name="Root",
            description="The root object that contains all solar system objects",
            fields=[
                Field(
                    name="solar_systems",
                    description="The solar systems",
                    type="SolarSystem",
                    is_list=True,
                    is_child=True,
                ),
            ],
        ),
        Klass(
            name="SolarSystem",
            description="A solar system",
            fields=[
                Field(
                    name="root",
                    description="The root object",
                    type="Root",
                    parent="solar_systems",
                ),
                Field(
                    name="name",
                    description="The name of the solar system",
                    type="str",
                    example="The Solar System",
                ),
                Field(
                    name="age",
                    description="The age of the solar system",
                    type="float",
                    default=0.00,
                    example=4.6,
                ),
                Field(
                    name="planets",
                    description="The planets in the solar system",
                    type="Planet",
                    is_child=True,
                    is_list=True,
                ),
                Field(
                    name="star",
                    description="The star in the solar system",
                    type="Star",
                    is_child=True,
                    is_list=False,
                ),
            ],
        ),
        Klass(
            name="Planet",
            description="A planet in the Solar System",
            fields=[
                Field(
                    name="solar_system",
                    description="The solar system the planet belongs to",
                    type="SolarSystem",
                    parent="planets",
                ),
                Field(
                    name="name",
                    description="The name of the planet",
                    type="str",
                    example="Earth",
                ),
                Field(
                    name="mass",
                    description="The mass of the planet",
                    type="float",
                    default=0.0,
                    example=5.972e24,
                ),
                Field(
                    name="radius",
                    description="The radius of the planet",
                    type="float",
                    default=0.0,
                    example=6371.0,
                ),
                Field(
                    name="star",
                    description="The star the planet orbits",
                    type="Star",
                    is_optional=True,
                ),
            ],
        ),
        Klass(
            name="Star",
            description="A star in the Solar System",
            fields=[
                Field(
                    name="solar_system",
                    description="The solar system the star belongs to",
                    type="SolarSystem",
                    parent="star",
                ),
                Field(
                    name="name",
                    description="The name of the star",
                    type="str",
                    example="The Sun",
                ),
                Field(
                    name="mass",
                    description="The mass of the star",
                    type="float",
                    default=0.0,
                    example=1.989e30,
                ),
                Field(
                    name="radius",
                    description="The radius of the star",
                    type="float",
                    default=0.0,
                    example=695700.0,
                ),
            ],
        ),
    ],
)
