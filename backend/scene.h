#pragma once
#include <map>
#include <memory>
#include "database/database_generated.h"
#include "types.h"

namespace le
{

    /// @brief Holds shapes in local coordinates derivied from design data.
    class Scene
    {
    public:
        /// @brief adds multiple shapes to the scene
        /// @param shapes the shapes to add
        void add_shapes(std::vector<std::unique_ptr<database::ShapeT>> shapes);

        /// @brief adds a shape to the scene
        /// @param shape the shape to add
        void add_shape(std::unique_ptr<database::ShapeT> shape);

        /// @brief Remove all shapes from the scene
        void clear_shapes();

        /// @brief Returns the shapes for the specified layer name
        /// @param layer_name the name of the layer
        ShapeRefs get_shapes_on_layer(std::string layer_name);

        /// @brief Returns the shapes for the specified layer names
        /// @param layer_names the names of the layers
        ShapeRefs get_shapes_on_layers(std::vector<std::string> layer_names);

        /// @brief Calculate the bbox of all shapes in the scene
        /// @return A RectT pointer representing the bbox
        std::unique_ptr<database::RectT> bbox();

        std::vector<database::ShapeT *> all_shapes();

    private:
        ShapeMap shapes{};
    };
}