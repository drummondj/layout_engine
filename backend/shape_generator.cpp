#include <iostream>
#include "shape_generator.h"
#include "shape_factory.h"

namespace le
{
    std::vector<std::unique_ptr<database::ShapeT>> ShapeGenerator::generate(std::weak_ptr<database::DatabaseT> database)
    {
        // Test shapes for now with strange shapes to make sure transforms work
        std::vector<std::unique_ptr<database::ShapeT>> shapes;
        float dbu = 1000.0f;
        float strap_width = 1.0f * dbu;

        shapes.push_back(ShapeFactory::create_rect("prBoundary", -50 * dbu, -100 * dbu, 1050 * dbu, 1100 * dbu));

        for (int x = 0; x < 1000 * dbu; x += 2 * strap_width)
        {
            shapes.push_back(ShapeFactory::create_rect("M1", x - 0.5 * strap_width, x, x + 0.5 * strap_width, 1000 * dbu));
        }

        for (int y = 0; y < 1000 * dbu; y += 2 * strap_width)
        {
            shapes.push_back(ShapeFactory::create_rect("M2", y, y - 0.5 * strap_width, 1000 * dbu, y + 0.5 * strap_width));
        }

        std::cout << "Created " << shapes.size() << " shapes" << std::endl;
        return shapes;
    }
}