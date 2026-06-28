#pragma once
#include "database/database_generated.h"
#include "geometry.h"
#include "coordinates.h"
#include "types.h"
#include <functional>
#include <vector>

namespace le
{
    class ShapeFilter
    {
    public:
        ShapeRefs filter(const ShapeRefs shapes, const database::RectT &viewport, const std::weak_ptr<Coordinates> coordinates);
    };
}