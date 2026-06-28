#pragma once

#include "database/database_generated.h"

using namespace le::database;

namespace le
{
    class ShapeFactory
    {
    public:
        static std::unique_ptr<ShapeT> create_rect(std::string layer_name, int ll_x, int ll_y, int ur_x, int ur_y)
        {
            auto shape = create_rect(ll_x, ll_y, ur_x, ur_y);
            shape->layer_name = layer_name;

            return shape;
        }

        static std::unique_ptr<ShapeT> create_rect(int ll_x, int ll_y, int ur_x, int ur_y)
        {

            auto rect = std::make_unique<RectT>();
            rect->ll = std::make_unique<PointT>();
            rect->ll->x = ll_x;
            rect->ll->y = ll_y;
            rect->ur = std::make_unique<PointT>();
            rect->ur->x = ur_x;
            rect->ur->y = ur_y;

            auto shape = std::make_unique<ShapeT>();
            shape->rects.push_back(std::move(rect));

            return shape;
        }
    };
}