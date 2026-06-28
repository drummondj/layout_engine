#pragma once

#include <vector>
#include <functional>
#include "database/database_generated.h"

namespace le
{
    using ShapeRefs = std::vector<std::reference_wrapper<const database::ShapeT>>;
    using ShapeMap = std::map<std::string, std::vector<std::unique_ptr<database::ShapeT>>>;
    using LayerRefs = std::vector<std::reference_wrapper<const database::LayerT>>;
    using LayerMap = std::map<std::string, std::unique_ptr<database::LayerT>>;
}