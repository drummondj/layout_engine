#pragma once
#include <expected>
#include <map>
#include "database/database_generated.h"
#include "types.h"
#include "include/core/SkPaint.h"

namespace le
{
    enum class LayerManagerError
    {
        DuplicateLayerName,
        MissingLayerName
    };

    /// @brief LayerManager contains a list of layers available for drawing along with their visiblity
    class LayerManager
    {
    public:
        /// @brief Add a layer with the specified name to the layer manager
        /// @param name The layer name
        /// @param outline_color The outline color in hex format, i.e. #aabbccff
        /// @param fill_color The fill color in hex format, i.e. #aabbccff
        /// @param zindex The position in the z-axis of the layer
        std::expected<void, LayerManagerError> add_layer(std::string name, std::string outline_color, int outline_width, std::string fill_color, int zindex);

        /// @brief Set visibility of the specified layer name
        /// @param layer_name The layer name
        /// @param visible Visible or not
        std::expected<void, LayerManagerError> set_visible(std::string layer_name, bool visible);

        /// @brief Returns visible layers in order from bottom to top
        /// @return List of layers
        LayerRefs get_visible_layers();

        /// @brief Returns visible layer name in order from bottom to top
        /// @return List of layers
        std::vector<std::string> get_visible_layer_names();

        /// @brief Returns the layer with the specified name
        /// @param layer_name The layer name
        /// @return A layer
        std::unique_ptr<database::LayerT> &get_layer(std::string layer_name);

        SkPaint get_fill_paint_for_layer(std::string layer_name);
        SkPaint get_stroke_paint_for_layer(std::string layer_name);

    private:
        LayerMap layers{};
        std::map<std::string, SkPaint> fill_paint{};
        std::map<std::string, SkPaint> stroke_paint{};
    };
}