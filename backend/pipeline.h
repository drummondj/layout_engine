#pragma once
#include <memory>
#include <map>
#include "scene.h"
#include "shape_filter.h"
#include "shape_generator.h"
#include "layer_manager.h"
#include "coordinates.h"
#include "database/database_generated.h"

namespace le
{
    /// @brief  The Pipeline class is responsible for passing data between the various stages
    class Pipeline
    {
    public:
        Pipeline(
            std::weak_ptr<database::DatabaseT> database,
            std::weak_ptr<LayerManager> layer_manager,
            std::weak_ptr<Coordinates> coordinates,
            std::weak_ptr<Scene> scene);

        std::map<std::string, ShapeRefs> run();

        void set_viewport(std::weak_ptr<database::RectT> viewport);

        bool did_run();

    private:
        std::weak_ptr<database::DatabaseT> database;
        std::weak_ptr<LayerManager> layer_manager;
        std::weak_ptr<Scene> scene;
        std::unique_ptr<ShapeGenerator> shape_generator;
        std::unique_ptr<ShapeFilter> shape_filter;
        std::weak_ptr<database::RectT> viewport;
        std::weak_ptr<Coordinates> coordinates;
        bool first_run = true;
    };
}