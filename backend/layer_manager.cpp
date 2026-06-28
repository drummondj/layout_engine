#include "layer_manager.h"
#include <charconv>

namespace le
{

    static float byte_to_float(unsigned v)
    {
        return static_cast<float>(v) / 255.0f;
    }

    SkColor4f hexToSkColor4f(std::string_view hex)
    {
        if (!hex.empty() && hex[0] == '#')
            hex.remove_prefix(1);
        if (hex.size() != 8)
            throw std::invalid_argument("expected #RRGGBBAA");

        auto parse_byte = [](std::string_view s) -> unsigned
        {
            unsigned v = 0;
            auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
            if (ec != std::errc{})
                throw std::invalid_argument("bad hex color");
            return v;
        };

        unsigned r = parse_byte(hex.substr(0, 2));
        unsigned g = parse_byte(hex.substr(2, 2));
        unsigned b = parse_byte(hex.substr(4, 2));
        unsigned a = parse_byte(hex.substr(6, 2));

        return SkColor4f{byte_to_float(r), byte_to_float(g), byte_to_float(b), byte_to_float(a)};
    }

    std::expected<void, LayerManagerError>
    LayerManager::add_layer(std::string name, std::string outline_color, int outline_width, std::string fill_color, int zindex)
    {
        // Check if layer name already exists
        if (layers.contains(name))
        {
            return std::unexpected(LayerManagerError::DuplicateLayerName);
        }
        auto layer = std::make_unique<database::LayerT>();
        layer->name = name;
        layer->outline_color = outline_color;
        layer->outline_width = outline_width;
        layer->fill_color = fill_color;
        layer->visible = true;
        layer->selectable = true;
        layer->zindex = zindex;

        // Create fill paint
        fill_paint[name] = SkPaint();
        fill_paint[name].setAntiAlias(false);
        fill_paint[name].setStyle(SkPaint::kFill_Style);
        fill_paint[name].setColor4f(hexToSkColor4f(layer->fill_color));

        stroke_paint[name] = SkPaint();
        stroke_paint[name].setAntiAlias(false);
        stroke_paint[name].setStyle(SkPaint::kStroke_Style);
        stroke_paint[name].setStrokeWidth(0.0f); // 0.0f will draw hair-line width
        stroke_paint[name].setColor4f(hexToSkColor4f(layer->outline_color));

        layers[name] = std::move(layer);

        return {};
    }

    std::expected<void, LayerManagerError> LayerManager::set_visible(std::string layer_name, bool visible)
    {
        if (!layers.contains(layer_name))
        {
            return std::unexpected(LayerManagerError::MissingLayerName);
        }

        layers[layer_name]->visible = visible;

        return {};
    }

    LayerRefs LayerManager::get_visible_layers()
    {
        LayerRefs visible_layers;
        for (const auto &[name, layer] : layers)
        {
            if (layer && layer->visible)
            {
                visible_layers.push_back(std::cref(*layer));
            }
        }

        std::sort(visible_layers.begin(), visible_layers.end(),
                  [](const auto &a, const auto &b)
                  {
                      return a.get().zindex < b.get().zindex;
                  });

        return visible_layers;
    }

    std::vector<std::string> LayerManager::get_visible_layer_names()
    {
        std::vector<std::string> layer_names;
        for (auto layer : get_visible_layers())
        {
            layer_names.push_back(layer.get().name);
        }

        return layer_names;
    }

    std::unique_ptr<database::LayerT> &LayerManager::get_layer(std::string layer_name)
    {
        return layers[layer_name];
    }

    SkPaint LayerManager::get_fill_paint_for_layer(std::string layer_name)
    {
        return fill_paint[layer_name];
    }

    SkPaint LayerManager::get_stroke_paint_for_layer(std::string layer_name)
    {
        return stroke_paint[layer_name];
    }

}
