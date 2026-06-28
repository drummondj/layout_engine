#include <chrono>
#include <iostream>
#include "pipeline.h"

namespace le
{
    Pipeline::Pipeline(
        std::weak_ptr<database::DatabaseT> database,
        std::weak_ptr<LayerManager> layer_manager,
        std::weak_ptr<Coordinates> coordinates,
        std::weak_ptr<Scene> scene)
    {
        this->database = database;
        this->layer_manager = layer_manager;
        this->coordinates = coordinates;
        this->scene = scene;
        shape_generator = std::make_unique<ShapeGenerator>();
        shape_filter = std::make_unique<ShapeFilter>();

        // Convert database to shapes
        auto shapes = shape_generator->generate(database);

        // Add shapes to scene
        scene.lock()->clear_shapes();
        scene.lock()->add_shapes(std::move(shapes));
    }

    std::map<std::string, ShapeRefs> Pipeline::run()
    {
        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

        // Select shapes based on layer
        auto visible_layers = layer_manager.lock()->get_visible_layer_names();

        // Filter shapes per layer (threading candidate)
        std::map<std::string, ShapeRefs> filtered_shapes;

        if (auto locked_viewport = viewport.lock())
        {
            for (auto layer_name : visible_layers)
            {
                auto shapes_on_layer = scene.lock()->get_shapes_on_layer(layer_name);
                auto filtered_shapes_on_layer = shape_filter->filter(shapes_on_layer, *locked_viewport, coordinates);
                filtered_shapes[layer_name] = filtered_shapes_on_layer;
            }
        }
        std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

        // std::cout << "Pipeline speed = " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() << "[ms]" << std::endl;

        first_run = false;
        return filtered_shapes;
    }

    void Pipeline::set_viewport(std::weak_ptr<database::RectT> viewport)
    {
        this->viewport = viewport;
    }

    bool Pipeline::did_run()
    {
        return !first_run;
    }

}