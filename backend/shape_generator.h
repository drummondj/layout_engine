#pragma once

#pragma once
#include "database/database_generated.h"
#include "geometry.h"
#include "types.h"
#include <functional>
#include <vector>

namespace le
{
    class ShapeGenerator
    {
    public:
        std::vector<std::unique_ptr<database::ShapeT>> generate(std::weak_ptr<database::DatabaseT> database);
    };
}