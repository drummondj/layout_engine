#include "scene.h"
#include "geometry.h"

namespace le
{

    void Scene::add_shapes(std::vector<std::unique_ptr<database::ShapeT>> shapes)
    {
        for (auto &shape : shapes)
        {
            add_shape(std::move(shape));
        }
    }

    void Scene::add_shape(std::unique_ptr<database::ShapeT> shape)
    {
        shapes[shape->layer_name].push_back(std::move(shape));
    }

    void Scene::clear_shapes()
    {
        shapes.clear();
    }

    ShapeRefs Scene::get_shapes_on_layer(std::string layer_name)
    {
        if (!shapes.contains(layer_name))
        {
            return {};
        }
        ShapeRefs shapes_on_layer;

        for (const auto &shape : shapes[layer_name])
        {
            if (shape)
            {
                shapes_on_layer.push_back(std::cref(*shape));
            }
        }
        return shapes_on_layer;
    }

    ShapeRefs Scene::get_shapes_on_layers(std::vector<std::string> layer_names)
    {
        ShapeRefs shapes_on_layers;
        for (auto layer_name : layer_names)
        {
            ShapeRefs current_layer_shapes = get_shapes_on_layer(layer_name);
            shapes_on_layers.insert(shapes_on_layers.end(), current_layer_shapes.begin(), current_layer_shapes.end());
        }
        return shapes_on_layers;
    }

    std::vector<database::ShapeT *> Scene::all_shapes()
    {
        std::vector<database::ShapeT *> combined_result;

        // 1. Calculate the exact total capacity required
        size_t total_elements = 0;
        for (const auto &[key, vec] : shapes)
        {
            total_elements += vec.size();
        }

        // 2. Pre-allocate all memory at once (Stops all multi-step reallocations)
        combined_result.reserve(total_elements);

        // 3. Perform bulk range insertions (Block copies)
        for (const auto &[key, vec] : shapes)
        {
            for (const auto &item : vec)
            {
                // Take the address of the actual element sitting inside the vector
                database::ShapeT *shape = item.get();
                combined_result.push_back(const_cast<database::ShapeT *>(shape));
            }
        }

        return combined_result;
    }

    std::unique_ptr<database::RectT> Scene::bbox()
    {
        return Geometry::bbox(all_shapes());
    }

}